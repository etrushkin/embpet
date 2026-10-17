/* Test-only build (CONFIG_EMBPET_LED_TEST, tools/led_test.py), never in production.
 *
 * The LED pin is INPUT_OUTPUT (led.c) and an any-edge ISR timestamps its edges, so the
 * pattern the pin really shows can be measured on the chip without anyone watching.
 *  - led t_edges: drain recorded edges [[level,t_us],...], with the current pin level, the
 *    polarity and the esp_timer task stack headroom.
 *  - led t_boot / led t_conf <0|1>: set the booting / configured flag of the shared state
 *    (booting is over before a console exists; configured has no CLI to clear it). */
#include "led_priv.h"

#include <stdint.h>

#include "argtable3/argtable3.h"
#include "console.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_intr_alloc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "state.h"

#define EDGE_RING 64                    /* power of two */

typedef struct {
    int64_t t_us;
    uint8_t level;
} edge_t;

static edge_t s_ring[EDGE_RING];
static volatile uint32_t s_head;        /* written by the ISR only */
static volatile uint32_t s_tail;        /* written by the console task only */
static volatile uint32_t s_dropped;

/* In IRAM: the GPIO ISR service is IRAM-safe (io_in.c), so every handler on it must be. */
static void IRAM_ATTR edge_isr(void *arg)
{
    (void)arg;
    uint32_t h = s_head;
    if (h - s_tail >= EDGE_RING) {
        s_dropped++;
        return;
    }
    edge_t *e = &s_ring[h & (EDGE_RING - 1)];
    e->t_us = esp_timer_get_time();
    e->level = (uint8_t)gpio_get_level((gpio_num_t)CONFIG_EMBPET_LED_GPIO);
    s_head = h + 1;
}

static void t_edges(console_req_t *req)
{
    cJSON *p = cJSON_CreateObject();
    cJSON *edges = cJSON_CreateArray();
    if (p == NULL || edges == NULL) {
        cJSON_Delete(p);
        cJSON_Delete(edges);
        console_err(req, CONSOLE_ERR_INTERNAL, "out of memory");
        return;
    }
    uint32_t head = s_head;
    for (uint32_t t = s_tail; t != head; t++) {
        const edge_t *e = &s_ring[t & (EDGE_RING - 1)];
        cJSON *a = cJSON_CreateArray();
        if (a == NULL) {
            break;
        }
        cJSON_AddItemToArray(a, cJSON_CreateNumber(e->level));
        cJSON_AddItemToArray(a, cJSON_CreateNumber((double)e->t_us));
        cJSON_AddItemToArray(edges, a);
    }
    s_tail = head;
    TaskHandle_t h = xTaskGetHandle("esp_timer");
    cJSON_AddNumberToObject(p, "level", gpio_get_level((gpio_num_t)CONFIG_EMBPET_LED_GPIO));
    cJSON_AddNumberToObject(p, "now_us", (double)esp_timer_get_time());
    cJSON_AddBoolToObject(p, "active_low",
#ifdef CONFIG_EMBPET_LED_ACTIVE_LOW
                          true
#else
                          false
#endif
    );
    cJSON_AddNumberToObject(p, "dropped", (double)s_dropped);
    cJSON_AddNumberToObject(p, "timer_stack_free_min",
                            h != NULL ? (double)uxTaskGetStackHighWaterMark(h) : -1.0);
    cJSON_AddItemToObject(p, "edges", edges);
    console_ok(req, p);
}

static const console_cmd_t s_t_edges = {
    .name = "led t_edges",
    .help = "TEST BUILD: drain recorded LED pin edges [[level,t_us],...], level, polarity",
    .handler = t_edges,
};

static struct arg_int *s_boot_arg;
static struct arg_int *s_conf_arg;
static void *s_boot_args[2];
static void *s_conf_args[2];

static void t_boot(console_req_t *req)
{
    state_set_booting(s_boot_arg->ival[0] != 0);
    console_ok(req, NULL);
}

static void t_conf(console_req_t *req)
{
    state_set_configured(s_conf_arg->ival[0] != 0);
    console_ok(req, NULL);
}

static const console_cmd_t s_t_boot = {
    .name = "led t_boot",
    .help = "TEST BUILD: set the state's booting flag (0 or 1)",
    .args = s_boot_args,
    .handler = t_boot,
};

static const console_cmd_t s_t_conf = {
    .name = "led t_conf",
    .help = "TEST BUILD: set the state's configured flag (0 or 1)",
    .args = s_conf_args,
    .handler = t_conf,
};

void led_test_register(void)
{
    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    ESP_ERROR_CHECK(gpio_set_intr_type((gpio_num_t)CONFIG_EMBPET_LED_GPIO, GPIO_INTR_ANYEDGE));
    ESP_ERROR_CHECK(gpio_isr_handler_add((gpio_num_t)CONFIG_EMBPET_LED_GPIO, edge_isr, NULL));
    s_boot_arg = arg_int1(NULL, NULL, "<0|1>", "booting");
    s_conf_arg = arg_int1(NULL, NULL, "<0|1>", "configured");
    if (s_boot_arg == NULL || s_conf_arg == NULL) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    s_boot_args[0] = s_boot_arg;
    s_conf_args[0] = s_conf_arg;
    ESP_ERROR_CHECK(console_register(&s_t_edges));
    ESP_ERROR_CHECK(console_register(&s_t_boot));
    ESP_ERROR_CHECK(console_register(&s_t_conf));
}
