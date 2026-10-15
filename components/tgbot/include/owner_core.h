#ifndef EMBPET_OWNER_CORE_H
#define EMBPET_OWNER_CORE_H
/* Pure logic (no ESP-IDF includes) of owner binding (README 2.1): pairing code generation
 * and validation, `/start <code>` parsing, the pairing state machine, `tg owner` argument
 * parsing and the text of the online notice. Uses cJSON. Used by bridge.c / tg_cmd.c and
 * unit-tested on the host. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#define PAIR_CODE_LEN    6
#define PAIR_CODE_BUF    (PAIR_CODE_LEN + 1)
#define PAIR_TTL_S       300            /* a code is valid for 5 minutes */
#define PAIR_MAX_WRONG   5              /* wrong codes after which pairing is cancelled */

#define PAIR_CONFIRM_TEXT "Paired. This chat is now the embpet owner."

/* A 6-digit code ("000000".."999999", leading zeros kept) from a random word. False if r is
 * in the biased tail (r >= 4294000000): draw again, so every code is equally likely. */
bool pair_code_make(uint32_t r, char out[PAIR_CODE_BUF]);

/* Parse the text of a message as `/start <code>` (extra spaces and trailing whitespace
 * allowed). `/start@<name> <code>` counts only if name equals bot (this bot's username,
 * case-insensitive; NULL or "" = unknown, then only the plain form counts), so another
 * bot's command in a group is no attempt. True only for a well-formed 6-digit code, which
 * is copied to code; a bare `/start` or `/start hello` is not a pairing attempt. */
bool pair_parse_start(const char *text, const char *bot, char code[PAIR_CODE_BUF]);

typedef struct {
    bool active;
    char code[PAIR_CODE_BUF];
    uint64_t expires_ms;                /* on the caller's monotonic clock */
    unsigned wrong;                     /* wrong codes seen while active */
} pair_t;

void pair_init(pair_t *p);

/* Start (or restart) pairing with code, valid ttl_s seconds from now_ms. */
void pair_start(pair_t *p, const char *code, uint64_t now_ms, uint32_t ttl_s);

void pair_cancel(pair_t *p);

typedef enum {
    PAIR_PASS = 0,      /* not a pairing attempt (inactive, expired, not `/start <code>`):
                           route the update normally */
    PAIR_MATCH,         /* right code: bind the owner (the caller then cancels) */
    PAIR_WRONG,         /* wrong code: ignore the update */
    PAIR_LOCKED,        /* wrong code, the PAIR_MAX_WRONG-th: pairing is cancelled, ignore */
} pair_result_t;

/* Judge the text of an incoming message (bot as for pair_parse_start) at now_ms. An expired pairing becomes inactive
 * here (PAIR_PASS). Only PAIR_MATCH leaves the pairing active: the caller binds the owner
 * and cancels, so a failed write can retry with the same update. */
pair_result_t pair_check(pair_t *p, const char *text, const char *bot, uint64_t now_ms);

/* Remaining validity in whole seconds, rounded up; 0 if inactive or expired. */
uint32_t pair_left_s(const pair_t *p, uint64_t now_ms);

/* chat.id and text of an update's message (text is "" if absent, points into the update).
 * False if the update has no message with a chat id. */
bool owner_update_text(const cJSON *update, int64_t *chat, const char **text);

/* `tg owner <chat_id>`: optional '-', 1..15 digits, not zero (Telegram ids fit a JSON
 * number; groups are negative). */
bool owner_parse_chat(const char *s, int64_t *out);

/* "embpet online · fw <fw> · reset <reason>" ("·" is UTF-8 U+00B7) into out. Returns the
 * untruncated length; out is NUL-terminated (truncated if cap is too small, cap 0 no-op). */
size_t notice_format(char *out, size_t cap, const char *fw, const char *reason);

#endif /* EMBPET_OWNER_CORE_H */
