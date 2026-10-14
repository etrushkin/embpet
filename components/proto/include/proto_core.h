#ifndef EMBPET_PROTO_CORE_H
#define EMBPET_PROTO_CORE_H
/* Pure logic (no ESP-IDF includes): "@@" frame rendering and the event replay ring
 * (README 3.3). Uses only libc and cJSON. Not thread-safe: the caller (proto.c) holds a
 * lock around every call. The escape decoder and other pure cores of this component
 * join this layer later. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

/* On target the component CMakeLists defines EMBPET_EVT_RING from CONFIG_EMBPET_EVT_RING. */
#ifndef EMBPET_EVT_RING
#define EMBPET_EVT_RING 32
#endif
_Static_assert(EMBPET_EVT_RING >= 1, "EMBPET_EVT_RING must be at least 1");

/* Fields echoed from the command into its res frame. NULL members are omitted. */
typedef struct {
    const char *cmd;
    const char *rid;
    const char *actor;
} proto_ctx_t;

/* Render a complete frame line "\n@@{json}\n" (malloc'd, NUL-terminated, *len excludes
 * the NUL). Field order: t, seq, boot, then the type-specific fields.
 * `fields` / `payload` are CONSUMED (freed) whether or not rendering succeeds; they may
 * be NULL. Keys of fields/payload that collide with an already-present key are dropped.
 * Returns NULL on allocation failure. Free the result with free(). */

/* evt: {"t":"evt","seq":..,"boot":..,"ev":ev, ...fields} */
char *proto_core_render_evt(uint32_t seq, const char *boot, const char *ev, cJSON *fields,
                            size_t *len);

/* res ok:   {"t":"res","seq","boot","cmd","rid","actor","ok":true, ...payload}
 * res error (err != NULL): {... "ok":false,"err":err,"msg":msg}; msg may be NULL and
 * payload is ignored (freed). ctx may be NULL. */
char *proto_core_render_res(uint32_t seq, const char *boot, const proto_ctx_t *ctx,
                            const char *err, const char *msg, cJSON *payload, size_t *len);

/* ---- Event ring: the last EMBPET_EVT_RING rendered evt lines, verbatim. ---- */

typedef struct {
    char *line;                 /* owned, malloc'd */
    size_t len;
    uint32_t seq;
} proto_ring_slot_t;

/* Byte cap on the stored lines (heap budget, PLAN 1.2); CMake defines it from
 * CONFIG_EMBPET_EVT_RING_BYTES on target. */
#ifndef EMBPET_EVT_RING_BYTES
#define EMBPET_EVT_RING_BYTES 16384
#endif

typedef struct {
    proto_ring_slot_t s[EMBPET_EVT_RING];
    size_t head;                /* index of the oldest entry */
    size_t count;
    size_t bytes;               /* sum of stored line lengths, always <= EMBPET_EVT_RING_BYTES */
    uint32_t evicted_seq;       /* highest seq pushed out of the ring so far (0 = none) */
} proto_ring_t;

void proto_ring_init(proto_ring_t *r);

/* Free every stored line and reset to empty. */
void proto_ring_clear(proto_ring_t *r);

/* Append a line (takes ownership of `line`, which must be malloc'd). Seqs must be
 * strictly increasing. The oldest entries are evicted while the ring has
 * EMBPET_EVT_RING entries or the byte cap would be exceeded. A line longer than the cap
 * is not stored (freed); it counts as evicted, so `since` reports truncated. */
void proto_ring_push(proto_ring_t *r, uint32_t seq, char *line, size_t len);

typedef void (*proto_ring_visit_fn)(uint32_t seq, const char *line, size_t len, void *ctx);

/* Visit, oldest first, every stored event with seq > since (since 0 = all). Returns how
 * many were visited. *truncated (may be NULL) is set when events newer than `since` were
 * already evicted, i.e. the host missed some that can no longer be replayed. The
 * callback must not modify the ring. */
size_t proto_ring_since(const proto_ring_t *r, uint32_t since, proto_ring_visit_fn fn,
                        void *ctx, bool *truncated);

#endif /* EMBPET_PROTO_CORE_H */
