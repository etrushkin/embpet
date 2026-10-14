#ifndef EMBPET_PREFIX_CORE_H
#define EMBPET_PREFIX_CORE_H
/* Pure logic (no ESP-IDF includes): actor id validation and "[A] text" prefix parsing. */

#include <stdbool.h>
#include <stddef.h>

#define ACTOR_ID_MAX 16                  /* chars, excluding NUL */
#define ACTOR_BUF_SIZE (ACTOR_ID_MAX + 1)

/* True if s[0..len) is 1..ACTOR_ID_MAX chars of [A-Za-z0-9_-]. */
bool actor_id_valid(const char *s, size_t len);

/* If text[0..len) starts with "[<actor>]", copy the actor into out (NUL-terminated,
 * ACTOR_BUF_SIZE bytes) and return true. Whatever follows "]" is ignored (it may be
 * UTF-8 or empty). Returns false (out untouched) if there is no valid prefix. */
bool prefix_parse_actor(const char *text, size_t len, char out[ACTOR_BUF_SIZE]);

#endif /* EMBPET_PREFIX_CORE_H */
