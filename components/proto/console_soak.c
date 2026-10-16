/* Console soak generator (CONFIG_EMBPET_CONSOLE_SOAK, test builds only). Every 100 ms:
 * one "soak" event and one WARN log line. Each event carries the longest proto_evt() call,
 * ESP_LOGW() call and loop period seen since uptime SETTLE_S, so after the host port was
 * closed a reconnecting host can tell from `evt since` whether output ever stalled the
 * firmware. Before SETTLE_S the Wi-Fi driver task (priority 23) stalls this task for about
 * 0.3 s at a time while it computes the WPA3-SAE handshake (README 3.1, embpet-s2j.5); the
 * maxima of that window are kept apart as boot_max_*, and `settled` says whether the window
 * is over. */
#include <inttypes.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "console_priv.h"
#include "proto.h"

#define SETTLE_S 15

static const char *TAG = "soak";

typedef struct {
    int64_t evt_us;
    int64_t log_us;
    int64_t period_us;
} soak_max_t;

static void soak_task(void *arg)
{
    (void)arg;
    uint32_t n = 0;
    soak_max_t boot = {0};
    soak_max_t run = {0};
    int64_t last = esp_timer_get_time();
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(100));
        int64_t t0 = esp_timer_get_time();
        bool settled = t0 >= SETTLE_S * 1000000LL;
        soak_max_t *m = settled ? &run : &boot;
        if (t0 - last > m->period_us) {
            m->period_us = t0 - last;
        }
        last = t0;
        n++;

        cJSON *f = cJSON_CreateObject();
        if (f != NULL) {
            cJSON_AddNumberToObject(f, "n", n);
            cJSON_AddBoolToObject(f, "settled", settled);
            cJSON_AddNumberToObject(f, "max_evt_us", (double)run.evt_us);
            cJSON_AddNumberToObject(f, "max_log_us", (double)run.log_us);
            cJSON_AddNumberToObject(f, "max_period_us", (double)run.period_us);
            cJSON_AddNumberToObject(f, "boot_max_evt_us", (double)boot.evt_us);
            cJSON_AddNumberToObject(f, "boot_max_log_us", (double)boot.log_us);
            cJSON_AddNumberToObject(f, "boot_max_period_us", (double)boot.period_us);
        }
        proto_evt("soak", f);
        int64_t t1 = esp_timer_get_time();
        ESP_LOGW(TAG, "soak line %" PRIu32 " padding padding padding padding padding", n);
        int64_t t2 = esp_timer_get_time();
        if (t1 - t0 > m->evt_us) {
            m->evt_us = t1 - t0;
        }
        if (t2 - t1 > m->log_us) {
            m->log_us = t2 - t1;
        }
    }
}

void console_soak_start(void)
{
#ifdef CONFIG_EMBPET_BOOT_TRACE
    boot_trace_start();
#endif
    xTaskCreate(soak_task, "soak", 3072, NULL, 5, NULL);
}
