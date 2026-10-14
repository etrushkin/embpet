/* Device stress test for the frame writer (embpet-kzd acceptance 2). Compiled only with
 * CONFIG_EMBPET_PROTO_STRESS=y. Two tasks each emit STRESS_EVENTS events while
 * ESP_LOGW spams from both of them; the host checks every "@@" line with
 * `tools/embpet_cli.py --check-frames SEC --reset`. */
#include "sdkconfig.h"

#ifdef CONFIG_EMBPET_PROTO_STRESS

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "proto.h"

#define STRESS_EVENTS      1000
#define STRESS_START_DELAY pdMS_TO_TICKS(3000)   /* let the host open the port */

static const char *TAG = "stress";

static void stress_task(void *arg)
{
    int id = (int)(intptr_t)arg;
    vTaskDelay(STRESS_START_DELAY);
    for (int i = 0; i < STRESS_EVENTS; i++) {
        cJSON *f = cJSON_CreateObject();
        if (f != NULL) {
            cJSON_AddNumberToObject(f, "task", id);
            cJSON_AddNumberToObject(f, "i", i);
        }
        proto_evt("stress", f);
        ESP_LOGW(TAG, "task %d spam %d with \"@@{quotes}\" and padding xxxxxxxxxxxxxxxxxxxxxxxx", id, i);
        if ((i % 16) == 15) {
            vTaskDelay(1);                       /* let lower-priority tasks and USB run */
        }
    }
    cJSON *done = cJSON_CreateObject();
    if (done != NULL) {
        cJSON_AddNumberToObject(done, "task", id);
    }
    proto_evt("stress_done", done);
    vTaskDelete(NULL);
}

void proto_stress_start(void)
{
    xTaskCreate(stress_task, "stress0", 4096, (void *)(intptr_t)0, 5, NULL);
    xTaskCreate(stress_task, "stress1", 4096, (void *)(intptr_t)1, 5, NULL);
}

#endif /* CONFIG_EMBPET_PROTO_STRESS */
