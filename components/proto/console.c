/* Serial console: REPL task, command table with -a/-i, response-once, log sink.
 * See console.h for why esp_console's REPL is not used.
 *
 * Output path (embpet-s2j.4). With the driver installed, the IDF VFS (usb_serial_jtag_vfs.c,
 * IDF 5.5.2) writes stdout one byte per usb_serial_jtag_write_bytes() call, about 10 us per
 * byte. So frames, log lines, echo, prompt and help text bypass it: console_out() converts
 * LF to CRLF itself (the VFS does that for CONFIG_LIBC_STDOUT_LINE_ENDING_CRLF) into a
 * staging buffer and hands the driver whole chunks, under flockfile(stdout). It keeps the
 * VFS's other semantics:
 *  - nothing is written while usb_serial_jtag_is_connected() is false (no USB SOF for
 *    ~3 ms: cable out, host asleep; the VFS write returns -1 then). An enumerated port that
 *    no program has open still counts as connected.
 *  - a chunk that does not fit in the TX ring waits up to 50 ms (TX_FLUSH_TIMEOUT_US) once;
 *    if that times out, later chunks are dropped at once until one fits again. So output
 *    never blocks indefinitely and a stalled host costs one 50 ms wait per stall. A driver
 *    write is all or nothing, so a dropped chunk is whole (the VFS dropped single bytes),
 *    and the first dropped chunk ends that console_out() call: the rest of a longer line is
 *    not sent either. A frame cut by a stall is truncated, never spliced; it lacks its end,
 *    so hosts see an unparsable line.
 * Text that another stdio caller left in stdout's buffer is flushed first, under the lock.
 * Log sink (README 3.1, "never block when no host reads"): it drops whole log lines while
 * the cable is unplugged or the TX ring is full, or when console_out() could not write the
 * line (the host stopped reading), and counts them (sys info log_dropped), so once a stall
 * is established logging costs next to nothing. The write that finds the ring full can
 * still pay the one 50 ms wait (soak build, port closed 60 s and 120 s: max proto_evt
 * 50.0 ms, max ESP_LOGW 49 ms, once per close). Frames (proto.c) are written normally;
 * events stay in the ring for evt since.
 * Rules from proto.c are kept: each log line and each frame is written whole while holding
 * flockfile(stdout), the lock every stdio call on stdout takes, and the sink never calls
 * proto_*, so logging while holding proto's lock cannot deadlock. */
#include "console.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "argtable3/argtable3.h"
#include "console_priv.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_select.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "escape_core.h"
#include "esp_freertos_hooks.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "mbedtls/platform_util.h"
#include "proto.h"
#include "sdkconfig.h"

/* Written against the IDF 5.5 USB-Serial-JTAG driver; on an upgrade re-check: the ISR calls
 * the select-notify hook (rx_gate) right after queueing each RX packet
 * (usb_serial_jtag.c:144-154 in 5.5.2), the hook has one slot that VFS select() takes over
 * (usb_serial_jtag_vfs.c:421/453), the per-byte VFS write path (console_out), and the LL
 * int_ena helpers (usb_serial_jtag_ll_{ena,disable}_intr_mask, _get_intr_ena_status). */
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 5, 0) || \
    ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 6, 0)
#error "console.c relies on IDF 5.5.x USB-Serial-JTAG driver internals; re-check them (see above)"
#endif

#ifndef CONFIG_LIBC_STDOUT_LINE_ENDING_CRLF
#error "console_out() sends LF as CRLF like the VFS does with LIBC_STDOUT_LINE_ENDING_CRLF"
#endif

static const char *TAG = "console";

#define PROMPT         "embpet> "
#define ARGV_MAX       32
#define WORD_MAX       16           /* command word incl. NUL */
#define REPL_STACK     6144
#define REPL_PRIO      3
#define RX_CHUNK       64
#define USJ_TX_RING    2048
#define USJ_RX_RING    (CONFIG_EMBPET_CMDLINE_MAX + 256)   /* one full line fits */
#define USJ_PKT        64           /* USB packet / RX FIFO size */
/* Stop taking packets when less is free (rx_gate). The free estimate (ring size minus bytes
 * waiting) can be up to one packet high while read_bytes is copying a chunk it has not yet
 * returned to the ring, so the effective margin is 2-3 packets. */
