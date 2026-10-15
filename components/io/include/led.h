/* Status LED on CONFIG_EMBPET_LED_GPIO (README 2.2), driven by a 50 ms esp_timer from the
 * shared state. */
#ifndef EMBPET_LED_H
#define EMBPET_LED_H

/* After state_init(), before the console starts. Configures the pin, subscribes to the
 * state and starts the tick. While state.booting is set the LED is solid on, so call it
 * early; app_main clears booting at the end of init. Aborts on failure. */
void led_init(void);

#endif /* EMBPET_LED_H */
