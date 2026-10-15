#include "debounce_core.h"

void deb_init(deb_t *d, bool active, uint32_t raw_total)
{
    d->active = active;
    d->base = raw_total;
}

bool deb_settle(deb_t *d, bool active, uint32_t raw_total, uint32_t *extra)
{
    if (active == d->active) {
        return false;
    }
    uint32_t n = raw_total - d->base;           /* unsigned: wrap-safe */
    *extra = n > 0 ? n - 1 : 0;                 /* n == 0: the edge was missed, nothing extra */
    d->active = active;
    d->base = raw_total;
    return true;
}

uint32_t deb_pending(const deb_t *d, uint32_t raw_total)
{
    return raw_total - d->base;
}