#define RX_GATE_FREE   (3 * USJ_PKT)
#define RX_POLL_MS     1000         /* REPL read timeout: rx_ungate runs at least this often */
#define IDLE_STARVE_MS 100          /* see let_idle_run */
#define IDLE_WAIT_TICKS pdMS_TO_TICKS(100)  /* at most this long per wait */
#define TX_CHUNK       512          /* staging buffer: one driver write per chunk */
#define TX_TIMEOUT_MS  50           /* the VFS's TX_FLUSH_TIMEOUT_US */
#define LOG_LINE_MAX   256          /* longer log lines are formatted into the heap */

struct console_req {
    proto_ctx_t ctx;
    bool responded;
};

typedef struct {
    const console_cmd_t *cmd;
    char w1[WORD_MAX];
    char w2[WORD_MAX];              /* "" for a one-word command */
    void **table;                   /* -a, -i, the command's args, arg_end */
    struct arg_str *actor;
    struct arg_str *rid;
    struct arg_end *end;
} cmd_entry_t;

static cmd_entry_t s_cmds[CONSOLE_MAX_CMDS];
static size_t s_ncmds;              /* published with release/acquire, see console_register */
static atomic_uint s_log_dropped;
static atomic_uint s_idle_tick;     /* tick count when the idle task last ran */
static atomic_uint s_idle_runs;     /* bumped on every idle task pass */
static char s_line[CONFIG_EMBPET_CMDLINE_MAX + 1];
static char *s_argv[ARGV_MAX];
static char s_tx[TX_CHUNK];         /* these three are guarded by flockfile(stdout) */
static bool s_tx_stalled;           /* a 50 ms wait timed out; set until a chunk fits */
static char s_log[LOG_LINE_MAX];
static portMUX_TYPE s_rx_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---------------------------------------------------------------- output */

/* Caller holds flockfile(stdout). The VFS's per-byte logic, applied per chunk. */
static bool tx_flush(size_t n)
{
    if (n == 0) {
        return true;
    }
    if (usb_serial_jtag_write_bytes(s_tx, n, 0) > 0) {
        s_tx_stalled = false;
        return true;
    }
    if (s_tx_stalled) {
        return false;
    }
    if (usb_serial_jtag_write_bytes(s_tx, n, pdMS_TO_TICKS(TX_TIMEOUT_MS)) > 0) {
        return true;
    }
    s_tx_stalled = true;
    return false;
}

bool console_out(const char *s, size_t n)
{
    if (n == 0) {
        return true;
    }
    if (!usb_serial_jtag_is_connected()) {
        return false;
    }
    if (!usb_serial_jtag_is_driver_installed()) {   /* before console_start: the VFS path */
        (void)fwrite(s, 1, n, stdout);
        (void)fflush(stdout);
        return true;
    }
    flockfile(stdout);
    (void)fflush(stdout);           /* text another stdio caller buffered goes out first */
    bool all = true;
    size_t o = 0;
    for (size_t k = 0; k < n && all; k++) {
        if (o + 2 > sizeof(s_tx)) {
            all = tx_flush(o);      /* a dropped chunk ends this write: truncate, never splice */
            o = 0;
        }
        if (s[k] == '\n') {
            s_tx[o++] = '\r';
        }
        s_tx[o++] = s[k];
    }
    if (all) {
        all = tx_flush(o);
    }
    funlockfile(stdout);
    return all;
}

/* ---------------------------------------------------------------- log sink */

/* With whole-chunk writes the TX ring rarely fills to the last byte, so write_ready() alone
 * misses most stalls: a line that console_out() could not write whole is counted too. */
static int log_vprintf(const char *fmt, va_list ap)
{
    if (!usb_serial_jtag_is_connected() ||
        (usb_serial_jtag_is_driver_installed() && !usb_serial_jtag_write_ready())) {
        atomic_fetch_add(&s_log_dropped, 1);
        return 0;
    }
    flockfile(stdout);
    va_list aq;
    va_copy(aq, ap);
    bool sent = true;
    int len = vsnprintf(s_log, sizeof(s_log), fmt, ap);
    if (len >= (int)sizeof(s_log)) {
        char *big = malloc((size_t)len + 1);
        if (big != NULL) {
            vsnprintf(big, (size_t)len + 1, fmt, aq);
            sent = console_out(big, (size_t)len);
            free(big);
        } else {                                    /* cut, but keep the line a line */
            s_log[sizeof(s_log) - 2] = '\n';
            sent = console_out(s_log, sizeof(s_log) - 1);
        }
    } else if (len > 0) {
        sent = console_out(s_log, (size_t)len);
    }
    va_end(aq);
    funlockfile(stdout);
    if (!sent) {
        atomic_fetch_add(&s_log_dropped, 1);
    }
    return len;
}

