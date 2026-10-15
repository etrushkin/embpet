/* Private to the proto component (not in include/). */
#ifndef EMBPET_CONSOLE_PRIV_H
#define EMBPET_CONSOLE_PRIV_H

#include <stdbool.h>
#include <stddef.h>

/* Start the soak generator task (console_soak.c, CONFIG_EMBPET_CONSOLE_SOAK only). */
void console_soak_start(void);

/* Register `probe cpu` and `probe nop` (console_probe.c, CONFIG_EMBPET_CONSOLE_PROBE only). */
void console_probe_register(void);

/* Write S[0..N) to the console in whole driver writes, LF sent as CRLF, under
 * flockfile(stdout) (console.c header: when it drops output, and the one 50 ms wait per host
 * stall). Before console_start it falls back to fwrite on stdout. False if any of it was
 * dropped. Never logs. */
bool console_out(const char *s, size_t n);

#endif /* EMBPET_CONSOLE_PRIV_H */
