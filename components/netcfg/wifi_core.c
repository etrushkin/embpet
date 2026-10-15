#include "wifi_core.h"

#include <stddef.h>

#include "cfg_core.h"

uint32_t wifi_backoff_ms(unsigned attempt)
{
    uint32_t ms = WIFI_BACKOFF_MIN_MS;
    while (attempt-- > 0 && ms < WIFI_BACKOFF_MAX_MS) {
        ms *= 2;
    }
    return ms < WIFI_BACKOFF_MAX_MS ? ms : WIFI_BACKOFF_MAX_MS;
}

bool wifi_parse_txpower(const char *s, int *qdbm)
{
    if (s == NULL || qdbm == NULL) {
        return false;
    }
    int whole = 0;
    size_t nd = 0;
    for (; s[nd] >= '0' && s[nd] <= '9'; nd++) {
        if (nd >= 3) {
            return false;
        }
        whole = whole * 10 + (s[nd] - '0');
    }
    if (nd == 0) {
        return false;
    }
    int frac = 0;                       /* hundredths of a dBm */
    if (s[nd] == '.') {
        const char *f = s + nd + 1;
        if (f[0] < '0' || f[0] > '9') {
            return false;
        }
        frac = (f[0] - '0') * 10;
        if (f[1] >= '0' && f[1] <= '9') {
            frac += f[1] - '0';
            f += 2;
        } else {
            f += 1;
        }
        if (*f != '\0') {
            return false;
        }
    } else if (s[nd] != '\0') {
        return false;
    }
    if (frac % 25 != 0) {
        return false;
    }
    int q = whole * 4 + frac / 25;
    if (!cfg_txq_valid(q)) {
        return false;
    }
    *qdbm = q;
    return true;
}
