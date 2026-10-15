#include "bridge_core.h"

#include <string.h>

static const cJSON *member(const cJSON *obj, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(obj, key);
}

static const char *string_member(const cJSON *obj, const char *key)
{
    const cJSON *s = member(obj, key);
    return cJSON_IsString(s) && s->valuestring != NULL ? s->valuestring : NULL;
}

int64_t bridge_bot_id(const char *token)
{
    int64_t id = 0;
    size_t n = 0;
    for (; token != NULL && token[n] >= '0' && token[n] <= '9'; n++) {
        if (n == 15) {
            return 0;                   /* over 2^53: not a Telegram user id */
        }
        id = id * 10 + (token[n] - '0');
    }
    return n > 0 && token[n] == ':' ? id : 0;
}

bridge_route_kind_t bridge_route(const cJSON *update, int64_t owner, int64_t bot_id,
                                 const pending_table_t *t, bridge_route_t *out)
{
    memset(out, 0, sizeof(*out));
    out->text = "";
    const cJSON *m = member(update, "message");
    int64_t chat = 0;
    if (owner == 0 || !cJSON_IsObject(m) || !tg_json_i64(member(m, "chat"), "id", &chat) ||
        chat != owner || !tg_json_i64(m, "message_id", &out->msg)) {
        return BRIDGE_IGNORE;
    }
    tg_json_i64(m, "date", &out->date);

    const cJSON *rt = member(m, "reply_to_message");
    if (!cJSON_IsObject(rt) || !tg_json_i64(rt, "message_id", &out->to)) {
        out->to = 0;
        return BRIDGE_HINT;
    }
    if (t != NULL && pending_lookup(t, out->to, out->actor)) {
        out->from_table = true;
    } else {
        const char *quoted = string_member(rt, "text");
        int64_t author = 0;
        if (bot_id == 0 || !tg_json_i64(member(rt, "from"), "id", &author) ||
            author != bot_id || quoted == NULL ||
            !prefix_parse_actor(quoted, strlen(quoted), out->actor)) {
            out->to = 0;
            return BRIDGE_HINT;
        }
    }
    const char *text = string_member(m, "text");
    if (text == NULL) {
        text = string_member(m, "caption");
    }
    out->text = text != NULL ? text : "";
    return BRIDGE_REPLY;
}

bridge_text_t bridge_check_text(const char *actor, const char *text, size_t max_bytes)
{
    size_t len = strlen(text);
    if (len > max_bytes) {
        return BRIDGE_TEXT_TOO_LONG;
    }
    size_t units = tg_utf16_len(text, len);
    if (units == TG_UTF16_INVALID) {
        return BRIDGE_TEXT_BAD_UTF8;
    }
    /* "[" actor "] ": the actor is ASCII, one unit per byte */
    return strlen(actor) + 3 + units > TG_TEXT_MAX_UTF16 ? BRIDGE_TEXT_TOO_LONG : BRIDGE_TEXT_OK;
}

const char *bridge_send_err(tg_status_t st, int error_code, const char *description)
{
    switch (st) {
    case TG_ERR_NET:
    case TG_ERR_SERVER:
        return "offline";
    case TG_ERR_TIMEOUT:
        return "timeout";
    case TG_ERR_AUTH:
        return "auth_error";
    case TG_ERR_NO_TOKEN:
        return "not_configured";
    case TG_ERR_RETRY:
        return "busy";
    case TG_ERR_TOO_LONG:
        return "too_long";
    case TG_ERR_BAD_ARG:
        return "bad_args";
    case TG_ERR_API:
        if (error_code == 403 ||
            (error_code == 400 && description != NULL &&
             strstr(description, "chat not found") != NULL)) {
            return "not_configured";
        }
        return "bad_args";
    default:                    /* OK (not an error), NO_MEM, TOO_LARGE */
        return "internal";
    }
}
