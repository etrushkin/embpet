#ifndef EMBPET_BRIDGE_CORE_H
#define EMBPET_BRIDGE_CORE_H
/* Pure logic (no ESP-IDF includes) of the Telegram bridge (README 2.1): routing of one
 * getUpdates update, the `tg send` text check and the mapping of a failed sendMessage to a
 * console error code. Uses cJSON. Used by bridge.c and unit-tested on the host. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "pending_core.h"
#include "prefix_core.h"
#include "tgapi_core.h"

/* The bot's answer to an owner message that is not a routable Reply (README 2.1). */
#define BRIDGE_HINT_TEXT "Use Reply on an actor's message so I know who it's for."

typedef enum {
    BRIDGE_IGNORE = 0,  /* not a message from the owner chat (or no owner set): confirm only */
    BRIDGE_REPLY,       /* owner replied to an actor's message: emit a `reply` event */
    BRIDGE_HINT,        /* owner message that is no Reply, or a Reply that cannot be routed:
                           answer with BRIDGE_HINT_TEXT, no event */
} bridge_route_kind_t;

typedef struct {
    int64_t msg;                /* the owner's message_id (REPLY, HINT) */
    int64_t to;                 /* the replied-to message_id (REPLY) */
    int64_t date;               /* unix time of the owner's message, 0 if missing */
    const char *text;           /* REPLY: "text", else "caption" (media), else ""; points
                                   into the update */
    char actor[ACTOR_BUF_SIZE]; /* REPLY */
    bool from_table;            /* REPLY: src "table" (true) or "prefix" (false) */
} bridge_route_t;

/* Decide what to do with one update of a getUpdates result:
 *  - no "message", a chat other than `owner`, owner 0, or no message_id: IGNORE;
 *  - message.reply_to_message.message_id found in the pending table t (answered entries
 *    still route; t may be NULL): REPLY from the table;
 *  - else a replied-to message written by this bot (from.id == bot_id, see bridge_bot_id)
 *    whose text starts with a valid "[A]" prefix: REPLY from the prefix (after a reboot or
 *    an eviction); bot_id 0 (unknown) disables this fallback;
 *  - anything else from the owner (no Reply, Reply to an untagged message, to the owner's
 *    own or to another bot's message): HINT.
 * Changes nothing: the caller marks the message answered. *out is fully written. */
bridge_route_kind_t bridge_route(const cJSON *update, int64_t owner, int64_t bot_id,
                                 const pending_table_t *t, bridge_route_t *out);

/* The bot's user id: the digits before ':' of a bot token ("123456:AA..." -> 123456), as
 * Telegram puts it in from.id of the bot's messages. 0 if the token is not of that form. */
int64_t bridge_bot_id(const char *token);

typedef enum {
    BRIDGE_TEXT_OK = 0,
    BRIDGE_TEXT_TOO_LONG,       /* over max_bytes, or "[actor] text" over TG_TEXT_MAX_UTF16 */
    BRIDGE_TEXT_BAD_UTF8,
} bridge_text_t;

/* `tg send` text check before anything is queued (README 2.1): at most max_bytes bytes of
 * valid UTF-8, and the whole "[<actor>] <text>" at most TG_TEXT_MAX_UTF16 UTF-16 units.
 * actor must be a valid actor id. Empty text is accepted ("[A] " is sent). */
bridge_text_t bridge_check_text(const char *actor, const char *text, size_t max_bytes);

/* Console error code (the CONSOLE_ERR_* strings, README 3.3) for a sendMessage that ended
 * with st != TG_OK; error_code and description are Telegram's (tg_resp_t):
 *   NET, SERVER -> offline; TIMEOUT -> timeout; AUTH -> auth_error; NO_TOKEN ->
 *   not_configured; RETRY (429) -> busy; TOO_LONG -> too_long; BAD_ARG -> bad_args;
 *   API: 403 (bot blocked by the owner) or 400 "chat not found" -> not_configured, any
 *   other -> bad_args; NO_MEM, TOO_LARGE -> internal. */
const char *bridge_send_err(tg_status_t st, int error_code, const char *description);

#endif /* EMBPET_BRIDGE_CORE_H */
