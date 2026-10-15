#include "led_core.h"

bool led_core_level(const led_inputs_t *in, uint64_t now_ms)
{
    if (in->booting) {
        return true;
    }
    if (!in->configured || !in->wifi_up || !in->tg_online) {
        return now_ms % LED_BLINK_PERIOD_MS < LED_BLINK_PERIOD_MS / 2;
    }
    if (in->pending > 0) {
        uint32_t t = (uint32_t)(now_ms % LED_PENDING_PERIOD_MS);
        return t < LED_PENDING_BURST_MS && t % 200 < 100;
    }
    return false;
}
