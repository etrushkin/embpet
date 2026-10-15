#ifndef EMBPET_WIFI_CORE_H
#define EMBPET_WIFI_CORE_H
/* Pure logic (no ESP-IDF includes) for the Wi-Fi station: reconnect backoff schedule and
 * the `wifi txpower` argument parser. Used by wifi.c and cmd_wifi.c. */

#include <stdbool.h>
#include <stdint.h>

/* Reconnect backoff: attempt 0 waits 1 s, then 2, 4, 8, 16 s, capped at 30 s. */
#define WIFI_BACKOFF_MIN_MS 1000
#define WIFI_BACKOFF_MAX_MS 30000
uint32_t wifi_backoff_ms(unsigned attempt);

/* Parse a TX power in dBm ("8", "8.5", "17.25"): 2..20 dBm in 0.25 steps, plain decimal
 * only (no sign, exponent or spaces). On success stores quarter-dBm (8..80) in *qdbm. */
bool wifi_parse_txpower(const char *s, int *qdbm);

#endif /* EMBPET_WIFI_CORE_H */
