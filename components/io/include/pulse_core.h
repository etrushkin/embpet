#ifndef EMBPET_PULSE_CORE_H
#define EMBPET_PULSE_CORE_H
/* Pure logic (no ESP-IDF includes): logical level of one output with an optional timed
 * pulse (README 2.4). Levels are logical: true = on. Time is passed in as now_ms
 * (monotonic); the caller owns the timer and the locking. */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool on;                /* current logical level */
    bool pulsing;
    bool restore_on;        /* level to return to when the pulse ends (valid while pulsing) */
    uint64_t deadline_ms;   /* pulse end (valid while pulsing) */
} pulse_t;

void pulse_init(pulse_t *p, bool on);

/* Set a level; cancels a running pulse. */
void pulse_set(pulse_t *p, bool on);

/* Invert the current level; cancels a running pulse. Returns the new level. */
bool pulse_toggle(pulse_t *p);

/* Invert the level for ms. If already pulsing, only the deadline restarts: the pulse level
 * and the level restored at the end are unchanged. */
void pulse_start(pulse_t *p, uint32_t ms, uint64_t now_ms);

/* End the pulse if its deadline has passed. Returns true if the level changed. A stale
 * timer callback (the pulse was cancelled or restarted) does nothing. */
bool pulse_expire(pulse_t *p, uint64_t now_ms);

/* Milliseconds left in the pulse, 0 when not pulsing. */
uint32_t pulse_remaining_ms(const pulse_t *p, uint64_t now_ms);

#endif /* EMBPET_PULSE_CORE_H */