uint32_t console_log_dropped(void)
{
    return atomic_load(&s_log_dropped);
}

/* ---------------------------------------------------------------- responses */

void console_ok(console_req_t *req, cJSON *payload)
{
    if (req->responded) {
        cJSON_Delete(payload);
        ESP_LOGE(TAG, "%s: second response dropped", req->ctx.cmd ? req->ctx.cmd : "?");
        return;
    }
    req->responded = true;
    proto_res(&req->ctx, payload);
}

void console_err(console_req_t *req, const char *code, const char *msg)
{
    if (req->responded) {
        ESP_LOGE(TAG, "%s: second response dropped", req->ctx.cmd ? req->ctx.cmd : "?");
        return;
    }
    req->responded = true;
    proto_res_err(&req->ctx, code, msg);
}

const proto_ctx_t *console_ctx(const console_req_t *req)
{
    return &req->ctx;
}

/* ---------------------------------------------------------------- registry */

static bool split_name(const char *name, char *w1, char *w2)
{
    const char *sp = strchr(name, ' ');
    size_t l1 = sp ? (size_t)(sp - name) : strlen(name);
    size_t l2 = sp ? strlen(sp + 1) : 0;
    if (l1 == 0 || l1 >= WORD_MAX || l2 >= WORD_MAX || (sp != NULL && l2 == 0) ||
        (sp != NULL && strchr(sp + 1, ' ') != NULL)) {
        return false;
    }
    memcpy(w1, name, l1);
    w1[l1] = '\0';
    memcpy(w2, sp ? sp + 1 : "", l2 + 1);
    return true;
}

static size_t ncmds(void)
{
    return __atomic_load_n(&s_ncmds, __ATOMIC_ACQUIRE);
}

