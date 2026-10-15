/* Console soak generator (CONFIG_EMBPET_CONSOLE_SOAK, test builds only). Every 100 ms:
 * one "soak" event and one WARN log line. Each event carries the longest proto_evt() call,
 * ESP_LOGW() call and loop period seen since boot, so after the host port was closed a
 * reconnecting host can tell from `evt since` whether output ever stalled the firmware. */
#include <inttypes.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "console_priv.h"
#include "proto.h"

static const char *TAG = "soak";

static void soak_task(void *arg)
{
    (void)arg;
    uint32_t n = 0;
    int64_t max_evt_us = 0;
    int64_t max_log_us = 0;
    int64_t max_period_us = 0;
    int64_t last = esp_timer_get_time();
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(100));
        int64_t t0 = esp_timer_get_time();
        if (t0 - last > max_period_us) {
            max_period_us = t0 - last;
        }
        last = t0;
        n++;

        cJSON *f = cJSON_CreateObject();
        if (f != NULL) {
            cJSON_AddNumberToObject(f, "n", n);
            cJSON_AddNumberToObject(f, "max_evt_us", (double)max_evt_us);
            cJSON_AddNumberToObject(f, "max_log_us", (double)max_log_us);
            cJSON_AddNumberToObject(f, "max_period_us", (double)max_period_us);
        }
        proto_evt("soak", f);
        int64_t t1 = esp_timer_get_time();
        ESP_LOGW(TAG, "soak line %" PRIu32 " padding padding padding padding padding", n);
        int64_t t2 = esp_timer_get_time();
        if (t1 - t0 > max_evt_us) {
            max_evt_us = t1 - t0;
        }
        if (t2 - t1 > max_log_us) {
            max_log_us = t2 - t1;
        }
    }
}

void console_soak_start(void)
{
    xTaskCreate(soak_task, "soak", 3072, NULL, 5, NULL);
}
