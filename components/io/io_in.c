/* Inputs GPIO10/20/21 behind PC817 optos: opto on = pin low = active (README 2.3).
 *
 * The any-edge ISR only counts the edge and (re)arms the pin's debounce esp_timer, no more
 * often than once per ms so an edge storm cannot keep the CPU in timer bookkeeping (it keeps
 * counting). The debounce timer is dispatched from the timer interrupt (ESP_TIMER_ISR), so it
 * expires on time even while a higher-priority task (the Wi-Fi driver during WPA3-SAE) holds
 * the CPU: the pin is sampled and debounce_core decides whether it is a change. A change is
 * queued, and a task-dispatched timer turns the queue into `input` events in the esp_timer
 * task, in order; the event can still be late. If the queue is full, the pin's decision is
 * not taken but retried once the queue has drained, so `active` always matches the last
 * event and the edges count into the next `extra`, as without the queue.
 * Debounce 0 arms the timer for one storm window (1 ms): the sample happens about 1 ms
 * after the first edge, and edges inside that window are folded into extra.
 *
 * IRAM: the GPIO ISR service is installed with ESP_INTR_FLAG_IRAM, and the esp_timer
 * interrupt has it as well (CONFIG_ESP_TIMER_IN_IRAM, forced on for the C3), so both run
 * while the flash cache is off (NVS writes). Edges are therefore counted and the debounce
 * window re-armed during flash writes too, and the debounce timer never samples a level
 * whose edges the ISR has not seen. Everything both ISR paths call is in IRAM: in_isr, arm
 * and debounce_cb here, deb_settle (linker.lf), gpio_get_level
 * (CONFIG_GPIO_CTRL_FUNC_IN_IRAM), the esp_timer and FreeRTOS FromISR calls and the atomics
 * (IDF keeps them there). The state they touch is in DRAM. */
#include "io.h"

#include <stdatomic.h>
#include <stdint.h>

#include "cJSON.h"
#include "cfg.h"
#include "debounce_core.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_intr_alloc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "proto.h"
#include "sdkconfig.h"

#define STORM_REARM_US 1000             /* ISR re-arms the timer at most this often */
#define EVT_QUEUE_LEN 32                /* decisions waiting for the esp_timer task */

typedef struct {
    int pin;
    esp_timer_handle_t timer;
    atomic_uint raw;                    /* raw edges, written by the ISR */
    volatile uint32_t deb_us;           /* debounce time, read by the ISR */
    volatile int64_t armed_us;          /* ISR: time of the last (re)arm */
    deb_t d;                            /* guarded by s_lock */
    bool retry;                         /* guarded by s_lock: decision skipped, queue full */
} in_pin_t;

typedef struct {
    uint8_t pin;
    uint8_t active;
    uint32_t extra;
} in_evt_t;

static in_pin_t s_in[IO_IN_COUNT] = {{.pin = 10}, {.pin = 20}, {.pin = 21}};
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_evtq;
static esp_timer_handle_t s_evt_timer;  /* task-dispatched: drains s_evtq */

int io_in_index(int pin)
{
    for (int k = 0; k < IO_IN_COUNT; k++) {
        if (s_in[k].pin == pin) {
            return k;
        }
    }
    return -1;
}

static void IRAM_ATTR arm(in_pin_t *p, uint32_t us)
{
    if (esp_timer_restart(p->timer, us) != ESP_OK) {        /* not running yet */
        esp_timer_start_once(p->timer, us);
    }
}

static void IRAM_ATTR in_isr(void *arg)
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

static void IRAM_ATTR debounce_cb(void *arg)
{
    in_pin_t *p = arg;
    /* Count first, then sample: an edge in between is then counted by the next event
     * instead of being attributed to this one with a stale level. */
    uint32_t raw = atomic_load_explicit(&p->raw, memory_order_relaxed);
    in_evt_t e = {.pin = (uint8_t)p->pin, .active = gpio_get_level((gpio_num_t)p->pin) == 0};
    /* Only this interrupt fills the queue, so room seen here is still there below. */
    bool full = xQueueIsQueueFullFromISR(s_evtq) != pdFALSE;
    bool changed = false;
    portENTER_CRITICAL_ISR(&s_lock);
    if (full) {
        p->retry = true;
    } else {
        changed = deb_settle(&p->d, e.active, raw, &e.extra);
    }
    portEXIT_CRITICAL_ISR(&s_lock);
    if (changed) {
        /* No yield request: right only because no task blocks on s_evtq (evt_cb polls). */
        xQueueSendFromISR(s_evtq, &e, NULL);
    }
    if (changed || full) {
        /* Armed and not yet run: INVALID_STATE, fine. While evt_cb runs the timer is
         * disarmed, so this succeeds and the drain runs once more. */
        esp_timer_start_once(s_evt_timer, 0);
    }
}

/* esp_timer task: emit the queued decisions in order, then retry the pins whose decision
 * found the queue full. */
static void evt_cb(void *arg)
{
    (void)arg;
    in_evt_t e;
    while (xQueueReceive(s_evtq, &e, 0) == pdTRUE) {
        cJSON *f = cJSON_CreateObject();
        if (f == NULL) {
            continue;                                   /* out of memory: event lost */
        }
        cJSON_AddNumberToObject(f, "pin", e.pin);
        cJSON_AddNumberToObject(f, "active", e.active);
        cJSON_AddNumberToObject(f, "extra", (double)e.extra);
        proto_evt("input", f);
    }
    for (int k = 0; k < IO_IN_COUNT; k++) {
        in_pin_t *p = &s_in[k];
        portENTER_CRITICAL(&s_lock);
        bool retry = p->retry;
        p->retry = false;
        portEXIT_CRITICAL(&s_lock);
        if (retry) {
            /* Sample now. An edge since the skipped decision has re-armed the timer for a
             * full debounce time (then INVALID_STATE here), so an idle timer means the level
             * has been stable for at least that long. */
            esp_timer_start_once(p->timer, 0);
        }
    }
}

void io_inputs_init(void)
{
    s_evtq = xQueueCreate(EVT_QUEUE_LEN, sizeof(in_evt_t));
    ESP_ERROR_CHECK(s_evtq != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    const esp_timer_create_args_t ea = {
        .callback = evt_cb,
        .name = "input_evt",
    };
    ESP_ERROR_CHECK(esp_timer_create(&ea, &s_evt_timer));
    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {    /* already installed: fine */
        ESP_ERROR_CHECK(err);
    }
    for (int k = 0; k < IO_IN_COUNT; k++) {
        in_pin_t *p = &s_in[k];
        const esp_timer_create_args_t a = {
            .callback = debounce_cb,
            .arg = p,
            .dispatch_method = ESP_TIMER_ISR,
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
    portENTER_CRITICAL(&s_lock);
    st->active = p->d.active;
    st->extra = deb_pending(&p->d, raw);
    portEXIT_CRITICAL(&s_lock);
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
