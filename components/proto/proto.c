/* "@@" frame writer (README 3.3). Rendering and the ring are in proto_core.c.
 *
 * Why a frame cannot be split by ESP_LOG output (ESP-IDF v5.5.2, this build uses
 * CONFIG_LOG_VERSION_1):
 *  - With log version 1 each log line is ONE vprintf() call on stdout (the default sink);
 *    newlib's vfprintf holds the stdout FILE lock for that whole call. (Log version 2
 *    instead brackets the line with flockfile(stdout)/funlockfile(stdout).)
 *  - fwrite() takes the same recursive FILE lock for the entire call, including the
 *    chunked unbuffered write down to the VFS. So one fwrite of the complete
 *    "\n@@{json}\n" line is atomic against log lines in both directions.
 *  - The USB-Serial-JTAG VFS write has its own write_lock per call.
 * Not covered (and not used by this firmware): esp_rom_printf, panic output and logging
 * from constrained contexts (ISR / scheduler off), which bypass the stdout lock.
 * Our own mutex serialises seq assignment, ring insertion and write order, so frames on
 * the wire are in strictly increasing seq order.
 *
 * Rules for a custom log sink (esp_log_set_vprintf, embpet-86h):
 *  - it must write each log line with ONE stdio call on stdout (or hold flockfile(stdout)
 *    around its several calls), otherwise it can split a frame;
 *  - nothing may ESP_LOG while holding s_lock if a sink can call proto_*: the sink would
 *    wait on s_lock held by the logging task (deadlock). This file never logs under
 *    s_lock. */
#include "proto.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "driver/usb_serial_jtag.h"
#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#ifdef CONFIG_EMBPET_PROTO_STRESS
void proto_stress_start(void);
#endif

static SemaphoreHandle_t s_lock;
static char s_boot[9];
static uint32_t s_seq;
static proto_ring_t s_ring;

/* Caller holds s_lock. Blocks at most the VFS's TX timeout (about 50 ms once per host
 * stall; later bytes are then dropped immediately). A short write is ignored on
 * purpose: the frame stays in the ring and the host recovers with `evt since`. */
static void write_line(const char *line, size_t len)
{
    if (usb_serial_jtag_is_connected()) {
        (void)fwrite(line, 1, len, stdout);
        (void)fflush(stdout);
    }
}

void proto_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock != NULL);
    snprintf(s_boot, sizeof(s_boot), "%08" PRIx32, esp_random());
    s_seq = 0;
    proto_ring_init(&s_ring);
#ifdef CONFIG_EMBPET_PROTO_STRESS
    proto_stress_start();
#endif
}

const char *proto_boot_id(void)
{
    return s_boot;
}

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    case ESP_RST_USB:       return "usb";
    case ESP_RST_JTAG:      return "jtag";
    case ESP_RST_EFUSE:     return "efuse";
    case ESP_RST_PWR_GLITCH: return "pwr_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default:                return "unknown";
    }
}

uint32_t proto_emit_boot(void)
{
    cJSON *f = cJSON_CreateObject();
    if (f != NULL) {
        cJSON_AddStringToObject(f, "fw", esp_app_get_description()->version);
        cJSON_AddStringToObject(f, "idf", esp_get_idf_version());
        cJSON_AddStringToObject(f, "reset_reason", reset_reason_str(esp_reset_reason()));
    }
    return proto_evt("boot", f);
}

uint32_t proto_res(const proto_ctx_t *ctx, cJSON *payload)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t seq = s_seq + 1;
    size_t len = 0;
    char *line = proto_core_render_res(seq, s_boot, ctx, NULL, NULL, payload, &len);
    if (line == NULL) {
        xSemaphoreGive(s_lock);
        return 0;
    }
    s_seq = seq;
    write_line(line, len);
    xSemaphoreGive(s_lock);
    free(line);
    return seq;
}

uint32_t proto_res_err(const proto_ctx_t *ctx, const char *code, const char *msg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t seq = s_seq + 1;
    size_t len = 0;
    char *line = proto_core_render_res(seq, s_boot, ctx, code != NULL ? code : "error", msg,
                                       NULL, &len);
    if (line == NULL) {
        xSemaphoreGive(s_lock);
        return 0;
    }
    s_seq = seq;
    write_line(line, len);
    xSemaphoreGive(s_lock);
    free(line);
    return seq;
}

uint32_t proto_evt(const char *ev, cJSON *fields)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t seq = s_seq + 1;
    size_t len = 0;
    char *line = proto_core_render_evt(seq, s_boot, ev, fields, &len);
    if (line == NULL) {
        xSemaphoreGive(s_lock);
        return 0;
    }
    s_seq = seq;
    write_line(line, len);
    proto_ring_push(&s_ring, seq, line, len);     /* ring takes ownership of line */
    xSemaphoreGive(s_lock);
    return seq;
}

static void replay_visit(uint32_t seq, const char *line, size_t len, void *ctx)
{
    (void)seq;
    (void)ctx;
    write_line(line, len);
}

size_t proto_evt_replay(uint32_t since, bool *truncated)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = proto_ring_since(&s_ring, since, replay_visit, NULL, truncated);
    xSemaphoreGive(s_lock);
    return n;
}
