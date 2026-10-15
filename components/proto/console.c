/* Serial console: REPL task, command table with -a/-i, response-once, log sink.
 * See console.h for why esp_console's REPL is not used.
 *
 * Log sink (README 3.1, "never block when no host reads"). Measured/derived facts for
 * IDF 5.5.2 with the USB-Serial-JTAG driver installed (usb_serial_jtag_vfs.c):
 *  - VFS write returns -1 at once while usb_serial_jtag_is_connected() is false (no USB
 *    SOF for ~3 ms: cable out, host asleep). An enumerated port that no program has open
 *    still counts as connected.
 *  - Otherwise each byte goes to the driver's TX ring; when the ring is full the first
 *    byte waits up to 50 ms, then every later byte is dropped at once until a write
 *    succeeds again. So stdout never blocks indefinitely, but a stalled host costs one
 *    50 ms wait per stall and bytes are lost mid-line.
 * The sink below drops whole log lines while the cable is unplugged or the TX ring is full
 * (the host stopped reading) and counts them (sys info log_dropped), so once a stall is
 * established logging costs nothing. The line that fills the ring can still pay the one
 * 50 ms wait (measured with the soak build: max ESP_LOGW 49 ms, max proto_evt 51 ms over a
 * 10 min closed port, once per close). Frames (proto.c) go through stdout unchanged; events
 * stay in the ring for evt since.
 * Rules from proto.c are kept: each log line is ONE stdio call (vprintf) on stdout, and
 * the sink never calls proto_*, so logging while holding proto's lock cannot deadlock. */
#include "console.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "argtable3/argtable3.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_select.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "escape_core.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"
#include "proto.h"
#include "sdkconfig.h"

#ifdef CONFIG_EMBPET_CONSOLE_SOAK
#include "console_priv.h"
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
static char s_line[CONFIG_EMBPET_CMDLINE_MAX + 1];
static char *s_argv[ARGV_MAX];

/* ---------------------------------------------------------------- log sink */

static int log_vprintf(const char *fmt, va_list ap)
{
    if (!usb_serial_jtag_is_connected() ||
        (usb_serial_jtag_is_driver_installed() && !usb_serial_jtag_write_ready())) {
        atomic_fetch_add(&s_log_dropped, 1);
        return 0;
    }
    return vprintf(fmt, ap);
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

static void out(const char *s, size_t n)
{
    fwrite(s, 1, n, stdout);
    fflush(stdout);
}

static void repl_task(void *arg)
{
    (void)arg;
    static esc_line_t line;
    uint8_t in[RX_CHUNK];
    char echo[RX_CHUNK * 3];                       /* worst case: "\b \b" per byte */

    esc_line_init(&line, s_line, sizeof(s_line));
    out(PROMPT, strlen(PROMPT));
    for (;;) {
        int n = usb_serial_jtag_read_bytes(in, sizeof(in), portMAX_DELAY);
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
            case ESC_LINE_DONE:
                echo[ne++] = '\n';
                out(echo, ne);
                ne = 0;
                run_line(&line);
                /* The line may hold a secret (`tg token`, `wifi set`): do not leave it in
                 * RAM. s_argv points into it, so it goes only after the command ran. */
                mbedtls_platform_zeroize(s_line, sizeof(s_line));
                wipe = true;
                esc_line_reset(&line);
                out(PROMPT, strlen(PROMPT));
                break;
            case ESC_LINE_IGNORED:
                break;
            }
        }
        if (ne > 0) {
            out(echo, ne);
        }
        if (wipe) {                                 /* the typed bytes, as read and echoed */
            mbedtls_platform_zeroize(in, sizeof(in));
            mbedtls_platform_zeroize(echo, sizeof(echo));
        }
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
        arg_print_syntax_ds(ds, e->table, "");
        printf("%s%s\n    %s\n", e->cmd->name, arg_dstr_cstr(ds), e->cmd->help ? e->cmd->help : "");
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
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = USJ_TX_RING,
        .rx_buffer_size = USJ_RX_RING,
    };
    err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        return err;
    }
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
