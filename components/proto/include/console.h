/* Serial console (README 3.1-3.3): REPL task on USB-Serial-JTAG, command table with the
 * common -a <actor> / -i <rid> options, exactly one res per command, console log sink.
 *
 * Input path: the REPL reads the USB-Serial-JTAG driver directly, assembles lines with
 * escape_core (echo, backspace, CR/LF/CRLF), splits them with esc_split (decodes \n \" \\
 * inside quotes, UTF-8 untouched) and dispatches here. esp_console's REPL/linenoise and
 * esp_console_run are not used because, in IDF 5.5.2, linenoise strips non-isprint bytes
 * (all UTF-8), esp_console_split_argv drops "\n" escapes, and the REPL prints "Unrecognized
 * command" instead of letting us answer with a res. argtable3 (from the console component)
 * still parses every command's options. */
#ifndef EMBPET_CONSOLE_H
#define EMBPET_CONSOLE_H

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "proto_core.h"

/* Error codes for res frames (README 3.3). */
#define CONSOLE_ERR_BAD_ARGS       "bad_args"
#define CONSOLE_ERR_BAD_PIN        "bad_pin"
#define CONSOLE_ERR_TOO_LONG       "too_long"
#define CONSOLE_ERR_OFFLINE        "offline"
#define CONSOLE_ERR_NOT_CONFIGURED "not_configured"
#define CONSOLE_ERR_AUTH_ERROR     "auth_error"
#define CONSOLE_ERR_TIMEOUT        "timeout"
#define CONSOLE_ERR_BUSY           "busy"
#define CONSOLE_ERR_INTERNAL       "internal"

typedef struct console_req console_req_t;

/* Runs on the console task. Must answer with exactly one console_ok() or console_err();
 * a second answer is dropped (and logged), and if the handler returns without answering
 * the console sends err "internal". May block (e.g. tg send waits for Telegram). */
typedef void (*console_handler_t)(console_req_t *req);

typedef struct {
    const char *name;           /* "sys info" or "help": one or two words, each <= 15 chars */
    const char *help;           /* one line for `help` */
    void **args;                /* the command's own argtable3 entries, NULL-terminated, no
                                   arg_end (it is added, together with -a and -i); NULL if
                                   the command takes no arguments. Parsed values are read
                                   from these entries inside the handler. */
    console_handler_t handler;
} console_cmd_t;

#define CONSOLE_MAX_CMDS 40

/* Register a command; `cmd` and everything it points to must stay valid (static). Callable
 * before or after console_start(), but registrations must not run concurrently with each
 * other (normally they all happen in app_main). ESP_ERR_INVALID_ARG for a bad name,
 * ESP_ERR_INVALID_STATE for a duplicate, ESP_ERR_NO_MEM when the table is full. */
esp_err_t console_register(const console_cmd_t *cmd);

/* Install the USB-Serial-JTAG driver (stdout then goes through its TX ring), set the log
 * sink and start the REPL task. Call once, after proto_init(). */
esp_err_t console_start(void);

/* Answer ok:true with the payload fields (consumed; may be NULL). Call only from the
 * handler, on the console task (req lives on its stack). */
void console_ok(console_req_t *req, cJSON *payload);

/* Answer ok:false with an error code (CONSOLE_ERR_*) and a human message (may be NULL). */
void console_err(console_req_t *req, const char *code, const char *msg);

/* cmd / rid / actor of the request (rid and actor are NULL when not given). */
const proto_ctx_t *console_ctx(const console_req_t *req);

/* Log lines dropped by the console log sink because no host was reading. */
uint32_t console_log_dropped(void);

#endif /* EMBPET_CONSOLE_H */
