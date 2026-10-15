#include "wifi.h"

#include <string.h>

#include "cfg.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/platform_util.h"
#include "proto.h"
#include "state.h"
#include "wifi_core.h"

static const char *TAG = "wifi_sta";

/* s_lock guards the connection-control fields below. state_set_wifi() and proto_evt() are
 * never called with it held (they can block on the frame writer). esp_wifi_set_config() is
 * the one blocking driver call made under it: it blocks only on the Wi-Fi task's own lock,
 * never on the event loop, so it cannot deadlock with the handlers that take s_lock. */
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_retry_timer;
static bool s_started;
static bool s_have_creds;       /* an SSID is stored */
static bool s_cfg_dirty;        /* stored credentials not yet in the driver */
static bool s_sta_connected;    /* associated (L2), maybe without an IP yet */
static bool s_connecting;       /* esp_wifi_connect() accepted, no CONNECTED/DISCONNECTED yet */
static bool s_reconfig;         /* disconnect requested by new credentials: reconnect at once */
static unsigned s_attempt;      /* consecutive failed attempts, indexes the backoff */

/* Event emission follows the state.h subscriber rule. */
static SemaphoreHandle_t s_emit_lock;
static uint32_t s_emit_gen;
static state_wifi_state_t s_emit_state = STATE_WIFI_DOWN;
static char s_emit_ip[STATE_IP_LEN];

static void on_state(const state_snapshot_t *snap, uint32_t changed, void *ctx)
{
    (void)changed;
    (void)ctx;
    xSemaphoreTake(s_emit_lock, portMAX_DELAY);
    if (snap->gen > s_emit_gen) {
        s_emit_gen = snap->gen;
        if (snap->wifi.state != s_emit_state || strcmp(snap->wifi.ip, s_emit_ip) != 0) {
            cJSON *f = cJSON_CreateObject();
            if (f != NULL) {
                /* Remember what was emitted only if the event could be built, so a later
                 * snapshot still reports the change after an out-of-memory. */
                s_emit_state = snap->wifi.state;
                memcpy(s_emit_ip, snap->wifi.ip, sizeof(s_emit_ip));
                bool up = snap->wifi.state == STATE_WIFI_UP;
                cJSON_AddStringToObject(f, "state", up ? "up" : "down");
                if (up) {
                    cJSON_AddStringToObject(f, "ip", snap->wifi.ip);
                    cJSON_AddNumberToObject(f, "rssi", snap->wifi.rssi);
                }
                proto_evt("wifi", f);
            }
        }
    }
    xSemaphoreGive(s_emit_lock);
}

bool wifi_get_rssi(int8_t *rssi)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return false;
    }
    *rssi = ap.rssi;
    return true;
}

/* Read the stored credentials into the driver config (RAM only); sets s_have_creds and
 * s_cfg_dirty. Call with s_lock held. */
static void load_creds_locked(void)
{
    char ssid[CFG_SSID_MAX + 1];
    char pass[CFG_PASS_MAX + 1];
    wifi_config_t wc = {0};

    bool have = cfg_get_wifi_ssid(ssid, sizeof(ssid)) == ESP_OK && ssid[0] != '\0';
    s_have_creds = have;
    s_cfg_dirty = false;
    if (have) {
        if (cfg_get_wifi_pass(pass, sizeof(pass)) != ESP_OK) {
            pass[0] = '\0';
        }
        /* ssid and password are not NUL-terminated when full (32 / 64 bytes). */
        memcpy(wc.sta.ssid, ssid, strlen(ssid));
        memcpy(wc.sta.password, pass, strlen(pass));
        wc.sta.threshold.authmode = pass[0] != '\0' ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
        wc.sta.pmf_cfg.capable = true;
        esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
        if (err == ESP_ERR_WIFI_STATE) {
            /* The driver refuses a new config while a connect is in progress. The caller
             * has asked for a disconnect; connect_locked() retries once it is idle. */
            s_cfg_dirty = true;
        } else if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_config: %s", esp_err_to_name(err));
            s_have_creds = false;
        }
    }
    mbedtls_platform_zeroize(&wc, sizeof(wc));
    mbedtls_platform_zeroize(pass, sizeof(pass));
    mbedtls_platform_zeroize(ssid, sizeof(ssid));
}

/* Connect with the stored credentials; no-op without an SSID. If the driver is still busy
 * with an old attempt (dirty config) do nothing: the pending DISCONNECTED event or the
 * fallback timer calls this again. */
static void connect_locked(void)
{
    if (s_cfg_dirty) {
        load_creds_locked();
        if (s_cfg_dirty) {
            return;
        }
    }
    if (!s_have_creds) {
        return;
    }
    esp_err_t err = esp_wifi_connect();
    s_connecting = err == ESP_OK;
    /* ESP_ERR_WIFI_CONN: an attempt is already running, nothing to do. */
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
    }
}

static void retry_cb(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_reconfig = false;
    if (!s_sta_connected && !s_connecting) {
        connect_locked();
    }
    xSemaphoreGive(s_lock);
}

