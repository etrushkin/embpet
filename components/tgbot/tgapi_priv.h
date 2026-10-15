#ifndef EMBPET_TGAPI_PRIV_H
#define EMBPET_TGAPI_PRIV_H
/* Test-only hooks (CONFIG_EMBPET_TG_TEST, never in production). */

/* Start the two test worker tasks and register the `tg t_*` commands (tgapi_test.c). */
void tgapi_test_register(void);

#endif /* EMBPET_TGAPI_PRIV_H */
