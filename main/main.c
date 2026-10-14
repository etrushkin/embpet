#include "esp_app_desc.h"
#include "esp_log.h"

static const char *TAG = "embpet";

void app_main(void)
{
    ESP_LOGI(TAG, "embpet %s booted", esp_app_get_description()->version);
}
