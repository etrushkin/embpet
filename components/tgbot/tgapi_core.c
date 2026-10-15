#include "tgapi_core.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "prefix_core.h"

#define I64_JSON_MAX 9007199254740992.0     /* 2^53: larger integers lose precision as double */
#define RETRY_AFTER_MAX_S 86400u

static const char *const s_status_names[] = {
    [TG_OK] = "ok",
    [TG_ERR_NET] = "net",
    [TG_ERR_TIMEOUT] = "timeout",
    [TG_ERR_SERVER] = "server",
    [TG_ERR_AUTH] = "auth",
    [TG_ERR_RETRY] = "retry",
    [TG_ERR_API] = "api",
    [TG_ERR_TOO_LARGE] = "too_large",
    [TG_ERR_NO_TOKEN] = "no_token",
    [TG_ERR_BAD_ARG] = "bad_arg",
    [TG_ERR_NO_MEM] = "no_mem",
    [TG_ERR_TOO_LONG] = "too_long",
};

const char *tg_status_name(tg_status_t st)
{
    if ((unsigned)st < sizeof(s_status_names) / sizeof(s_status_names[0])) {
        return s_status_names[st];
    }
    return "?";
}

/* ------------------------------------------------------------ responses */

/* Copy src, truncated to the buffer on a UTF-8 character boundary. */
static void copy_desc(char dst[TG_DESC_SIZE], const char *src)
{
    size_t n = strlen(src);
    if (n >= TG_DESC_SIZE) {
        n = TG_DESC_SIZE - 1;
        /* src[n] is the first byte dropped; if it continues a character, drop all of it */
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static tg_status_t classify(int code, tg_resp_t *out)
{
    out->error_code = code;
    if (code == 401 || code == 404) {
        return TG_ERR_AUTH;
    }
    if (code == 429) {
        if (out->retry_after_s == 0) {
            out->retry_after_s = TG_RETRY_DEFAULT_S;
        }
        return TG_ERR_RETRY;
    }
    if (code >= 500 || code <= 0) {
        return TG_ERR_SERVER;
    }
    return TG_ERR_API;
}

tg_status_t tg_parse_response(int http_status, const char *body, size_t len, size_t max_len,
                              tg_resp_t *out)
{
    memset(out, 0, sizeof(*out));
    out->http_status = http_status;
    out->body_len = len;
    if (len > max_len) {
        out->error_code = http_status;
        return TG_ERR_TOO_LARGE;
    }
    cJSON *root = body != NULL ? cJSON_ParseWithLength(body, len) : NULL;
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    if (!cJSON_IsObject(root) || !cJSON_IsBool(ok)) {
        cJSON_Delete(root);
        tg_status_t st = classify(http_status, out);
        return st == TG_ERR_API ? TG_ERR_SERVER : st;
    }
    if (cJSON_IsTrue(ok)) {
        const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
        if (result == NULL) {
            cJSON_Delete(root);
            out->error_code = http_status;
            return TG_ERR_SERVER;
        }
        out->root = root;
        out->result = result;
        return TG_OK;
    }

    int64_t code = http_status;
    tg_json_i64(root, "error_code", &code);
    const cJSON *desc = cJSON_GetObjectItemCaseSensitive(root, "description");
    if (cJSON_IsString(desc)) {
        copy_desc(out->description, desc->valuestring);
    }
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "parameters");
    int64_t ra = 0;
    if (tg_json_i64(params, "retry_after", &ra) && ra > 0) {
        out->retry_after_s = ra > RETRY_AFTER_MAX_S ? RETRY_AFTER_MAX_S : (uint32_t)ra;
    }
    cJSON_Delete(root);
    tg_status_t st = classify(code >= INT_MIN && code <= INT_MAX ? (int)code : 0, out);
    if (st != TG_ERR_RETRY) {
        out->retry_after_s = 0;
    }
    return st;
}

void tg_resp_free(tg_resp_t *r)
{
    if (r != NULL) {
        cJSON_Delete(r->root);
        r->root = NULL;
        r->result = NULL;
    }
}

/* ------------------------------------------------------------ UTF-16 */

/* Decode one UTF-8 character at s[0..n), n >= 1. Returns its length in bytes (1..4) and
 * its UTF-16 length in *units, or 0 if it is malformed. */
static size_t utf8_char(const unsigned char *s, size_t n, size_t *units)
{
    unsigned char c = s[0];
    size_t len;
    uint32_t cp;
    uint32_t min;
    if (c < 0x80) {
        *units = 1;
        return 1;
    }
    if (c >= 0xC2 && c <= 0xDF) {
        len = 2;
        cp = c & 0x1Fu;
        min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
        cp = c & 0x0Fu;
        min = 0x800;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4;
        cp = c & 0x07u;
        min = 0x10000;
    } else {
        return 0;               /* continuation byte, C0/C1 (always overlong) or F5..FF */
    }
    if (n < len) {
        return 0;
    }
    for (size_t i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (s[i] & 0x3Fu);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0;
    }
    *units = cp >= 0x10000 ? 2 : 1;
    return len;
}

size_t tg_utf16_len(const char *s, size_t len)
{
    if (s == NULL) {
        return len == 0 ? 0 : TG_UTF16_INVALID;
    }
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    size_t units = 0;
    while (i < len) {
        size_t u;
        size_t n = utf8_char(p + i, len - i, &u);
        if (n == 0) {
            return TG_UTF16_INVALID;
        }
        i += n;
        units += u;
    }
    return units;
}

bool tg_utf16_span(const char *text, size_t len, size_t byte_off, size_t byte_len,
                   size_t *off16, size_t *len16)
{
    if (text == NULL || off16 == NULL || len16 == NULL || byte_off > len ||
        byte_len > len - byte_off) {
        return false;
    }
    const unsigned char *p = (const unsigned char *)text;
    size_t end = byte_off + byte_len;
    size_t start16 = 0;
    size_t end16 = 0;
    bool got_start = false;
    bool got_end = false;
    size_t i = 0;
    size_t units = 0;
    for (;;) {
        if (i == byte_off) {
            got_start = true;
            start16 = units;
        }
        if (i == end) {
            got_end = true;
            end16 = units;
        }
        if (i >= len) {
            break;
        }
        size_t u;
        size_t n = utf8_char(p + i, len - i, &u);
        if (n == 0) {
            return false;
        }
        i += n;
        units += u;
    }
    if (!got_start || !got_end) {
        return false;               /* a boundary fell inside a character */
    }
    *off16 = start16;
    *len16 = end16 - start16;
    return true;
}

/* ------------------------------------------------------------ requests */

static bool i64_json_ok(int64_t v)
{
    return v > -(int64_t)I64_JSON_MAX && v < (int64_t)I64_JSON_MAX;
}

tg_status_t tg_build_send_message(int64_t chat_id, const char *actor, const char *text,
                                  int64_t reply_to, char **json)
{
    if (json == NULL) {
        return TG_ERR_BAD_ARG;
    }
    *json = NULL;
    if (text == NULL || (actor != NULL && !actor_id_valid(actor, strlen(actor))) ||
        (actor == NULL && text[0] == '\0') ||
        chat_id == 0 || !i64_json_ok(chat_id) || reply_to < 0 || !i64_json_ok(reply_to)) {
        return TG_ERR_BAD_ARG;
    }
    size_t alen = actor != NULL ? strlen(actor) : 0;
    size_t tlen = strlen(text);
    size_t tag = actor != NULL ? alen + 2 : 0;          /* "[A]" */
    size_t full_len = (actor != NULL ? tag + 1 : 0) + tlen;   /* "[A] text" or "text" */
    char *full = malloc(full_len + 1);
    if (full == NULL) {
        return TG_ERR_NO_MEM;
    }
    if (actor != NULL) {
        full[0] = '[';
        memcpy(full + 1, actor, alen);
        full[1 + alen] = ']';
        full[tag] = ' ';
    }
    memcpy(full + full_len - tlen, text, tlen);
    full[full_len] = '\0';

    size_t off16;
    size_t len16;
    size_t total16 = tg_utf16_len(full, full_len);
    if (total16 == TG_UTF16_INVALID || !tg_utf16_span(full, full_len, 0, tag, &off16, &len16)) {
        free(full);
        return TG_ERR_BAD_ARG;                  /* the text is not valid UTF-8 */
    }
    if (total16 > TG_TEXT_MAX_UTF16) {
        free(full);
        return TG_ERR_TOO_LONG;
    }

    cJSON *root = cJSON_CreateObject();
    bool ok = root != NULL &&
              cJSON_AddNumberToObject(root, "chat_id", (double)chat_id) != NULL &&
              cJSON_AddStringToObject(root, "text", full) != NULL;
    free(full);
    if (ok && actor != NULL) {
        cJSON *ents = cJSON_AddArrayToObject(root, "entities");
        cJSON *ent = ents != NULL ? cJSON_CreateObject() : NULL;
        ok = ent != NULL && cJSON_AddItemToArray(ents, ent) &&
             cJSON_AddStringToObject(ent, "type", "bold") != NULL &&
             cJSON_AddNumberToObject(ent, "offset", (double)off16) != NULL &&
             cJSON_AddNumberToObject(ent, "length", (double)len16) != NULL;
    }
    if (ok && reply_to > 0) {
        cJSON *rp = cJSON_AddObjectToObject(root, "reply_parameters");
        ok = rp != NULL && cJSON_AddNumberToObject(rp, "message_id", (double)reply_to) != NULL &&
             cJSON_AddTrueToObject(rp, "allow_sending_without_reply") != NULL;
    }
    if (ok) {
        *json = cJSON_PrintUnformatted(root);
    }
    cJSON_Delete(root);
    return *json != NULL ? TG_OK : TG_ERR_NO_MEM;
}

char *tg_build_get_updates(int64_t offset, unsigned limit, unsigned timeout_s)
{
    if (!i64_json_ok(offset)) {
        return NULL;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    bool ok = offset == 0 || cJSON_AddNumberToObject(root, "offset", (double)offset) != NULL;
    ok = ok && cJSON_AddNumberToObject(root, "limit", limit) != NULL &&
         cJSON_AddNumberToObject(root, "timeout", timeout_s) != NULL;
    cJSON *types = ok ? cJSON_AddArrayToObject(root, "allowed_updates") : NULL;
    cJSON *msg = types != NULL ? cJSON_CreateString("message") : NULL;
    char *out = NULL;
    if (msg != NULL) {
        cJSON_AddItemToArray(types, msg);
        out = cJSON_PrintUnformatted(root);
    }
    cJSON_Delete(root);
    return out;
}

/* ------------------------------------------------------------ result fields */

bool tg_json_i64(const cJSON *obj, const char *key, int64_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(v)) {
        return false;
    }
    double d = v->valuedouble;
    if (!(d > -I64_JSON_MAX && d < I64_JSON_MAX)) {
        return false;
    }
    int64_t i = (int64_t)d;
    if ((double)i != d) {
        return false;
    }
    *out = i;
    return true;
}

bool tg_parse_bot_username(const cJSON *result, char *out, size_t cap)
{
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(result, "username");
    if (!cJSON_IsString(u) || out == NULL || strlen(u->valuestring) >= cap) {
        return false;
    }
    strcpy(out, u->valuestring);
    return true;
}

bool tg_parse_message_id(const cJSON *result, int64_t *msg_id)
{
    return tg_json_i64(result, "message_id", msg_id);
}

bool tg_updates_first_id(const char *body, size_t len, int64_t *update_id)
{
    static const char key[] = "\"update_id\"";
    const size_t klen = sizeof(key) - 1;
    if (body == NULL || update_id == NULL) {
        return false;
    }
    size_t i = 0;
    for (; i + klen <= len; i++) {
        if (memcmp(body + i, key, klen) == 0) {
            break;
        }
    }
    if (i + klen > len) {
        return false;
    }
    i += klen;
    while (i < len && (body[i] == ' ' || body[i] == '\t' || body[i] == '\r' || body[i] == '\n')) {
        i++;
    }
    if (i >= len || body[i] != ':') {
        return false;
    }
    i++;
    while (i < len && (body[i] == ' ' || body[i] == '\t' || body[i] == '\r' || body[i] == '\n')) {
        i++;
    }
    int64_t v = 0;
    size_t nd = 0;
    for (; i < len && body[i] >= '0' && body[i] <= '9'; i++, nd++) {
        if (nd >= 15) {
            return false;       /* update ids are far below 10^15 */
        }
        v = v * 10 + (body[i] - '0');
    }
    if (nd == 0 || i >= len) {
        return false;           /* no digits, or the body ends inside the number */
    }
    *update_id = v;
    return true;
}

/* ------------------------------------------------------------ retry */

uint32_t tg_retry_delay_ms(tg_status_t st, uint32_t retry_after_s, unsigned attempt)
{
    switch (st) {
    case TG_ERR_RETRY:
        if (retry_after_s == 0) {
            retry_after_s = 1;
        }
        if (retry_after_s > RETRY_AFTER_MAX_S) {
            retry_after_s = RETRY_AFTER_MAX_S;
        }
        return retry_after_s * 1000u;
    case TG_ERR_NET:
    case TG_ERR_TIMEOUT:
    case TG_ERR_SERVER: {
        uint32_t ms = TG_BACKOFF_MIN_MS;
        while (attempt-- > 0 && ms < TG_BACKOFF_MAX_MS) {
            ms *= 2;
        }
        return ms < TG_BACKOFF_MAX_MS ? ms : TG_BACKOFF_MAX_MS;
    }
    case TG_ERR_AUTH:
        return TG_AUTH_RETRY_MS;
    default:
        return 0;
    }
}
