#ifndef EMBPET_PENDING_CORE_H
#define EMBPET_PENDING_CORE_H
/* Pure logic (no ESP-IDF includes): RAM-only pending-message table (README 2.1).
 * No dynamic allocation; time is passed in as uint64_t now_ms (monotonic uptime).
 * Not thread-safe: the caller must hold a lock around every call. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "prefix_core.h"

/* On target the component CMakeLists defines EMBPET_PENDING_MAX from CONFIG_EMBPET_PENDING_MAX. */
#ifndef EMBPET_PENDING_MAX
#define EMBPET_PENDING_MAX 64
#endif
_Static_assert(EMBPET_PENDING_MAX >= 1, "EMBPET_PENDING_MAX must be at least 1");

typedef struct {
    bool used;
    bool answered;
    int64_t msg_id;
    char actor[ACTOR_BUF_SIZE];
    uint64_t sent_ms;
    uint64_t order;                      /* monotonically increasing insertion order */
} pending_entry_t;

typedef struct {
    pending_entry_t e[EMBPET_PENDING_MAX];
    uint64_t next_order;
} pending_table_t;

/* Filled by pending_add when a still-pending entry had to be dropped: the caller
 * emits a pending_dropped event {actor, msg}. */
typedef struct {
    bool dropped;
    int64_t msg_id;
    char actor[ACTOR_BUF_SIZE];
} pending_drop_t;

typedef struct {
    int64_t msg_id;
    char actor[ACTOR_BUF_SIZE];
    uint32_t age_s;
} pending_item_t;

void pending_init(pending_table_t *t);

/* Number of unanswered entries. The caller emits a `pending` event when this changes. */
size_t pending_count(const pending_table_t *t);

/* Record a sent message as pending. Returns false (table unchanged) if actor is invalid.
 * A msg_id already in the table is refreshed in place (pending again, new order).
 * When full, the oldest answered entry is evicted; if all are pending, the oldest
 * pending entry is dropped and reported through *drop (drop may be NULL). */
bool pending_add(pending_table_t *t, int64_t msg_id, const char *actor, uint64_t now_ms,
                 pending_drop_t *drop);

/* Look up msg_id (answered entries still route). Copies the actor if found. */
bool pending_lookup(const pending_table_t *t, int64_t msg_id, char actor_out[ACTOR_BUF_SIZE]);

/* Owner replied to msg_id: mark it answered and return its actor. Returns false if not
 * found. A repeated reply still returns true (routes again) but changes nothing. */
bool pending_answer(pending_table_t *t, int64_t msg_id, char actor_out[ACTOR_BUF_SIZE]);

/* Mark one message answered. Returns 1 if it was pending, 0 if unknown or already answered. */
size_t pending_ack_msg(pending_table_t *t, int64_t msg_id);

/* Mark pending entries with age >= older_s seconds as answered (0 = all). Returns how many
 * changed. Age is clamped to 0 if now_ms < sent_ms. */
size_t pending_ack_older(pending_table_t *t, uint64_t now_ms, uint32_t older_s);

/* List unanswered entries, oldest first, optionally only for one actor (NULL = all).
 * Writes at most max items; returns the total number matching.
 * Complexity is O(N^2) (one scan per listed item, to avoid a sort buffer); fine for
 * N <= 256. */
size_t pending_list(const pending_table_t *t, const char *actor_filter, uint64_t now_ms,
                    pending_item_t *out, size_t max);

#endif /* EMBPET_PENDING_CORE_H */
