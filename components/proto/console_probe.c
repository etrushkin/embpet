/* Console probe commands (CONFIG_EMBPET_CONSOLE_PROBE, test builds only).
 *  - probe cpu          run-time counters (us) of the console task, the idle task and the
 *                       total, so a host computes console CPU per command as
 *                       delta(console_us) / commands (embpet-s2j.4, tools/console_cpu.py)
 *  - probe nop          ok with no payload: the fixed cost of a command
 *  - probe sleep <ms>   block the console task for ms (at most 30000), like a pending
 *                       `tg send` (embpet-s2j.3, tools/console_test.py pipeline)
 *  - probe echo <text>  ok with len and the Adler-32 of text, so a host can tell that a
 *                       long line arrived whole (embpet-s2j.3)
 *  - probe rxoff        turn the USB RX packet interrupt off while input is not gated, as a
 *                       lost re-enable would; the REPL must turn it back on within its read
 *                       timeout (embpet-s2j.3) */
#include <string.h>

#include "argtable3/argtable3.h"
#include "console.h"
#include "console_priv.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"

#define MAX_TASKS    24
#define SLEEP_MAX_MS 30000

static void probe_cpu(console_req_t *req)
{
    static TaskStatus_t st[MAX_TASKS];             /* console task only */
    configRUN_TIME_COUNTER_TYPE total = 0;
    UBaseType_t n = uxTaskGetSystemState(st, MAX_TASKS, &total);
    if (n == 0) {                                   /* more tasks than MAX_TASKS */
        console_err(req, CONSOLE_ERR_INTERNAL, "too many tasks");
        return;
    }
    double console_us = 0;
    double idle_us = 0;
    for (UBaseType_t k = 0; k < n; k++) {
        if (strcmp(st[k].pcTaskName, "console") == 0) {
            console_us = st[k].ulRunTimeCounter;
        } else if (strncmp(st[k].pcTaskName, "IDLE", 4) == 0) {
            idle_us = st[k].ulRunTimeCounter;
        }
    }
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "console_us", console_us);
    cJSON_AddNumberToObject(p, "idle_us", idle_us);
    cJSON_AddNumberToObject(p, "total_us", (double)total);
    console_ok(req, p);
}

static void probe_nop(console_req_t *req)
{
    console_ok(req, NULL);
}

static struct arg_int *s_sleep_arg;
static void *s_sleep_args[2];

static void probe_sleep(console_req_t *req)
{
    int ms = s_sleep_arg->ival[0];
    if (ms < 0 || ms > SLEEP_MAX_MS) {
        console_err(req, CONSOLE_ERR_BAD_ARGS, "<ms> must be 0..30000");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(ms));
    console_ok(req, NULL);
}

static struct arg_str *s_echo_arg;
static void *s_echo_args[2];

static void probe_echo(console_req_t *req)
{
    const char *t = s_echo_arg->sval[0];
    uint32_t sum = 0;                               /* Adler-32, as Python's zlib.adler32 */
    uint32_t acc = 1;
    size_t n = strlen(t);
    for (size_t k = 0; k < n; k++) {
        acc = (acc + (uint8_t)t[k]) % 65521u;
        sum = (sum + acc) % 65521u;
    }
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "len", (double)n);
    cJSON_AddNumberToObject(p, "sum", (double)((sum << 16) | acc));
    console_ok(req, p);
}

static void probe_rxoff(console_req_t *req)
{
    portDISABLE_INTERRUPTS();                       /* atomic against the driver ISR */
    usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
    portENABLE_INTERRUPTS();
    console_ok(req, NULL);
}

static const console_cmd_t s_rxoff = {
    .name = "probe rxoff",
    .help = "Test build: turn the USB RX interrupt off (lost re-enable fault)",
    .handler = probe_rxoff,
};

static const console_cmd_t s_cpu = {
    .name = "probe cpu",
    .help = "Test build: run-time counters (us) of the console and idle tasks",
    .handler = probe_cpu,
};

static const console_cmd_t s_nop = {
    .name = "probe nop",
    .help = "Test build: answer ok with no payload",
    .handler = probe_nop,
};

static const console_cmd_t s_sleep = {
    .name = "probe sleep",
    .help = "Test build: block the console for <ms> milliseconds",
    .args = s_sleep_args,
    .handler = probe_sleep,
};

static const console_cmd_t s_echo = {
    .name = "probe echo",
    .help = "Test build: length and Adler-32 of <text>",
    .args = s_echo_args,
    .handler = probe_echo,
};

void console_probe_register(void)
{
    s_sleep_arg = arg_int1(NULL, NULL, "<ms>", "milliseconds");
    s_echo_arg = arg_str1(NULL, NULL, "<text>", "any text");
    s_sleep_args[0] = s_sleep_arg;
    s_echo_args[0] = s_echo_arg;
    console_register(&s_cpu);
    console_register(&s_nop);
    console_register(&s_sleep);
    console_register(&s_echo);
    console_register(&s_rxoff);
}
