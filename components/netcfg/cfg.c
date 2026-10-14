#include "cfg.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define NS "embpet"
#define KEY_VER "ver"

static const char *TAG = "cfg";

static SemaphoreHandle_t s_lock;
static nvs_handle_t s_nvs;
static bool s_open;

static struct {
    cfg_change_cb_t cb;
    void *ctx;
} s_subs[CFG_MAX_SUBSCRIBERS];
static size_t s_nsubs;

static bool lock(void)
{
    return s_lock && s_open && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE;
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

/* Call with the lock NOT held. */
static void notify(cfg_key_t key)
{
    typeof(s_subs) subs;
    size_t n;

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    n = s_nsubs;
    memcpy(subs, s_subs, sizeof(subs));
    unlock();

    for (size_t i = 0; i < n; i++) {
        subs[i].cb(key, subs[i].ctx);
    }
}

esp_err_t cfg_init(void)
{
    if (s_open) {
        return ESP_OK;
    }
    /* Created before NVS init so cfg_subscribe() also works in degraded mode (NVS failed). */
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition unusable (%s), erasing", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_open(NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t ver = 0;
    esp_err_t verr = nvs_get_u32(s_nvs, KEY_VER, &ver);
    if (verr != ESP_OK && verr != ESP_ERR_NVS_NOT_FOUND) {
        /* wrong type or unreadable: treat as a foreign version */
        ver = UINT32_MAX;
        verr = ESP_OK;
    }
    switch (cfg_schema_decide(verr == ESP_OK, ver)) {
    case CFG_VER_KEEP:
        break;
    case CFG_VER_ERASE:
        ESP_LOGW(TAG, "schema version %u != %u, erasing config", (unsigned)ver, (unsigned)CFG_SCHEMA_VERSION);
        err = nvs_erase_all(s_nvs);
        if (err != ESP_OK) {
            break;
        }
        /* fall through */
    case CFG_VER_STAMP:
        err = nvs_set_u32(s_nvs, KEY_VER, CFG_SCHEMA_VERSION);
        if (err == ESP_OK) {
            err = nvs_commit(s_nvs);
        }
        break;
    }
    if (err != ESP_OK) {
        nvs_close(s_nvs);
        return err;
    }

    s_open = true;
    return ESP_OK;
}

esp_err_t cfg_subscribe(cfg_change_cb_t cb, void *ctx)
{
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;   /* cfg_init() has not been called */
    }
    esp_err_t err = ESP_ERR_NO_MEM;
    if (s_nsubs < CFG_MAX_SUBSCRIBERS) {
        s_subs[s_nsubs].cb = cb;
        s_subs[s_nsubs].ctx = ctx;
        s_nsubs++;
        err = ESP_OK;
    }
    unlock();
    return err;
}

static esp_err_t get_str(cfg_key_t key, char *buf, size_t cap)
{
    if (!buf || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';
    if (!lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t len = cap;
    esp_err_t err = nvs_get_blob(s_nvs, cfg_key_name(key), buf, &len);
    unlock();
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_ERR_NOT_FOUND;
    } else if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        err = ESP_ERR_INVALID_SIZE;
    } else if (err == ESP_OK && (len == 0 || memchr(buf, '\0', len) != buf + len - 1)) {
        err = ESP_ERR_INVALID_STATE;   /* corrupt: empty, unterminated or embedded NUL */
    }
    if (err != ESP_OK) {
        buf[0] = '\0';
    }
    return err;
}

esp_err_t cfg_get_wifi_ssid(char *buf, size_t cap) { return get_str(CFG_KEY_WIFI_SSID, buf, cap); }
esp_err_t cfg_get_wifi_pass(char *buf, size_t cap) { return get_str(CFG_KEY_WIFI_PASS, buf, cap); }
esp_err_t cfg_get_tg_token(char *buf, size_t cap) { return get_str(CFG_KEY_TG_TOKEN, buf, cap); }

int8_t cfg_get_wifi_txq(void)
{
    int8_t v = 0;
    if (lock()) {
        esp_err_t err = nvs_get_i8(s_nvs, cfg_key_name(CFG_KEY_WIFI_TXQ), &v);
        unlock();
        if (err == ESP_OK) {
            return cfg_txq_clamp(v);
        }
    }
    return CONFIG_EMBPET_WIFI_TXPOWER_QDBM;
}

int64_t cfg_get_tg_owner(void)
{
    int64_t v = 0;
    if (lock()) {
        esp_err_t err = nvs_get_i64(s_nvs, cfg_key_name(CFG_KEY_TG_OWNER), &v);
        unlock();
        if (err == ESP_OK) {
            return v;
        }
    }
    return 0;
}

