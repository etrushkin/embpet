#include "cfg_core.h"

#include <ctype.h>
#include <string.h>

static const char *const key_names[CFG_KEY_COUNT] = {
    [CFG_KEY_WIFI_SSID] = "wifi_ssid",
    [CFG_KEY_WIFI_PASS] = "wifi_pass",
    [CFG_KEY_WIFI_TXQ] = "wifi_txq",
    [CFG_KEY_TG_TOKEN] = "tg_token",
    [CFG_KEY_TG_OWNER] = "tg_owner",
    [CFG_KEY_OUT_BOOT] = "out_boot",
    [CFG_KEY_IN_DEB_10] = "in_deb_10",
    [CFG_KEY_IN_DEB_20] = "in_deb_20",
    [CFG_KEY_IN_DEB_21] = "in_deb_21",
};

const char *cfg_key_name(cfg_key_t k)
{
    return ((unsigned)k < CFG_KEY_COUNT) ? key_names[k] : "?";
}

cfg_ver_action_t cfg_schema_decide(bool have_ver, uint32_t stored)
{
    if (!have_ver) {
        return CFG_VER_STAMP;
    }
    return (stored == CFG_SCHEMA_VERSION) ? CFG_VER_KEEP : CFG_VER_ERASE;
}

bool cfg_txq_valid(int v)
{
    return v >= CFG_TXQ_MIN && v <= CFG_TXQ_MAX;
}

int8_t cfg_txq_clamp(int v)
{
    if (v < CFG_TXQ_MIN) {
        return CFG_TXQ_MIN;
    }
    return (int8_t)((v > CFG_TXQ_MAX) ? CFG_TXQ_MAX : v);
}

bool cfg_debounce_valid(int ms)
{
    return ms >= 0 && ms <= CFG_DEBOUNCE_MAX_MS;
}

uint16_t cfg_debounce_clamp(int ms)
{
    if (ms < 0) {
        return 0;
    }
    return (uint16_t)((ms > CFG_DEBOUNCE_MAX_MS) ? CFG_DEBOUNCE_MAX_MS : ms);
}

bool cfg_debounce_key(int pin, cfg_key_t *key)
{
    switch (pin) {
    case 10: *key = CFG_KEY_IN_DEB_10; return true;
    case 20: *key = CFG_KEY_IN_DEB_20; return true;
    case 21: *key = CFG_KEY_IN_DEB_21; return true;
    default: return false;
    }
}

bool cfg_out_index_valid(int idx)
{
    return idx >= 0 && idx <= 7;
}

bool cfg_owner_valid(int64_t id)
{
    return id != 0;
}

bool cfg_ssid_valid(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    return n >= 1 && n <= CFG_SSID_MAX;
}

bool cfg_pass_valid(const char *s)
{
    if (!s) {
        return false;
    }
    size_t n = strlen(s);
    if (n == 64) {   /* raw PSK: exactly 64 hex digits */
        for (size_t i = 0; i < n; i++) {
            if (!isxdigit((unsigned char)s[i])) {
                return false;
            }
        }
        return true;
    }
    return n == 0 || (n >= 8 && n <= 63);
}

#define MASK_FALLBACK "***"
#define MASK_ELLIPSIS "\xE2\x80\xA6"
#define MASK_ID_MAX 12   /* bot ids are up to ~10 digits */

bool cfg_token_valid(const char *s)
{
    if (!s) {
        return false;
    }
    size_t n = strlen(s);
    if (n > CFG_TOKEN_MAX) {
        return false;
    }
    size_t id = 0;
    while (id < n && s[id] >= '0' && s[id] <= '9') {
        id++;
    }
    if (id < 1 || id > MASK_ID_MAX || id >= n || s[id] != ':' || n - id - 1 < CFG_MASK_MIN_SECRET) {
        return false;
    }
    for (size_t i = id + 1; i < n; i++) {
        char c = s[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

size_t cfg_mask_token(const char *token, char *out, size_t cap)
{
    char tmp[CFG_MASK_BUF_SIZE];
    size_t len = 0;

    if (token && token[0] != '\0') {
        size_t n = strlen(token);
        size_t id = 0;
        while (id < n && token[id] >= '0' && token[id] <= '9') {
            id++;
        }
        size_t secret = (id >= 1 && id <= MASK_ID_MAX && id < n && token[id] == ':') ? n - id - 1 : 0;
        if (secret >= CFG_MASK_MIN_SECRET) {
            memcpy(tmp, token, id + 1 + 2);            /* "<digits>:" + first 2 chars */
            len = id + 1 + 2;
            memcpy(tmp + len, MASK_ELLIPSIS, 3);
            len += 3;
            memcpy(tmp + len, token + n - 3, 3);       /* last 3 chars */
            len += 3;
        } else {
            memcpy(tmp, MASK_FALLBACK, sizeof(MASK_FALLBACK) - 1);
            len = sizeof(MASK_FALLBACK) - 1;
        }
    }
    tmp[len] = '\0';

    if (cap > 0) {
        size_t c = (len < cap - 1) ? len : cap - 1;
        memcpy(out, tmp, c);
        out[c] = '\0';
    }
    return len;
}
