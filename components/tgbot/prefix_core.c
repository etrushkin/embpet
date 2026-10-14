#include "prefix_core.h"

#include <string.h>

bool actor_id_valid(const char *s, size_t len)
{
    if (s == NULL || len < 1 || len > ACTOR_ID_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool prefix_parse_actor(const char *text, size_t len, char out[ACTOR_BUF_SIZE])
{
    if (text == NULL || out == NULL || len < 3 || text[0] != '[') {
        return false;
    }
    size_t end = 1;
    while (end < len && end <= ACTOR_ID_MAX + 1 && text[end] != ']') {
        end++;
    }
    if (end >= len || text[end] != ']') {
        return false;
    }
    size_t n = end - 1;
    if (!actor_id_valid(text + 1, n)) {
        return false;
    }
    memcpy(out, text + 1, n);
    out[n] = '\0';
    return true;
}
