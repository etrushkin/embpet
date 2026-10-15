#ifndef EMBPET_DEBOUNCE_CORE_H
#define EMBPET_DEBOUNCE_CORE_H
/* Pure logic (no ESP-IDF includes): debounce bookkeeping of one input (README 2.3).
 * The ISR only counts raw edges (a free-running uint32 that may wrap) and re-arms a timer;
 * when the timer expires the caller samples the stable level and calls deb_settle().
 * `active` is the logical level: opto energised = pin low. */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool active;        /* last reported (debounced) state */
    uint32_t base;      /* raw edge count when the last event was reported */
} deb_t;

/* Start in the given state with raw_total edges counted so far; no event is implied. */
void deb_init(deb_t *d, bool active, uint32_t raw_total);

/* The level has been stable. If it differs from the last reported state, report a change:
 * returns true, sets *extra to the raw edges since the previous event minus the one edge
 * that produced this change (so a glitch that returned to the old level counts into it),
 * and makes this the new reference. If it is unchanged returns false and keeps counting,
 * so the glitch edges show up in the next event's extra. */
bool deb_settle(deb_t *d, bool active, uint32_t raw_total, uint32_t *extra);

/* Raw edges since the last event: what `io in` reports as extra. */
uint32_t deb_pending(const deb_t *d, uint32_t raw_total);

#endif /* EMBPET_DEBOUNCE_CORE_H */
