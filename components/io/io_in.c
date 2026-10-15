/* Inputs GPIO10/20/21 behind PC817 optos: opto on = pin low = active (README 2.3).
 *
 * The any-edge ISR only counts the edge and (re)arms the pin's debounce esp_timer, no more
 * often than once per ms so an edge storm cannot keep the CPU in timer bookkeeping (it keeps
 * counting). When the timer expires, in the esp_timer task, the pin is sampled and
 * debounce_core decides whether it is a change; a change becomes an `input` event.
 * Debounce 0 arms the timer for one storm window (1 ms): the sample happens about 1 ms
 * after the first edge, and edges inside that window are folded into extra.
 *
 * The ISR is not in IRAM: while flash is being written (NVS) edges are held back and merged
 * by the GPIO interrupt latch, so such a burst may be counted as fewer edges. The level
 * sampled at expiry is always right. */
#include "io.h"

#include <stdatomic.h>
#include <stdint.h>

#include "cJSON.h"
#include "cfg.h"
#include "debounce_core.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "proto.h"
#include "sdkconfig.h"

#define STORM_REARM_US 1000             /* ISR re-arms the timer at most this often */

typedef struct {
    int pin;
    esp_timer_handle_t timer;
    atomic_uint raw;                    /* raw edges, written by the ISR */
    volatile uint32_t deb_us;           /* debounce time, read by the ISR */
    volatile int64_t armed_us;          /* ISR: time of the last (re)arm */
    deb_t d;                            /* guarded by s_lock */
} in_pin_t;

static in_pin_t s_in[IO_IN_COUNT] = {{.pin = 10}, {.pin = 20}, {.pin = 21}};
static SemaphoreHandle_t s_lock;

int io_in_index(int pin)
{
    for (int k = 0; k < IO_IN_COUNT; k++) {
        if (s_in[k].pin == pin) {
            return k;
        }
    }
    return -1;
}

static void arm(in_pin_t *p, uint32_t us)
{
    if (esp_timer_restart(p->timer, us) != ESP_OK) {        /* not running yet */
        esp_timer_start_once(p->timer, us);
    }
}

static void in_isr(void *arg)
{
    in_pin_t *p = arg;
    atomic_fetch_add_explicit(&p->raw, 1, memory_order_relaxed);
    int64_t now = esp_timer_get_time();
    if (esp_timer_is_active(p->timer) && now - p->armed_us < STORM_REARM_US) {
        return;
    }
    p->armed_us = now;
    /* Debounce 0 still waits one storm window: at most 1000 events/s per pin, and bounces
     * inside that window fold into extra. */
    uint32_t us = p->deb_us;
    arm(p, us != 0 ? us : STORM_REARM_US);
}

static void debounce_cb(void *arg)
{
    in_pin_t *p = arg;
    /* Count first, then sample: an edge in between is then counted by the next event
     * instead of being attributed to this one with a stale level. */
    uint32_t raw = atomic_load_explicit(&p->raw, memory_order_relaxed);
    bool active = gpio_get_level((gpio_num_t)p->pin) == 0;
    uint32_t extra = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = deb_settle(&p->d, active, raw, &extra);
    xSemaphoreGive(s_lock);
    if (!changed) {
        return;
    }
    cJSON *f = cJSON_CreateObject();
    if (f == NULL) {
        return;                                         /* out of memory: event lost */
    }
    cJSON_AddNumberToObject(f, "pin", p->pin);
    cJSON_AddNumberToObject(f, "active", active ? 1 : 0);
    cJSON_AddNumberToObject(f, "extra", (double)extra);
    proto_evt("input", f);
}

void io_inputs_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {    /* already installed: fine */
        ESP_ERROR_CHECK(err);
    }
    for (int k = 0; k < IO_IN_COUNT; k++) {
        in_pin_t *p = &s_in[k];
        const esp_timer_create_args_t a = {
            .callback = debounce_cb,
            .arg = p,
            .name = "debounce",
        };
        ESP_ERROR_CHECK(esp_timer_create(&a, &p->timer));
        p->deb_us = (uint32_t)cfg_get_debounce(p->pin) * 1000u;
#ifdef CONFIG_EMBPET_IO_TEST
        /* Open-drain with the pull-up: the test drives the pin low itself, like a jumper
         * to GND, and releases it to let the pull-up take it high. */
        gpio_set_level((gpio_num_t)p->pin, 1);
        const gpio_mode_t mode = GPIO_MODE_INPUT_OUTPUT_OD;
#else
        const gpio_mode_t mode = GPIO_MODE_INPUT;
#endif
        const gpio_config_t c = {
            .pin_bit_mask = 1ULL << p->pin,
            .mode = mode,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_ANYEDGE,
        };
        ESP_ERROR_CHECK(gpio_config(&c));
        deb_init(&p->d, gpio_get_level((gpio_num_t)p->pin) == 0, 0);
        ESP_ERROR_CHECK(gpio_isr_handler_add((gpio_num_t)p->pin, in_isr, p));
    }
}

void io_in_get(int idx, io_in_state_t *st)
{
    in_pin_t *p = &s_in[idx];
    st->pin = p->pin;
    st->level = gpio_get_level((gpio_num_t)p->pin);
    uint32_t raw = atomic_load_explicit(&p->raw, memory_order_relaxed);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    st->active = p->d.active;
    st->extra = deb_pending(&p->d, raw);
    xSemaphoreGive(s_lock);
}

uint32_t io_in_debounce_ms(int idx)
{
    return s_in[idx].deb_us / 1000u;
}

esp_err_t io_in_set_debounce(int idx, int ms)
{
    esp_err_t err = cfg_set_debounce(s_in[idx].pin, ms);
    if (err == ESP_OK) {
        s_in[idx].deb_us = (uint32_t)ms * 1000u;
    }
    return err;
}