esp_err_t console_register(const console_cmd_t *cmd)
{
    if (cmd == NULL || cmd->name == NULL || cmd->handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t n = ncmds();
    if (n >= CONSOLE_MAX_CMDS) {
        return ESP_ERR_NO_MEM;
    }
    cmd_entry_t *e = &s_cmds[n];
    memset(e, 0, sizeof(*e));
    if (!split_name(cmd->name, e->w1, e->w2)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t k = 0; k < n; k++) {
        if (strcmp(s_cmds[k].w1, e->w1) == 0 && strcmp(s_cmds[k].w2, e->w2) == 0) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    size_t nargs = 0;
    while (cmd->args != NULL && cmd->args[nargs] != NULL) {
        nargs++;
    }
    e->table = calloc(nargs + 4, sizeof(void *));
    e->actor = arg_str0("a", NULL, "<actor>", "actor id [A-Za-z0-9_-]{1,16}");
    e->rid = arg_str0("i", NULL, "<rid>", "request id [A-Za-z0-9_-]{1,32}");
    e->end = arg_end(5);
    if (e->table == NULL || e->actor == NULL || e->rid == NULL || e->end == NULL) {
        free(e->table);
        free(e->actor);
        free(e->rid);
        free(e->end);
        return ESP_ERR_NO_MEM;
    }
    e->table[0] = e->actor;
    e->table[1] = e->rid;
    for (size_t k = 0; k < nargs; k++) {
        e->table[2 + k] = cmd->args[k];
    }
    e->table[2 + nargs] = e->end;
    e->cmd = cmd;
    /* Publish only after the entry is complete; the REPL task reads s_ncmds with acquire.
     * Registrations themselves must not run concurrently (console.h). */
    __atomic_store_n(&s_ncmds, n + 1, __ATOMIC_RELEASE);
    return ESP_OK;
}

/* Longest match: a two-word entry needs argv[1]. *used = words consumed. */
static const cmd_entry_t *find_cmd(size_t argc, char *const *argv, size_t *used)
{
    size_t n = ncmds();
    const cmd_entry_t *one = NULL;
    for (size_t k = 0; k < n; k++) {
        const cmd_entry_t *e = &s_cmds[k];
        if (strcmp(e->w1, argv[0]) != 0) {
            continue;
        }
        if (e->w2[0] == '\0') {
            one = e;
        } else if (argc >= 2 && strcmp(e->w2, argv[1]) == 0) {
            *used = 2;
            return e;
        }
    }
    *used = 1;
    return one;
}

static bool is_group(const char *w)
{
    size_t n = ncmds();
    for (size_t k = 0; k < n; k++) {
        if (s_cmds[k].w2[0] != '\0' && strcmp(s_cmds[k].w1, w) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_word(const char *w)
{
    return w[0] != '-' && esc_id_valid(w, WORD_MAX - 1);
}

/* Name echoed in the res of an unknown command: only safe ASCII words, else none. */
static const char *unknown_name(size_t argc, char *const *argv, char *buf, size_t cap)
{
    if (argc == 0 || !is_word(argv[0])) {
        return NULL;
    }
    if (is_group(argv[0]) && argc >= 2 && is_word(argv[1])) {
        snprintf(buf, cap, "%s %s", argv[0], argv[1]);
        return buf;
    }
    return argv[0];
}

/* ---------------------------------------------------------------- dispatch */

static void parse_and_run(const cmd_entry_t *e, int argc, char **argv, console_req_t *req)
{
    int nerr = arg_parse(argc, argv, e->table);
    if (nerr != 0) {
        /* argtable's text, one error per line; it may quote decoded user text (UTF-8,
         * newlines), which cJSON escapes. Cut to fit on a UTF-8 boundary, drop the
         * trailing newline. */
        char msg[160];
        arg_dstr_t ds = arg_dstr_create();
        arg_print_errors_ds(ds, e->end, e->cmd->name);
        const char *s = arg_dstr_cstr(ds);
        size_t o = strlen(s);
        if (o >= sizeof(msg)) {
            o = sizeof(msg) - 1;
            while (o > 0 && ((uint8_t)s[o] & 0xC0u) == 0x80u) {
                o--;                                /* s[o] would start the cut-off part */
            }
        }
        memcpy(msg, s, o);
        while (o > 0 && msg[o - 1] == '\n') {
            o--;
        }
        msg[o] = '\0';
        arg_dstr_destroy(ds);
        console_err(req, CONSOLE_ERR_BAD_ARGS, msg);
        return;
    }
    /* Echo exactly what argtable parsed (the pre-scan in run_line is only for errors). */
    req->ctx.actor = e->actor->count > 0 ? e->actor->sval[0] : NULL;
    req->ctx.rid = e->rid->count > 0 ? e->rid->sval[0] : NULL;
    bool rid_ok = req->ctx.rid == NULL || esc_id_valid(req->ctx.rid, ESC_RID_MAX);
    bool actor_ok = req->ctx.actor == NULL || esc_id_valid(req->ctx.actor, ESC_ACTOR_MAX);
    if (!rid_ok || !actor_ok) {
        if (!rid_ok) {
            req->ctx.rid = NULL;
        }
        if (!actor_ok) {
            req->ctx.actor = NULL;
        }
        console_err(req, CONSOLE_ERR_BAD_ARGS,
                    !rid_ok ? "-i <rid> must match [A-Za-z0-9_-]{1,32}"
                            : "-a <actor> must match [A-Za-z0-9_-]{1,16}");
        return;
    }
    e->cmd->handler(req);
    if (!req->responded) {
        ESP_LOGE(TAG, "%s: handler sent no response", e->cmd->name);
        console_err(req, CONSOLE_ERR_INTERNAL, "no response");
    }
}

static void run_line(const esc_line_t *l)
{
    size_t argc = 0;
    esc_err_t serr = esc_split(l->buf, s_argv, ARGV_MAX, &argc);
    if (argc == 0 && serr == ESC_OK && !l->overflow) {
        return;                                     /* blank line: no command, no res */
    }
    size_t used = 1;
    const cmd_entry_t *e = argc > 0 ? find_cmd(argc, s_argv, &used) : NULL;
    const char *actor;
    const char *rid;
    esc_scan_ids(argc, s_argv, &actor, &rid);

    char name[2 * WORD_MAX];
    console_req_t req = {0};
    req.ctx.cmd = e != NULL ? e->cmd->name : unknown_name(argc, s_argv, name, sizeof(name));
    req.ctx.rid = esc_id_valid(rid, ESC_RID_MAX) ? rid : NULL;
    req.ctx.actor = esc_id_valid(actor, ESC_ACTOR_MAX) ? actor : NULL;

    if (l->overflow) {
        char msg[48];
        snprintf(msg, sizeof(msg), "line longer than %d bytes", CONFIG_EMBPET_CMDLINE_MAX);
        console_err(&req, CONSOLE_ERR_TOO_LONG, msg);
    } else if (serr != ESC_OK) {
        console_err(&req, CONSOLE_ERR_BAD_ARGS, esc_strerror(serr));
    } else if (e == NULL) {
        console_err(&req, CONSOLE_ERR_BAD_ARGS, "unknown command (see help)");
    } else {
        /* argtable sees the last command word as argv[0] (the program name). */
        parse_and_run(e, (int)(argc - used + 1), &s_argv[used - 1], &req);
    }
}

/* ---------------------------------------------------------------- REPL */

static bool idle_hook(void)
{
    atomic_store(&s_idle_tick, (unsigned)xTaskGetTickCount());
    atomic_fetch_add(&s_idle_runs, 1);
    return true;
}

/* While input is waiting the REPL never blocks. When stdout still went through the per-byte
 * VFS (about 10 us per byte), a host sending `sys info` back to back kept this task at about
 * 95% CPU. The idle task then ran only in the gaps no priority 1-2 task wanted, so a TLS
 * handshake (tg tasks, priority 2) after boot, or a host keeping a few commands in flight at
 * any time, starved it for 5 s and tripped the task watchdog (embpet-94q). Batched output
 * (embpet-s2j.4) cut the cost per command 3-6x, but a host keeping several commands in
 * flight still keeps this task busy. So after each command and each input chunk, once the
 * idle task has not run for IDLE_STARVE_MS, the REPL sleeps tick by tick until it has run,
 * for at most IDLE_WAIT_TICKS (a sleep of one tick only lasts to the next tick boundary, and
 * priority 1-2 tasks may use all of it). That leaves lower tasks and the idle task a share
 * of the CPU while commands are still answered; a watchdog report then points at a task
 * that hogs the CPU itself. */
static void let_idle_run(void)
{
    TickType_t since = xTaskGetTickCount() - (TickType_t)atomic_load(&s_idle_tick);
    if (since <= pdMS_TO_TICKS(IDLE_STARVE_MS)) {
        return;
    }
    unsigned runs = atomic_load(&s_idle_runs);
    for (TickType_t k = 0; k < IDLE_WAIT_TICKS && atomic_load(&s_idle_runs) == runs; k++) {
        vTaskDelay(1);
    }
}

/* Input flow control (embpet-s2j.3). The driver's ISR reads every USB packet out of the
 * 64-byte RX FIFO into the RX ring and drops it when the ring is full, so while a command
 * blocks the REPL (`tg send`, up to 10 s) a host sending more than the ring holds lost
 * input. The driver calls this hook from its ISR after each packet it queued: once less
 * than RX_GATE_FREE is free, the RX packet interrupt goes off. The next packet then stays in
 * the FIFO and the hardware NAKs further ones, so the host's USB stack holds them and
 * retries; nothing is dropped and no RAM is added. rx_ungate() turns the interrupt back on
 * after the REPL has read enough.
 * The driver itself does an unlocked read-modify-write of the interrupt enable register in
 * task context (usb_serial_jtag_write_bytes enabling the TX interrupt), which can race both
 * ways:
 *  - it can turn RX back on while gated. RX_GATE_FREE leaves room for more packets, and each
 *    packet let through re-gates.
 *  - a preempted lower-priority writer can store a stale value with RX off after the REPL
 *    ungated. So rx_ungate() decides from the hardware enable bit, not from a flag, and the
 *    REPL reads with a timeout (RX_POLL_MS), so it re-enables input within that time even
 *    when the ring is empty. */
static void rx_gate(usj_select_notif_t notif, BaseType_t *task_woken)
{
    (void)task_woken;
    /* usb_serial_jtag_get_read_bytes_available() calls vRingbufferGetInfo, whose
     * portENTER_CRITICAL only masks interrupts on the single-core RISC-V port, so it is safe
     * here (the driver ISR calls it too); it would trip
     * CONFIG_FREERTOS_CHECK_PORT_CRITICAL_COMPLIANCE, which is off. */
    if (notif == USJ_SELECT_READ_NOTIF &&
        USJ_RX_RING - usb_serial_jtag_get_read_bytes_available() < RX_GATE_FREE) {
        usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
    }
}

static void rx_ungate(void)
{
    taskENTER_CRITICAL(&s_rx_mux);                  /* no rx_gate between check and enable */
    if (!(usb_serial_jtag_ll_get_intr_ena_status() & USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT) &&
        USJ_RX_RING - usb_serial_jtag_get_read_bytes_available() >= RX_GATE_FREE) {
        usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
    }
    taskEXIT_CRITICAL(&s_rx_mux);
}

static void repl_task(void *arg)
{
    (void)arg;
    static esc_line_t line;
    uint8_t in[RX_CHUNK];
    char echo[RX_CHUNK * 3];                       /* worst case: "\b \b" per byte */

    esc_line_init(&line, s_line, sizeof(s_line));
    console_out(PROMPT, strlen(PROMPT));
    for (;;) {
        int n = usb_serial_jtag_read_bytes(in, sizeof(in), pdMS_TO_TICKS(RX_POLL_MS));
        rx_ungate();
        size_t ne = 0;
        bool wipe = false;
        for (int k = 0; k < n; k++) {
            switch (esc_line_feed(&line, in[k])) {
            case ESC_LINE_STORED:
                echo[ne++] = (char)in[k];
                break;
            case ESC_LINE_ERASED:
                memcpy(&echo[ne], "\b \b", 3);
                ne += 3;
                break;
            case ESC_LINE_KILLED:
                /* Echo "^U", end the line and prompt again; a long echo run before it
                 * goes out first. The discarded text may have been a secret. */
                console_out(echo, ne);
                ne = 0;
                console_out("^U\n" PROMPT, 3 + strlen(PROMPT));
                mbedtls_platform_zeroize(s_line, sizeof(s_line));
                wipe = true;
                break;
            case ESC_LINE_DONE:
                echo[ne++] = '\n';
                console_out(echo, ne);
                ne = 0;
                run_line(&line);
                /* The line may hold a secret (`tg token`, `wifi set`): do not leave it in
                 * RAM. s_argv points into it, so it goes only after the command ran. */
                mbedtls_platform_zeroize(s_line, sizeof(s_line));
                wipe = true;
                esc_line_reset(&line);
                console_out(PROMPT, strlen(PROMPT));
                let_idle_run();                     /* one chunk can hold many commands */
                break;
            case ESC_LINE_IGNORED:
                break;
            }
        }
        if (ne > 0) {
            console_out(echo, ne);
        }
        if (wipe) {                                 /* the typed bytes, as read and echoed */
            mbedtls_platform_zeroize(in, sizeof(in));
            mbedtls_platform_zeroize(echo, sizeof(echo));
            flockfile(stdout);
            mbedtls_platform_zeroize(s_tx, sizeof(s_tx));
            funlockfile(stdout);
        }
        let_idle_run();
    }
}

/* ---------------------------------------------------------------- help */

static void help_handler(console_req_t *req)
{
    cJSON *names = cJSON_CreateArray();
    size_t n = ncmds();
    for (size_t k = 0; k < n; k++) {
        const cmd_entry_t *e = &s_cmds[k];
        arg_dstr_t ds = arg_dstr_create();
        arg_dstr_cat(ds, e->cmd->name);
        arg_print_syntax_ds(ds, e->table, "\n    ");
        arg_dstr_cat(ds, e->cmd->help ? e->cmd->help : "");
        arg_dstr_catc(ds, '\n');
        const char *text = arg_dstr_cstr(ds);
        console_out(text, strlen(text));
        arg_dstr_destroy(ds);
        cJSON_AddItemToArray(names, cJSON_CreateString(e->cmd->name));
    }
    cJSON *p = cJSON_CreateObject();
    cJSON_AddItemToObject(p, "cmds", names);
    console_ok(req, p);
}

static const console_cmd_t s_help = {
    .name = "help",
    .help = "List commands with their options",
    .args = NULL,
    .handler = help_handler,
};

esp_err_t console_start(void)
{
    esp_err_t err = console_register(&s_help);
    if (err != ESP_OK) {
        return err;
    }
#ifdef CONFIG_EMBPET_CONSOLE_PROBE
    console_probe_register();
#endif
    atomic_store(&s_idle_tick, (unsigned)xTaskGetTickCount());
    err = esp_register_freertos_idle_hook(idle_hook);
    if (err != ESP_OK) {
        return err;
    }
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = USJ_TX_RING,
        .rx_buffer_size = USJ_RX_RING,
    };
    err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    usb_serial_jtag_set_select_notif_callback(rx_gate);    /* select() is never used here */
    usb_serial_jtag_vfs_use_driver();
    esp_log_set_vprintf(log_vprintf);
    if (xTaskCreate(repl_task, "console", REPL_STACK, NULL, REPL_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
#ifdef CONFIG_EMBPET_CONSOLE_SOAK
    console_soak_start();
#endif
    return ESP_OK;
}
