#ifndef EMBPET_TGAPI_CORE_H
#define EMBPET_TGAPI_CORE_H
/* Pure logic (no ESP-IDF includes) for the Telegram Bot API client (README 2.1): request
 * bodies, response classification, UTF-16 entity offsets and the retry delay. Uses cJSON.
 * Used by tgapi.c and unit-tested on the host. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

/* Largest response body accepted (docs/PLAN.md 1.2); a larger one is rejected unparsed. */
#define TG_RESP_MAX 16384

/* Cap for a getUpdates retry with limit 1, which must hold one update with the longest
 * possible owner reply quoting the longest possible bot message (docs/PLAN.md 1.2). */
#define TG_RESP_MAX_ONE 32768

/* getUpdates: updates per request (bead embpet-q9h). */
#define TG_UPDATES_LIMIT 5

/* sendMessage: Telegram accepts 1-4096 characters (UTF-16 code units) after entities
 * parsing, i.e. for the whole "[A] text". */
#define TG_TEXT_MAX_UTF16 4096

typedef enum {
    TG_OK = 0,
    TG_ERR_NET,         /* DNS / TCP / TLS / I/O failure or a cut-off body: offline, back off */
    TG_ERR_TIMEOUT,     /* no response headers within the connection's timeout */
    TG_ERR_SERVER,      /* HTTP 5xx, or a body that is not a Bot API answer: back off */
    TG_ERR_AUTH,        /* 401 / 404: the token was rejected */
    TG_ERR_RETRY,       /* 429: wait retry_after_s */
    TG_ERR_API,         /* any other ok:false (e.g. 400 chat not found, 403 blocked) */
    TG_ERR_TOO_LARGE,   /* body over the cap (TG_RESP_MAX, or TG_RESP_MAX_ONE) */
    TG_ERR_NO_TOKEN,    /* no valid token stored */
    TG_ERR_BAD_ARG,     /* invalid actor, invalid UTF-8 or empty text, chat id out of range */
    TG_ERR_NO_MEM,
    TG_ERR_TOO_LONG,    /* message over TG_TEXT_MAX_UTF16 (the console's err too_long) */
} tg_status_t;

/* Short lowercase name ("ok", "net", "timeout", ...); safe to log and to put in frames. */
const char *tg_status_name(tg_status_t st);

#define TG_DESC_SIZE 96         /* description buffer, NUL included */

typedef struct {
    int http_status;            /* 0 if no response arrived */
    int error_code;             /* Telegram error_code, else the HTTP status; 0 on TG_OK */
    uint32_t retry_after_s;     /* TG_ERR_RETRY only, >= 1 */
    char description[TG_DESC_SIZE]; /* Telegram description (truncated on a UTF-8 boundary) */
    cJSON *root;                /* TG_OK only: the parsed body, owned; free with tg_resp_free */
    const cJSON *result;        /* TG_OK only: "result" inside root */
    int64_t first_update_id;    /* TG_ERR_TOO_LARGE only, set by the transport (tgapi.c):
                                   tg_updates_first_id of the body's start, else 0 */
    size_t body_len;            /* body size in bytes (Content-Length when it was too large) */
} tg_resp_t;

/* Classify one HTTP response. len > max_len gives TG_ERR_TOO_LARGE without looking at body
 * (which may then be NULL). ok:true needs a "result"; ok:false maps error_code (or the
 * HTTP status when it is missing) 401/404 -> AUTH, 429 -> RETRY (parameters.retry_after,
 * default TG_RETRY_DEFAULT_S), >= 500 -> SERVER, else API. A body that is not a JSON object
 * with a boolean "ok" is classified from the HTTP status the same way, with SERVER for
 * anything that is not 401/404/429. *out is always fully written. */
#define TG_RETRY_DEFAULT_S 5
tg_status_t tg_parse_response(int http_status, const char *body, size_t len, size_t max_len,
                              tg_resp_t *out);

/* Free out->root (safe on a zeroed or already freed tg_resp_t). */
void tg_resp_free(tg_resp_t *r);

/* Length of UTF-8 s[0..len) in UTF-16 code units (Telegram entity offsets and lengths are
 * UTF-16). Returns TG_UTF16_INVALID for malformed UTF-8: stray or missing continuation
 * bytes, overlong forms, surrogates (U+D800..DFFF), code points above U+10FFFF. */
#define TG_UTF16_INVALID SIZE_MAX
size_t tg_utf16_len(const char *s, size_t len);

/* Convert the byte range [byte_off, byte_off + byte_len) of UTF-8 text[0..len) into a
 * UTF-16 offset and length. False if the text is malformed or the range is out of bounds
 * or does not start and end on character boundaries. */
bool tg_utf16_span(const char *text, size_t len, size_t byte_off, size_t byte_len,
                   size_t *off16, size_t *len16);

/* sendMessage body, no parse_mode. With an actor: text "[<actor>] <text>" and one bold
 * entity over "[<actor>]". With actor NULL: plain text, no entities (hint reply, online
 * notice, pairing confirmation). reply_to > 0 adds reply_parameters {message_id,
 * allow_sending_without_reply: true}, so a deleted original does not lose the message.
 * *json is a malloc'd NUL-terminated string (free with cJSON_free).
 * TG_ERR_TOO_LONG if the whole message text is over TG_TEXT_MAX_UTF16 UTF-16 units;
 * TG_ERR_BAD_ARG for an invalid actor, malformed UTF-8, an empty message, reply_to < 0 or
 * |chat_id| >= 2^53; TG_ERR_NO_MEM. The 4096-byte text limit of `tg send` is the
 * caller's (README 2.1). */
tg_status_t tg_build_send_message(int64_t chat_id, const char *actor, const char *text,
                                  int64_t reply_to, char **json);

/* getUpdates body {"offset":N,"limit":L,"timeout":T,"allowed_updates":["message"]}; the
 * offset is omitted when 0. Returns a malloc'd string (cJSON_free) or NULL. */
char *tg_build_get_updates(int64_t offset, unsigned limit, unsigned timeout_s);

/* Integer member obj[key] (a JSON number with an integral value below 2^53). */
bool tg_json_i64(const cJSON *obj, const char *key, int64_t *out);

/* getMe result -> "username" (copied, NUL-terminated; false if missing or too long). */
bool tg_parse_bot_username(const cJSON *result, char *out, size_t cap);

/* sendMessage result -> "message_id". */
bool tg_parse_message_id(const cJSON *result, int64_t *msg_id);

/* The first "update_id" in a getUpdates body that may be cut off (a response over the
 * cap). Lets a poller skip an update it cannot accept (offset = id + 1). False if
 * no complete number follows the first "update_id" key. */
bool tg_updates_first_id(const char *body, size_t len, int64_t *update_id);

/* How long to wait before the next request after `st`. RETRY: retry_after_s (at least
 * 1 s). NET, TIMEOUT, SERVER: TG_BACKOFF_MIN_MS doubled per consecutive failure (attempt
 * 0 = first failure), capped at TG_BACKOFF_MAX_MS. AUTH: TG_AUTH_RETRY_MS. Anything else
 * (OK, API, TOO_LARGE, NO_TOKEN, BAD_ARG, NO_MEM, TOO_LONG): 0, the caller decides. */
#define TG_BACKOFF_MIN_MS 1000u
#define TG_BACKOFF_MAX_MS 30000u
#define TG_AUTH_RETRY_MS  60000u
uint32_t tg_retry_delay_ms(tg_status_t st, uint32_t retry_after_s, unsigned attempt);

#endif /* EMBPET_TGAPI_CORE_H */
