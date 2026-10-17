/* Test-only build (CONFIG_EMBPET_IO_TEST, tools/io_test.py), never in production.
 *
 *  - io t_edges: the outputs are INPUT_OUTPUT (io_out.c) and an any-edge ISR timestamps
 *    their edges with esp_timer_get_time, so a pulse length can be measured on the chip.
 *  - io t_drive: the inputs are open-drain with the pull-up (io_in.c); this drives one low
 *    and releases it in a timed pattern, which stands in for a jumper to GND.
 *  - io t_emit / io t_stat: a periodic esp_timer callback emits proto_evt, and the esp_timer
 *    task stack high-water mark is reported (embpet-kzd review note). */
#include "io_priv.h"

#include <stdint.h>
#include <stdlib.h>

#include "argtable3/argtable3.h"
#include "console.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_intr_alloc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "io.h"
#include "proto.h"
#include "sdkconfig.h"

/* ------------------------------------------------------------ output edges */

#define EDGE_RING 128                   /* power of two */

typedef struct {
    int64_t t_us;
    uint8_t pin;
    uint8_t level;
} edge_t;

static edge_t s_ring[EDGE_RING];
static volatile uint32_t s_head;        /* written by the ISR only */
static volatile uint32_t s_tail;        /* written by the console task only */
static volatile uint32_t s_dropped;

/* In IRAM: the GPIO ISR service is IRAM-safe (io_in.c), so every handler on it must be. */
static void IRAM_ATTR edge_isr(void *arg)
{
    uint32_t h = s_head;
    if (h - s_tail >= EDGE_RING) {
        s_dropped++;
        return;
    }
    int pin = (int)(intptr_t)arg;
    edge_t *e = &s_ring[h & (EDGE_RING - 1)];
    e->t_us = esp_timer_get_time();
    e->pin = (uint8_t)pin;
    e->level = (uint8_t)gpio_get_level((gpio_num_t)pin);
    s_head = h + 1;
}

static void oom(console_req_t *req)
{
    console_err(req, CONSOLE_ERR_INTERNAL, "out of memory");
}

static void t_edges(console_req_t *req)
{
    cJSON *p = cJSON_CreateObject();
    cJSON *edges = cJSON_CreateArray();
    if (p == NULL || edges == NULL) {
        cJSON_Delete(p);
        cJSON_Delete(edges);
        oom(req);
        return;
    }
    int raw = 0;
    for (int pin = 0; pin < IO_OUT_COUNT; pin++) {
        raw |= gpio_get_level((gpio_num_t)pin) << pin;
    }
    uint32_t head = s_head;
    for (uint32_t t = s_tail; t != head; t++) {
        const edge_t *e = &s_ring[t & (EDGE_RING - 1)];
        cJSON *a = cJSON_CreateArray();
        if (a == NULL) {
            break;
        }
        cJSON_AddItemToArray(a, cJSON_CreateNumber(e->pin));
        cJSON_AddItemToArray(a, cJSON_CreateNumber(e->level));
        cJSON_AddItemToArray(a, cJSON_CreateNumber((double)e->t_us));
        cJSON_AddItemToArray(edges, a);
    }
    s_tail = head;
    cJSON_AddNumberToObject(p, "raw", raw);             /* bit n = level of GPIO n */
    cJSON_AddNumberToObject(p, "dropped", (double)s_dropped);
    cJSON_AddItemToObject(p, "edges", edges);
    console_ok(req, p);
}

static const console_cmd_t s_t_edges = {
    .name = "io t_edges",
    .help = "TEST BUILD: drain recorded output edges [[pin,level,t_us],...] and raw levels",
    .handler = t_edges,
};

/* ------------------------------------------------------------ input driver */

#define DRIVE_MAX_US 500000             /* the console task busy-waits, keep it short */

static struct arg_int *s_drive_pin;
static struct arg_str *s_drive_seq;
static void *s_drive_args[3];

static void spin_us(uint32_t us)
{
    int64_t until = esp_timer_get_time() + us;
    while (esp_timer_get_time() < until) {
    }
}

/* Pattern "L300H300L5000": drive low (L) or release (H) for that many microseconds, in
 * order; the pin stays in the last state. Returns the total microseconds, or -1. */
static int64_t drive(int pin, const char *s)
{
    int64_t total = 0;
    while (*s != '\0') {
        char c = *s++;
        if ((c != 'L' && c != 'H') || *s < '0' || *s > '9') {
            return -1;
        }
        char *end;
        unsigned long us = strtoul(s, &end, 10);
        s = end;
        total += (int64_t)us;
        if (total > DRIVE_MAX_US) {
            return -1;
        }
        gpio_set_level((gpio_num_t)pin, c == 'H' ? 1 : 0);
        spin_us((uint32_t)us);
    }
    return total;
}

static void t_drive(console_req_t *req)
{
    int pin = s_drive_pin->ival[0];
    if (io_in_index(pin) < 0) {
        console_err(req, CONSOLE_ERR_BAD_PIN, "not an input");
        return;
    }
    int64_t us = drive(pin, s_drive_seq->sval[0]);
    if (us < 0) {
        console_err(req, CONSOLE_ERR_BAD_ARGS, "pattern: (L|H)<us>..., total <= 500000 us");
        return;
    }
    cJSON *p = cJSON_CreateObject();
    if (p == NULL) {
        oom(req);
        return;
    }
    cJSON_AddNumberToObject(p, "pin", pin);
    cJSON_AddNumberToObject(p, "level", gpio_get_level((gpio_num_t)pin));
    cJSON_AddNumberToObject(p, "us", (double)us);
    console_ok(req, p);
}

