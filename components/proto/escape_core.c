/* Console input core: line assembly, argument splitting with escapes, -a/-i helpers.
 * See escape_core.h. No ESP-IDF includes; unit-tested in test/host/escape. */
#include "escape_core.h"

#include <string.h>

void esc_line_init(esc_line_t *l, char *buf, size_t cap)
{
    l->buf = buf;
    l->cap = cap;
    l->prev_cr = false;
    esc_line_reset(l);
}

void esc_line_reset(esc_line_t *l)
{
    l->len = 0;
    l->overflow = false;
    l->buf[0] = '\0';
}

esc_line_ev_t esc_line_feed(esc_line_t *l, uint8_t c)
{
    if (c == '\r' || c == '\n') {
        if (c == '\n' && l->prev_cr) {
            l->prev_cr = false;         /* second half of CRLF */
            return ESC_LINE_IGNORED;
        }
        l->prev_cr = (c == '\r');
        l->buf[l->len] = '\0';
        return ESC_LINE_DONE;
    }
    l->prev_cr = false;
    if (c == 0x15) {                    /* Ctrl-U: kill the line, overflow included */
        esc_line_reset(l);
        return ESC_LINE_KILLED;
    }
    if (c == 0x08 || c == 0x7f) {       /* BS or DEL: erase one character */
        if (l->len == 0 || l->overflow) {
            return ESC_LINE_IGNORED;
        }
        /* Drop UTF-8 continuation bytes (10xxxxxx, at most 3) together with their lead
         * byte. */
        int cont = 0;
        do {
            l->len--;
        } while (l->len > 0 && ((uint8_t)l->buf[l->len] & 0xC0u) == 0x80u && ++cont <= 3);
        l->buf[l->len] = '\0';
        return ESC_LINE_ERASED;
    }
    if (c < 0x20 && c != '\t') {
        return ESC_LINE_IGNORED;        /* other control bytes (ESC, Ctrl-C, ...); TAB is
                                           kept: a separator outside quotes, text inside */
    }
    if (l->len + 1 >= l->cap) {
        l->overflow = true;
        return ESC_LINE_IGNORED;
    }
    l->buf[l->len++] = (char)c;
    return ESC_LINE_STORED;
}

static bool is_sep(char c)
{
    return c == ' ' || c == '\t';
}

esc_err_t esc_split(char *line, char **argv, size_t argv_cap, size_t *argc)
{
    size_t n = 0;
    char *in = line;
    char *out = line;           /* write position; out <= in always */
    esc_err_t err = ESC_OK;

    for (;;) {
        while (is_sep(*in)) {
            in++;
        }
        if (*in == '\0') {
            break;
        }
        if (n + 1 >= argv_cap) {
            err = ESC_ERR_TOO_MANY_ARGS;
            break;
        }
        char *start = out;
        if (*in == '"') {
            in++;
            for (;;) {
                char c = *in;
                if (c == '\0') {
                    err = ESC_ERR_UNTERMINATED;
                    goto done;
                }
                in++;
                if (c == '"') {
                    break;
                }
                if (c == '\\') {
                    char e = *in;
                    if (e == '\0') {
                        err = ESC_ERR_TRAILING_BACKSLASH;
                        goto done;
                    }
                    in++;
                    if (e == 'n') {
                        c = '\n';
                    } else if (e == '"' || e == '\\') {
                        c = e;
                    } else {
                        err = ESC_ERR_BAD_ESCAPE;
                        goto done;
                    }
                }
                *out++ = c;
            }
            if (*in != '\0' && !is_sep(*in)) {
                err = ESC_ERR_AFTER_QUOTE;
                goto done;
            }
        } else {
            while (*in != '\0' && !is_sep(*in)) {
                *out++ = *in++;
            }
        }
        char end = *in;             /* NUL or a separator */
        if (end != '\0') {
            in++;                   /* out < in now, so the NUL below is safe */
        }
        *out++ = '\0';
        argv[n++] = start;
        if (end == '\0') {
            break;
        }
    }
done:
    argv[n] = NULL;
    *argc = n;
    return err;
}

const char *esc_strerror(esc_err_t err)
{
    switch (err) {
    case ESC_OK:                     return "ok";
    case ESC_ERR_UNTERMINATED:       return "missing closing quote";
    case ESC_ERR_BAD_ESCAPE:         return "unknown escape (use \\n, \\\" or \\\\)";
    case ESC_ERR_TRAILING_BACKSLASH: return "line ends with a backslash";
    case ESC_ERR_AFTER_QUOTE:        return "text after closing quote";
    case ESC_ERR_TOO_MANY_ARGS:      return "too many arguments";
    }
    return "?";
}

bool esc_id_valid(const char *s, size_t max_len)
{
    if (s == NULL) {
        return false;
    }
    size_t n = 0;
    for (; s[n] != '\0'; n++) {
        char c = s[n];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-';
        if (!ok || n >= max_len) {
            return false;
        }
    }
    return n > 0;
}

void esc_scan_ids(size_t argc, char *const *argv, const char **actor, const char **rid)
{
    *actor = NULL;
    *rid = NULL;
    for (size_t i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            break;
        }
        if (a[0] != '-' || (a[1] != 'a' && a[1] != 'i')) {
            continue;
        }
        const char **dst = (a[1] == 'a') ? actor : rid;
        const char *val = NULL;
        if (a[2] != '\0') {
            val = a + 2;
        } else if (i + 1 < argc) {
            val = argv[++i];
        }
        if (*dst == NULL) {
            *dst = val;
        }
    }
}
