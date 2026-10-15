#include "pulse_core.h"

void pulse_init(pulse_t *p, bool on)
{
    p->on = on;
    p->pulsing = false;
    p->restore_on = on;
    p->deadline_ms = 0;
}

void pulse_set(pulse_t *p, bool on)
{
    p->on = on;
    p->pulsing = false;
}

bool pulse_toggle(pulse_t *p)
{
    p->on = !p->on;
    p->pulsing = false;
    return p->on;
}

void pulse_start(pulse_t *p, uint32_t ms, uint64_t now_ms)
{
    if (!p->pulsing) {
        p->restore_on = p->on;
        p->on = !p->on;
        p->pulsing = true;
    }
    p->deadline_ms = now_ms + ms;
}

bool pulse_expire(pulse_t *p, uint64_t now_ms)
{
    if (!p->pulsing || now_ms < p->deadline_ms) {
        return false;
    }
    p->pulsing = false;
    p->on = p->restore_on;
    return true;
}

uint32_t pulse_remaining_ms(const pulse_t *p, uint64_t now_ms)
{
    if (!p->pulsing || now_ms >= p->deadline_ms) {
        return 0;
    }
    return (uint32_t)(p->deadline_ms - now_ms);
}
