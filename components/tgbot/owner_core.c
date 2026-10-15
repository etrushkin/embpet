#include "owner_core.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "tgapi_core.h"

#define CODE_LIMIT 4294000000u          /* largest multiple of 10^6 that fits 32 bits */

bool pair_code_make(uint32_t r, char out[PAIR_CODE_BUF])
{
    if (r >= CODE_LIMIT) {
        return false;
    }
    r %= 1000000u;
    for (int k = PAIR_CODE_LEN - 1; k >= 0; k--) {
        out[k] = (char)('0' + r % 10);
        r /= 10;
    }
    out[PAIR_CODE_LEN] = '\0';
    return true;
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool pair_parse_start(const char *text, const char *bot, char code[PAIR_CODE_BUF])
{
    static const char cmd[] = "/start";
    if (text == NULL || strncmp(text, cmd, sizeof(cmd) - 1) != 0) {
        return false;
    }
    const char *p = text + sizeof(cmd) - 1;
    if (*p == '@') {                    /* "/start@name": must be this bot */
        const char *name = ++p;
        while (*p != '\0' && !is_space(*p)) {
            p++;
        }
        size_t n = (size_t)(p - name);
        if (bot == NULL || n == 0 || strlen(bot) != n || strncasecmp(name, bot, n) != 0) {
            return false;
        }
    }
    if (!is_space(*p)) {
        return false;                   /* "/starts", "/start@" at the end, bare "/start" */
    }
    while (is_space(*p)) {
        p++;
    }
    size_t n = 0;
    while (p[n] >= '0' && p[n] <= '9') {
        n++;
    }
    if (n != PAIR_CODE_LEN) {
        return false;
    }
    const char *end = p + PAIR_CODE_LEN;
    while (is_space(*end)) {
        end++;
    }
    if (*end != '\0') {
        return false;
    }
    memcpy(code, p, PAIR_CODE_LEN);
    code[PAIR_CODE_LEN] = '\0';
    return true;
}

void pair_init(pair_t *p)
{
    memset(p, 0, sizeof(*p));
}

void pair_start(pair_t *p, const char *code, uint64_t now_ms, uint32_t ttl_s)
{
    p->active = true;
    memcpy(p->code, code, PAIR_CODE_BUF);
    p->expires_ms = now_ms + (uint64_t)ttl_s * 1000u;
    p->wrong = 0;
}

void pair_cancel(pair_t *p)
{
    memset(p, 0, sizeof(*p));
}

pair_result_t pair_check(pair_t *p, const char *text, const char *bot, uint64_t now_ms)
{
    if (!p->active) {
        return PAIR_PASS;
    }
    if (now_ms >= p->expires_ms) {
        pair_cancel(p);
        return PAIR_PASS;
    }
    char code[PAIR_CODE_BUF];
    if (!pair_parse_start(text, bot, code)) {
        return PAIR_PASS;
    }
    unsigned diff = 0;                  /* no early exit on the first differing digit */
    for (size_t k = 0; k < PAIR_CODE_LEN; k++) {
        diff |= (unsigned)(code[k] ^ p->code[k]);
    }
    if (diff == 0) {
        return PAIR_MATCH;
    }
    if (++p->wrong >= PAIR_MAX_WRONG) {
        pair_cancel(p);
        return PAIR_LOCKED;
    }
    return PAIR_WRONG;
}

uint32_t pair_left_s(const pair_t *p, uint64_t now_ms)
{
    if (!p->active || now_ms >= p->expires_ms) {
        return 0;
    }
    return (uint32_t)((p->expires_ms - now_ms + 999u) / 1000u);
}

bool owner_update_text(const cJSON *update, int64_t *chat, const char **text)
{
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(update, "message");
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(m, "text");
    if (!cJSON_IsObject(m) || !tg_json_i64(cJSON_GetObjectItemCaseSensitive(m, "chat"), "id", chat)) {
        return false;
    }
    *text = cJSON_IsString(t) && t->valuestring != NULL ? t->valuestring : "";
    return true;
}

bool owner_parse_chat(const char *s, int64_t *out)
{
    bool neg = *s == '-';
    const char *d = neg ? s + 1 : s;
    size_t n = strlen(d);
    if (n == 0 || n > 15) {
        return false;
    }
    int64_t v = 0;
    for (size_t k = 0; k < n; k++) {
        if (d[k] < '0' || d[k] > '9') {
            return false;
        }
        v = v * 10 + (d[k] - '0');
    }
    if (v == 0) {
        return false;
    }
    *out = neg ? -v : v;
    return true;
}

size_t notice_format(char *out, size_t cap, const char *fw, const char *reason)
{
    int n = snprintf(out, cap, "embpet online \xC2\xB7 fw %s \xC2\xB7 reset %s", fw, reason);
    return n < 0 ? 0 : (size_t)n;
}
