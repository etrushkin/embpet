#ifndef EMBPET_WIFI_H
#define EMBPET_WIFI_H
/* Wi-Fi station (README 2.5, 3.4): credentials and TX power come from cfg, the connection
 * state goes to the shared state module (state_set_wifi), and the `wifi` event is emitted
 * from there on up/down/ip transitions only. Reconnects use an exponential backoff
 * (wifi_core.h). Secrets are never logged. */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Bring up the netif, the Wi-Fi driver (credentials kept in RAM only: cfg is the single
 * store), apply the stored TX power after the driver has started, and connect if an SSID
 * is stored. With no SSID nothing is attempted. Later cfg changes (credentials, TX power)
 * are picked up through cfg_subscribe. Call once from app_main after cfg_init() and
 * state_init(). */
esp_err_t wifi_start(void);

/* RSSI of the current access point in dBm; false if not associated. */
bool wifi_get_rssi(int8_t *rssi);

#endif /* EMBPET_WIFI_H */