static void apply_txpower(void)
{
    int8_t q = cfg_get_wifi_txq();
    esp_err_t err = esp_wifi_set_max_tx_power(q);
    int8_t got = 0;
    esp_wifi_get_max_tx_power(&got);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_max_tx_power(%d): %s", q, esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "tx power %d qdbm requested, driver reports %d (%s)", q, got,
             esp_err_to_name(err));
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    switch (id) {
    case WIFI_EVENT_STA_START:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        connect_locked();
        xSemaphoreGive(s_lock);
        break;
    case WIFI_EVENT_STA_CONNECTED: {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_sta_connected = true;
        s_connecting = false;
        /* Associated with the OLD credentials while new ones are waiting: drop the link,
         * the DISCONNECTED handler reconnects with the new ones. */
        bool stale = s_cfg_dirty;
        if (stale) {
            s_reconfig = true;
        }
        xSemaphoreGive(s_lock);
        if (stale) {
            esp_wifi_disconnect();
        }
        break;
    }
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *d = data;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_sta_connected = false;
        s_connecting = false;
        if (s_reconfig) {
            s_reconfig = false;
            connect_locked();
        } else if (s_have_creds) {
            uint32_t ms = wifi_backoff_ms(s_attempt);
            if (s_attempt < 32) {
                s_attempt++;
            }
            esp_timer_stop(s_retry_timer);
            esp_timer_start_once(s_retry_timer, (uint64_t)ms * 1000);
            ESP_LOGI(TAG, "disconnected (reason %d), retry in %u ms", d->reason, (unsigned)ms);
        }
        xSemaphoreGive(s_lock);
        state_set_wifi(STATE_WIFI_DOWN, NULL, 0);
        break;
    }
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        char ip[STATE_IP_LEN];
        esp_ip4addr_ntoa(&e->ip_info.ip, ip, sizeof(ip));
        int8_t rssi = 0;
        wifi_get_rssi(&rssi);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_attempt = 0;
        xSemaphoreGive(s_lock);
        apply_txpower();    /* an AP's country IE may have changed the limit */
        state_set_wifi(STATE_WIFI_UP, ip, rssi);
    } else if (id == IP_EVENT_STA_LOST_IP) {
        state_set_wifi(STATE_WIFI_DOWN, NULL, 0);
    }
}

/* cfg change callback: runs on the caller's task (the console task for `wifi set`). */
static void on_cfg_change(cfg_key_t key, void *ctx)
{
    (void)ctx;
    if (key == CFG_KEY_WIFI_TXQ) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool started = s_started;
        xSemaphoreGive(s_lock);
        if (started) {
            apply_txpower();
        }
        return;
    }
    if (key != CFG_KEY_WIFI_SSID) {     /* "credentials changed" (cfg.h) */
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    load_creds_locked();
    esp_timer_stop(s_retry_timer);
    s_attempt = 0;
    bool started = s_started;
    if (started) {
        s_reconfig = true;
        s_connecting = false;       /* aborting: the timer must not wait for a dead attempt */
    }
    xSemaphoreGive(s_lock);
    if (started) {
        /* Drop the current link or connect attempt (the driver rejects a new config while
         * connecting), then reconnect from the DISCONNECTED handler. If the driver was
         * idle there is no such event, so the timer reconnects after a short delay. Both
         * calls are made without s_lock: esp_wifi_disconnect() blocks on the Wi-Fi task,
         * which blocks when it posts to a full event queue, and the event-loop task
         * (which drains it) may be waiting for s_lock. Calling it from the event-loop
         * task itself is fine. A failed esp_timer_start_once() because a backoff timer is
         * already running is harmless: that timer connects too. */
        esp_wifi_disconnect();
        esp_timer_start_once(s_retry_timer, 500 * 1000);
    }
}

esp_err_t wifi_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_emit_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL || s_emit_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (!state_subscribe(on_state, NULL)) {
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t targs = {.callback = retry_cb, .name = "wifi_retry"};
    esp_err_t err = esp_timer_create(&targs, &s_retry_timer);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {    /* INVALID_STATE: already created */
        return err;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&icfg);
    if (err != ESP_OK) {
        return err;
    }
    /* cfg is the single credential store; without this the driver also writes the
     * password to nvs.net80211. */
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event, NULL);
    }
    if (err == ESP_OK) {
        err = cfg_subscribe(on_cfg_change, NULL);
    }
    if (err != ESP_OK) {
        return err;
    }

    /* s_started is set before the driver starts so a `wifi set` in this window still
     * reconnects (via STA_START or the fallback timer); an early esp_wifi_disconnect() then
     * fails with ESP_ERR_WIFI_NOT_STARTED, which is ignored. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    load_creds_locked();
    s_started = true;
    xSemaphoreGive(s_lock);

    err = esp_wifi_start();         /* STA_START connects if an SSID is stored */
    if (err != ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_started = false;
        xSemaphoreGive(s_lock);
        return err;
    }
    apply_txpower();                /* the TX power can only be set after esp_wifi_start() */
    return ESP_OK;
}
