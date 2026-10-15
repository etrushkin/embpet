#ifndef EMBPET_ESCAPE_CORE_H
#define EMBPET_ESCAPE_CORE_H
/* Pure logic (no ESP-IDF includes) for the console input path (README 3.2):
 *  - esc_line_*: assemble bytes from the port into lines (CR, LF or CRLF; backspace;
 *    control bytes other than TAB dropped; UTF-8 bytes kept; overflow detection);
 *  - esc_split: split a line into argv and decode the escapes of quoted arguments;
 *  - esc_id_valid / esc_scan_ids: the common -a <actor> / -i <rid> options.
 *
 * Why the console does its own splitting: IDF 5.5.2 esp_console_split_argv() drops an
 * unknown escape together with its character, so "a\nb" reaches the command as "ab" and
 * the newline cannot be recovered afterwards. linenoise's sanitize() also removes every
 * byte for which isprint() is false, which in the C locale includes all UTF-8 bytes. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- line assembly ---- */

typedef struct {
    char *buf;          /* caller's buffer, cap bytes; holds at most cap - 1 bytes + NUL */
    size_t cap;
    size_t len;
    bool overflow;      /* bytes beyond cap - 1 were discarded on this line */
    bool prev_cr;       /* last terminator was CR, so a following LF is swallowed */
} esc_line_t;

typedef enum {
    ESC_LINE_IGNORED = 0,   /* byte consumed, nothing to echo (control byte other than
                               TAB, overflow, backspace on an empty line, LF after CR) */
    ESC_LINE_STORED,        /* byte appended: echo it */
    ESC_LINE_ERASED,        /* one character (a UTF-8 sequence: the lead byte and at most
                               3 continuation bytes) removed: echo "\b \b" */
    ESC_LINE_KILLED,        /* Ctrl-U (0x15): the whole line, partial or not, is discarded
                               (the line holds no quote or escape state, that is only
                               looked at by esc_split) and the buffer is already reset */
    ESC_LINE_DONE,          /* CR or LF: buf is NUL-terminated, len bytes; call
                               esc_line_reset() before feeding the next line */
} esc_line_ev_t;

/* cap must be at least 1. */
void esc_line_init(esc_line_t *l, char *buf, size_t cap);

/* Start a new line (keeps prev_cr so CRLF yields one line). */
void esc_line_reset(esc_line_t *l);

esc_line_ev_t esc_line_feed(esc_line_t *l, uint8_t c);

/* ---- splitting and escapes ---- */

typedef enum {
    ESC_OK = 0,
    ESC_ERR_UNTERMINATED,       /* quoted argument without a closing quote */
    ESC_ERR_BAD_ESCAPE,         /* backslash inside quotes followed by other than n " \ */
    ESC_ERR_TRAILING_BACKSLASH, /* line ends right after a backslash inside quotes */
    ESC_ERR_AFTER_QUOTE,        /* closing quote not followed by a space or the line end */
    ESC_ERR_TOO_MANY_ARGS,      /* more than argv_cap - 1 arguments */
} esc_err_t;

/* Split `line` in place into at most argv_cap - 1 arguments, separated by spaces or tabs.
 * An argument that starts with '"' is quoted: it ends at the next unescaped '"', may
 * contain spaces, and inside it \n, \" and \\ are decoded to LF, " and \. An unquoted
 * argument is taken verbatim (a backslash or '"' inside it is an ordinary byte). Bytes
 * >= 0x80 pass through unchanged (UTF-8 is not validated here). `""` is an empty argument.
 * Decoding only shrinks, so it is done in place.
 *
 * Always sets *argc and argv[*argc] = NULL (argv_cap >= 1). On error, *argc counts the
 * arguments completed before the error; they stay usable, e.g. to echo -i / -a. */
esc_err_t esc_split(char *line, char **argv, size_t argv_cap, size_t *argc);

/* Short human-readable text for an error ("ok" for ESC_OK). */
const char *esc_strerror(esc_err_t err);

/* ---- common options ---- */

#define ESC_ACTOR_MAX 16
#define ESC_RID_MAX   32

/* true if s matches [A-Za-z0-9_-]{1,max_len}. */
bool esc_id_valid(const char *s, size_t max_len);

/* Find the first "-a X" / "-aX" and "-i X" / "-iX" in argv[1..argc-1], stopping at "--".
 * Sets *actor / *rid to the value (pointer into argv) or NULL. Values are not validated.
 * Used to echo rid/actor when the full option parse fails or the command is unknown. */
void esc_scan_ids(size_t argc, char *const *argv, const char **actor, const char **rid);

#endif /* EMBPET_ESCAPE_CORE_H */
