#ifndef EMBPET_LED_CORE_H
#define EMBPET_LED_CORE_H
/* Pure logic (no ESP-IDF includes): status LED pattern engine (README 2.2). Returns the
 * logical LED level (true = lit) for the given state at now_ms (monotonic); the caller owns
 * the timer, the polarity and the GPIO. Patterns are a function of now_ms alone. */

#include <stdbool.h>
#include <stdint.h>

#define LED_BLINK_PERIOD_MS    1000     /* 500 ms on / 500 ms off */
#define LED_PENDING_PERIOD_MS  4000     /* 5 x (100 on / 100 off) burst, then off */
#define LED_PENDING_BURST_MS   1000

typedef struct {
    bool booting;
    bool configured;
    bool wifi_up;
    bool tg_online;
    uint32_t pending;
} led_inputs_t;

/* Priority, first match wins: booting = solid on; not configured, Wi-Fi down or Telegram
 * not online = 1 Hz blink; pending > 0 = burst pattern; else off. */
bool led_core_level(const led_inputs_t *in, uint64_t now_ms);

#endif /* EMBPET_LED_CORE_H */
