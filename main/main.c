#include "cfg.h"
#include "cmds.h"
#include "console.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "io.h"
#include "proto.h"
#include "state.h"
#include "tgapi.h"
#include "wifi.h"

static const char *TAG = "embpet";

/* configured = Wi-Fi credentials present (state.h). */
static void update_configured(void)
{
    char ssid[CFG_SSID_MAX + 1];
    state_set_configured(cfg_get_wifi_ssid(ssid, sizeof(ssid)) == ESP_OK && ssid[0] != '\0');
}

static void on_cfg_change(cfg_key_t key, void *ctx)
{
    (void)ctx;
    if (key == CFG_KEY_WIFI_SSID) {
        update_configured();
    }
}

void app_main(void)
{
    /* PLAN 1.2 init order: outputs latched off first, then NVS and cfg (boot levels
     * applied right after), then proto. */
    io_outputs_init();
    ESP_LOGI(TAG, "embpet %s booted", esp_app_get_description()->version);

    esp_err_t cfg_err = cfg_init();
    if (cfg_err != ESP_OK) {
        /* getters fall back to Kconfig defaults; setters return ESP_ERR_INVALID_STATE */
        ESP_LOGE(TAG, "cfg_init failed: %s", esp_err_to_name(cfg_err));
    }
    io_outputs_apply_boot();

    proto_init();
    state_init();
    proto_emit_boot();
    io_inputs_init();                       /* step 4: events need proto */

    /* Console REPL (step 5). */
    cmd_sys_register();
    cmd_wifi_register();
    io_register_cmds();
    ESP_ERROR_CHECK(console_start());

    /* Step 8: configured from cfg, booting done, runtime log level WARN. */
    ESP_ERROR_CHECK(cfg_subscribe(on_cfg_change, NULL));
    update_configured();
    esp_err_t wifi_err = wifi_start();
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_start failed: %s", esp_err_to_name(wifi_err));
    }
    /* Step 7: Telegram state and `tg` event (tasks follow in the bridge). */
    esp_err_t tg_err = tgapi_init();
    if (tg_err != ESP_OK) {
        ESP_LOGE(TAG, "tgapi_init failed: %s", esp_err_to_name(tg_err));
    }
    state_set_booting(false);
    cmd_sys_set_log_level(ESP_LOG_WARN);
}
