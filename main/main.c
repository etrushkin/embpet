#include "esp_app_desc.h"
#include "esp_log.h"
#include "cfg.h"
#include "proto.h"
#include "state.h"

static const char *TAG = "embpet";

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
}