static const console_cmd_t s_t_drive = {
    .name = "io t_drive",
    .help = "TEST BUILD: drive an input pin, e.g. L300H300L5000 (L = low, H = released, us)",
    .args = s_drive_args,
    .handler = t_drive,
};

/* ------------------------------------------------------------ timer-context emitter */

static struct arg_int *s_emit_n;
static struct arg_int *s_emit_ms;
static void *s_emit_args[3];
static esp_timer_handle_t s_emit_timer;
static volatile uint32_t s_emit_target;
static volatile uint32_t s_emitted;
static volatile int64_t s_emit_max_us;

static void emit_cb(void *arg)
{
    (void)arg;
    cJSON *f = cJSON_CreateObject();
    if (f != NULL) {
        cJSON_AddNumberToObject(f, "n", (double)(s_emitted + 1));
    }
    int64_t t0 = esp_timer_get_time();
    proto_evt("iotest", f);
    int64_t dt = esp_timer_get_time() - t0;
    if (dt > s_emit_max_us) {
        s_emit_max_us = dt;
    }
    if (++s_emitted >= s_emit_target) {
        esp_timer_stop(s_emit_timer);
    }
}

static void t_emit(console_req_t *req)
{
    int n = s_emit_n->ival[0];
    int ms = s_emit_ms->ival[0];
    if (n < 1 || n > 100000 || ms < 1 || ms > 60000) {
        console_err(req, CONSOLE_ERR_BAD_ARGS, "n 1..100000, period 1..60000 ms");
        return;
    }
    if (s_emit_timer == NULL) {
        const esp_timer_create_args_t a = {.callback = emit_cb, .name = "iotest"};
        if (esp_timer_create(&a, &s_emit_timer) != ESP_OK) {
            console_err(req, CONSOLE_ERR_INTERNAL, "no timer");
            return;
        }
    }
    if (esp_timer_is_active(s_emit_timer)) {
        console_err(req, CONSOLE_ERR_BUSY, "emitter running");
        return;
    }
    s_emitted = 0;
    s_emit_max_us = 0;
    s_emit_target = (uint32_t)n;
    if (esp_timer_start_periodic(s_emit_timer, (uint64_t)ms * 1000u) != ESP_OK) {
        console_err(req, CONSOLE_ERR_INTERNAL, "cannot start timer");
        return;
    }
    console_ok(req, NULL);
}

static const console_cmd_t s_t_emit = {
    .name = "io t_emit",
    .help = "TEST BUILD: emit <n> iotest events from an esp_timer callback every <ms>",
    .args = s_emit_args,
    .handler = t_emit,
};

static void t_stat(console_req_t *req)
{
    cJSON *p = cJSON_CreateObject();
    if (p == NULL) {
        oom(req);
        return;
    }
    TaskHandle_t h = xTaskGetHandle("esp_timer");
    cJSON_AddNumberToObject(p, "emitted", (double)s_emitted);
    cJSON_AddNumberToObject(p, "target", (double)s_emit_target);
    cJSON_AddBoolToObject(p, "running", s_emit_timer != NULL && esp_timer_is_active(s_emit_timer));
    cJSON_AddNumberToObject(p, "max_evt_us", (double)s_emit_max_us);
    cJSON_AddNumberToObject(p, "timer_stack", CONFIG_ESP_TIMER_TASK_STACK_SIZE);
    cJSON_AddNumberToObject(p, "timer_stack_free_min",
                            h != NULL ? (double)uxTaskGetStackHighWaterMark(h) : -1.0);
    console_ok(req, p);
}

static const console_cmd_t s_t_stat = {
    .name = "io t_stat",
    .help = "TEST BUILD: emitter progress, longest proto_evt, esp_timer task stack headroom",
    .handler = t_stat,
};

void io_test_register(void)
{
    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    for (int pin = 0; pin < IO_OUT_COUNT; pin++) {
        ESP_ERROR_CHECK(gpio_set_intr_type((gpio_num_t)pin, GPIO_INTR_ANYEDGE));
        ESP_ERROR_CHECK(gpio_isr_handler_add((gpio_num_t)pin, edge_isr, (void *)(intptr_t)pin));
    }
    s_drive_pin = arg_int1(NULL, NULL, "<pin>", "input 10, 20 or 21");
    s_drive_seq = arg_str1(NULL, NULL, "<pattern>", "(L|H)<us>...");
    s_emit_n = arg_int1(NULL, NULL, "<n>", "events");
    s_emit_ms = arg_int1(NULL, NULL, "<ms>", "period");
    if (s_drive_pin == NULL || s_drive_seq == NULL || s_emit_n == NULL || s_emit_ms == NULL) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    s_drive_args[0] = s_drive_pin;
    s_drive_args[1] = s_drive_seq;
    s_emit_args[0] = s_emit_n;
    s_emit_args[1] = s_emit_ms;
    ESP_ERROR_CHECK(console_register(&s_t_edges));
    ESP_ERROR_CHECK(console_register(&s_t_drive));
    ESP_ERROR_CHECK(console_register(&s_t_emit));
    ESP_ERROR_CHECK(console_register(&s_t_stat));
}
