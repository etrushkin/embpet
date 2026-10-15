#ifndef EMBPET_IO_PRIV_H
#define EMBPET_IO_PRIV_H
/* Test-only hooks (CONFIG_EMBPET_IO_TEST, never in production). */

/* Register the `io test ...` commands and start the edge recorder (io_test.c). */
void io_test_register(void);

#endif /* EMBPET_IO_PRIV_H */
