#ifndef EMBPET_LED_PRIV_H
#define EMBPET_LED_PRIV_H
/* Test-only hooks (CONFIG_EMBPET_LED_TEST, never in production). */

/* Register the `led t_*` commands and start the edge recorder (led_test.c). */
void led_test_register(void);

#endif /* EMBPET_LED_PRIV_H */
