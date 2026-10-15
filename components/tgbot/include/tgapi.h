#ifndef EMBPET_TGAPI_H
#define EMBPET_TGAPI_H
/* Telegram Bot API client (README 2.1) on esp_http_client with the IDF certificate bundle.
 *
 * Connections: a tgapi_conn_t is one keep-alive HTTPS session to api.telegram.org. Two are
 * meant to exist: the poll connection (getUpdates; HTTP timeout = poll timeout + 10 s),
 * owned by the tg_poll task, and the send connection (everything else; HTTP timeout
 * CONFIG_EMBPET_TG_SEND_TIMEOUT_MS), owned by the tg_send task. A connection is not
 * thread-safe: only its owner task may use it. The timeout applies to each network step
 * (connect, TLS handshake, write, each read), so one call can take several times the
 * timeout, and twice that when a dead kept-alive session is replaced (see tgapi_call): a
 * caller with a deadline (`tg send`: 10 s) must enforce it itself. Sessions idle for more
 * than 60 s are reconnected rather than reused, and TCP keepalive is on. Close both
 * connections on Wi-Fi down (tgapi_conn_close) to give their TLS memory back.
 *
 * Methods: pass only real Bot API method names. Telegram answers 404 for an unknown method
 * as for a bad token, and 404 is mapped to auth_error.
 *
 * Token: read from cfg on every call, so a new token applies to the next request. It goes
 * into the URL path only. esp_http_client prints the request line at DEBUG, so
 * esp_http_client, tcp_transport and esp-tls are compiled with LOG_LOCAL_LEVEL = WARN
 * (top-level CMakeLists.txt) and their tags are also capped at runtime (tgapi_log_guard).
 * The URL, token included, stays in the esp_http_client handle's heap (not wiped) for as
 * long as the connection exists.
 *
 * State: every call result updates the shared tg state (state_set_tg): ok -> online (with
 * the bot username from the latest getMe), network / timeout / server -> offline, 401/404
 * -> auth_error, no token -> unconfigured. A result is ignored if the token changed while
 * the call ran. Wi-Fi going down turns online into offline; a token change gives offline
 * (or unconfigured when cleared). The `tg` event {state, detail} is emitted on state or bot
 * changes (README 3.3). */

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "tgapi_core.h"

typedef enum {
    TGAPI_CONN_POLL,
    TGAPI_CONN_SEND,
} tgapi_conn_kind_t;

typedef struct tgapi_conn tgapi_conn_t;

/* Subscribe to state (tg event, Wi-Fi down) and cfg (token changes), set the initial tg
 * state from the stored token and cap the HTTP log tags. Call once from app_main after
 * cfg_init() and state_init(), before any other tgapi function. */
esp_err_t tgapi_init(void);

/* Cap the esp_http_client / tcp_transport / esp-tls log tags at min(level, WARN). Call
 * after every esp_log_level_set("*", level): a wildcard set clears per-tag levels. */
void tgapi_log_guard(esp_log_level_t level);

/* Create a connection (no network traffic yet). NULL when out of memory. */
tgapi_conn_t *tgapi_conn_create(tgapi_conn_kind_t kind);

/* Drop the TLS session (frees its 35-45 KB); the next call reconnects. */
void tgapi_conn_close(tgapi_conn_t *c);

/* POST https://api.telegram.org/bot<token>/<method> with a JSON body (NULL = "{}") and
 * classify the answer (tg_parse_response). *out is always written: on TG_OK out->result
 * holds the result and the caller frees it with tg_resp_free; on errors out carries
 * error_code, description and retry_after_s and owns nothing.
 * The response body is read into a buffer sized by Content-Length (at most TG_RESP_MAX,
 * freed before returning); a larger body closes the session and gives TG_ERR_TOO_LARGE.
 * A failure on a kept-alive session that the server had already closed (connection reset
 * or closed before any response header) is retried once on a fresh session; a timeout is
 * never retried, so a request is not sent twice because the server was slow. The retry
 * can still duplicate a sendMessage if the server processed the request and then reset
 * the connection before answering (rare; owners may see a message twice). */
tg_status_t tgapi_call(tgapi_conn_t *c, const char *method, const char *body, tg_resp_t *out);

/* The calls below use tgapi_call and free the response themselves; `err` (may be NULL)
 * receives the error details (root is always NULL on return). */

/* getMe: copies the bot username (STATE_BOT_LEN bytes suffice) and remembers it for the
 * online state. TG_ERR_SERVER if the result has no username. */
tg_status_t tgapi_get_me(tgapi_conn_t *c, char *bot, size_t cap, tg_resp_t *err);

/* getMe with a candidate token that is not stored (`tg token`): TG_OK and the bot username
 * if Telegram accepts it, TG_ERR_AUTH if it rejects it, other statuses as for any call.
 * Changes no state and emits no event; closes the session afterwards and points the
 * connection's URL back at the stored token (none: at the bare host), so the candidate does not
 * stay in the handle. */
tg_status_t tgapi_check_token(tgapi_conn_t *c, const char *token, char *bot, size_t cap,
                              tg_resp_t *err);

/* deleteWebhook (README 4: long polling fails while a webhook exists); pending updates
 * are kept. */
tg_status_t tgapi_delete_webhook(tgapi_conn_t *c, tg_resp_t *err);

/* sendMessage "[actor] text" with the bold tag entity (tg_build_send_message), or plain
 * text with actor NULL (hint reply, online notice, pairing confirmation). reply_to > 0
 * sends it as a reply (still sent if the original is gone). On TG_OK *msg_id is the new
 * message's id. TG_ERR_TOO_LONG (console: too_long) if the whole text is over 4096 UTF-16
 * units; nothing is sent then. */
tg_status_t tgapi_send_message(tgapi_conn_t *c, int64_t chat_id, const char *actor,
                               const char *text, int64_t reply_to, int64_t *msg_id,
                               tg_resp_t *err);

/* getUpdates(offset, limit TG_UPDATES_LIMIT, timeout CONFIG_EMBPET_TG_POLL_TIMEOUT_S,
 * allowed_updates ["message"]). Use the poll connection. On TG_OK out->result is the
 * update array; the caller frees it with tg_resp_free. On TG_ERR_TOO_LARGE (over
 * TG_RESP_MAX) the body is dropped: retry with tgapi_get_updates_one. */
tg_status_t tgapi_get_updates(tgapi_conn_t *c, int64_t offset, tg_resp_t *out);

/* getUpdates(offset, limit 1, timeout 0) with the larger cap TG_RESP_MAX_ONE, which fits
 * the largest possible owner reply quoting the largest possible bot message (docs/PLAN.md
 * 1.2). If it is still TG_ERR_TOO_LARGE (not expected from the Bot API), a WARN is logged
 * and the poller has to skip the update: offset = out->first_update_id + 1 (the id seen at
 * the start of the body; 0 if it could not be read, then retry later). */
tg_status_t tgapi_get_updates_one(tgapi_conn_t *c, int64_t offset, tg_resp_t *out);

#endif /* EMBPET_TGAPI_H */
