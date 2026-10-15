/* Console commands: wifi set, wifi status, wifi txpower (README 3.4). */
#include "cmds.h"

#include <string.h>

#include "argtable3/argtable3.h"
#include "cfg.h"
#include "console.h"
#include "state.h"
#include "wifi.h"
#include "wifi_core.h"

/* ---- wifi set "<ssid>" ["<pass>"] ---- */

static struct arg_str *s_set_ssid;
static struct arg_str *s_set_pass;
static void *s_set_args[3];

/* Stores the credentials; the wifi module reconnects from the cfg change callback. The
 * res never carries the values, and nothing here logs them. */
static void wifi_set(console_req_t *req)
{
    const char *ssid = s_set_ssid->sval[0];
    const char *pass = s_set_pass->count > 0 ? s_set_pass->sval[0] : "";
    if (!cfg_ssid_valid(ssid)) {
        console_err(req, CONSOLE_ERR_BAD_ARGS, "ssid must be 1..32 bytes");
        return;
    }
    if (!cfg_pass_valid(pass)) {
        console_err(req, CONSOLE_ERR_BAD_ARGS,
                    "password must be empty, 8..63 bytes or 64 hex digits");
        return;
    }
    esp_err_t err = cfg_set_wifi(ssid, pass);
    if (err != ESP_OK) {
        console_err(req, CONSOLE_ERR_INTERNAL, "cannot store credentials");
        return;
    }
    console_ok(req, NULL);
}

static const console_cmd_t s_wifi_set = {
    .name = "wifi set",
    .help = "Store Wi-Fi credentials and reconnect (open network: omit the password)",
    .args = s_set_args,
    .handler = wifi_set,
};

/* ---- wifi status ---- */

static void add_txpower(cJSON *p)
{
    cJSON_AddNumberToObject(p, "txpower", cfg_get_wifi_txq() / 4.0);
}

static void wifi_status(console_req_t *req)
{
    state_snapshot_t snap;
    state_get(&snap);
    char ssid[CFG_SSID_MAX + 1];
    if (cfg_get_wifi_ssid(ssid, sizeof(ssid)) != ESP_OK) {
        ssid[0] = '\0';
    }
    const char *st = !snap.configured ? "unconfigured"
                     : snap.wifi.state == STATE_WIFI_UP ? "up" : "down";

    cJSON *p = cJSON_CreateObject();
    if (p == NULL) {
        console_err(req, CONSOLE_ERR_INTERNAL, "out of memory");
        return;
    }
    cJSON_AddStringToObject(p, "state", st);
    cJSON_AddStringToObject(p, "ssid", ssid);
    if (snap.wifi.state == STATE_WIFI_UP) {
        int8_t rssi = snap.wifi.rssi;
        wifi_get_rssi(&rssi);
        cJSON_AddStringToObject(p, "ip", snap.wifi.ip);
        cJSON_AddNumberToObject(p, "rssi", rssi);
    }
    add_txpower(p);
    console_ok(req, p);
}

static const console_cmd_t s_wifi_status = {
    .name = "wifi status",
    .help = "State (up/down/unconfigured), ssid, ip, rssi, TX power; never the password",
    .handler = wifi_status,
};

/* ---- wifi txpower <dBm> ---- */

static struct arg_str *s_tx_arg;
static void *s_tx_args[2];

static void wifi_txpower(console_req_t *req)
{
    int q;
    if (!wifi_parse_txpower(s_tx_arg->sval[0], &q)) {
        console_err(req, CONSOLE_ERR_BAD_ARGS, "TX power must be 2..20 dBm in 0.25 steps");
        return;
    }
    /* The wifi module applies it to the driver from the cfg change callback. */
    if (cfg_set_wifi_txq(q) != ESP_OK) {
        console_err(req, CONSOLE_ERR_INTERNAL, "cannot store TX power");
        return;
    }
    cJSON *p = cJSON_CreateObject();
    if (p == NULL) {
        console_err(req, CONSOLE_ERR_INTERNAL, "out of memory");
        return;
    }
    add_txpower(p);
    console_ok(req, p);
}

static const console_cmd_t s_wifi_txpower = {
    .name = "wifi txpower",
    .help = "Set Wi-Fi TX power, 2..20 dBm in 0.25 steps (default 8.5)",
    .args = s_tx_args,
    .handler = wifi_txpower,
};

void cmd_wifi_register(void)
{
    s_set_ssid = arg_str1(NULL, NULL, "<ssid>", "network name, 1..32 bytes");
    s_set_pass = arg_str0(NULL, NULL, "<pass>", "passphrase; omit for an open network");
    s_tx_arg = arg_str1(NULL, NULL, "<dBm>", "2..20 in 0.25 steps");
    if (s_set_ssid == NULL || s_set_pass == NULL || s_tx_arg == NULL) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    s_set_args[0] = s_set_ssid;
    s_set_args[1] = s_set_pass;
    s_tx_args[0] = s_tx_arg;

    ESP_ERROR_CHECK(console_register(&s_wifi_set));
    ESP_ERROR_CHECK(console_register(&s_wifi_status));
    ESP_ERROR_CHECK(console_register(&s_wifi_txpower));
}
