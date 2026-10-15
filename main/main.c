#include "cfg.h"
#include "cmds.h"
#include "console.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "proto.h"
#include "state.h"

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
    ESP_LOGI(TAG, "embpet %s booted", esp_app_get_description()->version);

    /* PLAN 1.2 init order: outputs, then NVS and cfg, then proto. */
    esp_err_t cfg_err = cfg_init();
    if (cfg_err != ESP_OK) {
        /* getters fall back to Kconfig defaults; setters return ESP_ERR_INVALID_STATE */
        ESP_LOGE(TAG, "cfg_init failed: %s", esp_err_to_name(cfg_err));
    }

    proto_init();
    state_init();
    proto_emit_boot();

    /* Console REPL (step 5). */
    cmd_sys_register();
    ESP_ERROR_CHECK(console_start());

    /* Step 8: configured from cfg, booting done, runtime log level WARN. */
    ESP_ERROR_CHECK(cfg_subscribe(on_cfg_change, NULL));
    update_configured();
    state_set_booting(false);
    cmd_sys_set_log_level(ESP_LOG_WARN);
}