uint8_t cfg_get_out_boot(void)
{
    uint8_t v = 0;
    if (lock()) {
        esp_err_t err = nvs_get_u8(s_nvs, cfg_key_name(CFG_KEY_OUT_BOOT), &v);
        unlock();
        if (err == ESP_OK) {
            return v;
        }
    }
    return 0;
}

uint16_t cfg_get_debounce(int pin)
{
    cfg_key_t key;
    uint16_t v = 0;
    if (cfg_debounce_key(pin, &key) && lock()) {
        esp_err_t err = nvs_get_u16(s_nvs, cfg_key_name(key), &v);
        unlock();
        if (err == ESP_OK) {
            return cfg_debounce_clamp(v);
        }
    }
    return CONFIG_EMBPET_DEBOUNCE_DEFAULT_MS;
}

/* Commit a pending nvs_set/erase (err is its result); call with the lock held. Setters
 * unlock before notify() so callbacks never run under the lock. */
static esp_err_t commit_locked(cfg_key_t key, esp_err_t err)
{
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write of %s failed: %s", cfg_key_name(key), esp_err_to_name(err));
    }
    return err;
}

/* Secrets (SSID, password, token) are stored as NUL-terminated BLOBS, never with
 * nvs_set_str(): IDF's nvs_set_str() does ESP_LOGD("%s %s %s", __func__, key, value),
 * which would print the value when the log level is DEBUG. nvs_set_blob() and
 * nvs_get_blob() log only the key and the length. Call with the lock held. */
static esp_err_t put_secret(cfg_key_t key, const char *val)
{
    return nvs_set_blob(s_nvs, cfg_key_name(key), val, strlen(val) + 1);
}

static esp_err_t set_str(cfg_key_t key, const char *val)
{
    if (!lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = commit_locked(key, put_secret(key, val));
    unlock();
    if (err == ESP_OK) {
        notify(key);
    }
    return err;
}

static esp_err_t erase_key(cfg_key_t key)
{
    if (!lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = nvs_erase_key(s_nvs, cfg_key_name(key));
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    err = commit_locked(key, err);
    unlock();
    if (err == ESP_OK) {
        notify(key);
    }
    return err;
}

esp_err_t cfg_set_wifi(const char *ssid, const char *pass)
{
    if (!cfg_ssid_valid(ssid) || !cfg_pass_valid(pass)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = put_secret(CFG_KEY_WIFI_SSID, ssid);
    if (err == ESP_OK) {
        err = put_secret(CFG_KEY_WIFI_PASS, pass);
    }
    err = commit_locked(CFG_KEY_WIFI_SSID, err);   /* one commit for both keys */
    unlock();
    if (err == ESP_OK) {
        notify(CFG_KEY_WIFI_SSID);   /* single "credentials changed" notification */
    }
    return err;
}

esp_err_t cfg_set_wifi_txq(int qdbm)
{
    if (!cfg_txq_valid(qdbm)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = commit_locked(CFG_KEY_WIFI_TXQ,
                                  nvs_set_i8(s_nvs, cfg_key_name(CFG_KEY_WIFI_TXQ), (int8_t)qdbm));
    unlock();
    if (err == ESP_OK) {
        notify(CFG_KEY_WIFI_TXQ);
    }
    return err;
}

esp_err_t cfg_set_tg_token(const char *token)
{
    return cfg_token_valid(token) ? set_str(CFG_KEY_TG_TOKEN, token) : ESP_ERR_INVALID_ARG;
}

esp_err_t cfg_clear_tg_token(void)
{
    return erase_key(CFG_KEY_TG_TOKEN);
}

esp_err_t cfg_set_tg_owner(int64_t chat_id)
{
    if (!cfg_owner_valid(chat_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = commit_locked(CFG_KEY_TG_OWNER,
                                  nvs_set_i64(s_nvs, cfg_key_name(CFG_KEY_TG_OWNER), chat_id));
    unlock();
    if (err == ESP_OK) {
        notify(CFG_KEY_TG_OWNER);
    }
    return err;
}

esp_err_t cfg_clear_tg_owner(void)
{
    return erase_key(CFG_KEY_TG_OWNER);
}

esp_err_t cfg_set_out_boot(uint8_t mask)
{
    if (!lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = commit_locked(CFG_KEY_OUT_BOOT,
                                  nvs_set_u8(s_nvs, cfg_key_name(CFG_KEY_OUT_BOOT), mask));
    unlock();
    if (err == ESP_OK) {
        notify(CFG_KEY_OUT_BOOT);
    }
    return err;
}

esp_err_t cfg_set_debounce(int pin, int ms)
{
    cfg_key_t key;
    if (!cfg_debounce_key(pin, &key) || !cfg_debounce_valid(ms)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = commit_locked(key, nvs_set_u16(s_nvs, cfg_key_name(key), (uint16_t)ms));
    unlock();
    if (err == ESP_OK) {
        notify(key);
    }
    return err;
}
