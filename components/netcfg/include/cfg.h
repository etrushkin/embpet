#ifndef EMBPET_CFG_H
#define EMBPET_CFG_H
/* Typed NVS configuration store (README 2.5). Namespace "embpet", schema key "ver".
 * Secret values (Wi-Fi password, bot token) are never logged; only key names are. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cfg_core.h"
#include "esp_err.h"

/* Init NVS flash and open the namespace. nvs_flash_init is erased and retried on
 * ESP_ERR_NVS_NO_FREE_PAGES / ESP_ERR_NVS_NEW_VERSION_FOUND (the whole NVS partition is
 * lost). A "ver" mismatch (older or newer, i.e. also a downgrade) erases only the
 * "embpet" namespace (no migrations at schema 1). Idempotent once it has succeeded.
 * Call before any other cfg_* function. If it fails, getters return defaults and setters
 * return ESP_ERR_INVALID_STATE; cfg_subscribe() still works after the call. */
esp_err_t cfg_init(void);

/* Change notification: called after a successful set or clear, from the caller's task,
 * with no cfg lock held (so the callback may call cfg_* functions, except that it must
 * not set or clear the key it is notified for). Not callable from an ISR or an
 * esp_timer callback (cfg takes a mutex and writes flash). Up to CFG_MAX_SUBSCRIBERS
 * callbacks; returns ESP_ERR_NO_MEM when full. */
#define CFG_MAX_SUBSCRIBERS 8
typedef void (*cfg_change_cb_t)(cfg_key_t key, void *ctx);
esp_err_t cfg_subscribe(cfg_change_cb_t cb, void *ctx);

/* Strings: copy into buf (cap bytes). ESP_ERR_NOT_FOUND if unset, ESP_ERR_INVALID_SIZE
 * if it does not fit, ESP_ERR_INVALID_STATE if the stored value is corrupt; buf is left
 * empty on any error. Stored as NVS blobs so DEBUG logging never shows the value. */
esp_err_t cfg_get_wifi_ssid(char *buf, size_t cap);
esp_err_t cfg_get_wifi_pass(char *buf, size_t cap);
esp_err_t cfg_get_tg_token(char *buf, size_t cap);

/* Numbers: the Kconfig default (or 0 / 0 mask / unset owner) when unset or unreadable. */
int8_t cfg_get_wifi_txq(void);              /* quarter-dBm, CONFIG_EMBPET_WIFI_TXPOWER_QDBM */
int64_t cfg_get_tg_owner(void);             /* 0 = no owner */
uint8_t cfg_get_out_boot(void);             /* bit n set = OUT n boots on; default 0 */
uint16_t cfg_get_debounce(int pin);         /* ms; CONFIG_EMBPET_DEBOUNCE_DEFAULT_MS for
                                               unset or non-configurable pins */

/* Setters validate (ESP_ERR_INVALID_ARG), persist, then notify. */
/* Writes SSID and password under one lock with one commit, then notifies once with
 * CFG_KEY_WIFI_SSID, meaning "credentials changed" (read both keys again). */
esp_err_t cfg_set_wifi(const char *ssid, const char *pass);
esp_err_t cfg_set_wifi_txq(int qdbm);
esp_err_t cfg_set_tg_token(const char *token);
esp_err_t cfg_clear_tg_token(void);
esp_err_t cfg_set_tg_owner(int64_t chat_id);
esp_err_t cfg_clear_tg_owner(void);
esp_err_t cfg_set_out_boot(uint8_t mask);
esp_err_t cfg_set_debounce(int pin, int ms);                  /* pin 10, 20 or 21; ms 0..1000 */

#endif /* EMBPET_CFG_H */
