#ifndef EMBPET_CFG_CORE_H
#define EMBPET_CFG_CORE_H
/* Pure logic (no ESP-IDF includes): config validation, clamping, token masking and the
 * schema-version decision. Used by the NVS-backed cfg.c. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Schema version stored under NVS key "ver" in namespace "embpet". */
#define CFG_SCHEMA_VERSION 1

/* Configuration keys; also the identity passed to change callbacks. */
typedef enum {
    CFG_KEY_WIFI_SSID,
    CFG_KEY_WIFI_PASS,
    CFG_KEY_WIFI_TXQ,
    CFG_KEY_TG_TOKEN,
    CFG_KEY_TG_OWNER,
    CFG_KEY_OUT_BOOT,
    CFG_KEY_IN_DEB_10,
    CFG_KEY_IN_DEB_20,
    CFG_KEY_IN_DEB_21,
    CFG_KEY_COUNT
} cfg_key_t;

/* NVS key name of k ("wifi_ssid", ...), or "?" for an invalid key. Safe to log. */
const char *cfg_key_name(cfg_key_t k);

/* What to do with the namespace after reading the stored schema version. */
typedef enum {
    CFG_VER_KEEP,   /* stored == current: use as is */
    CFG_VER_STAMP,  /* no version stored (fresh or pre-versioned): keep data, write version */
    CFG_VER_ERASE   /* different version (older or newer): erase the namespace, write version */
} cfg_ver_action_t;

/* have_ver: whether "ver" exists in NVS; stored: its value. There are no migrations at
 * schema 1, so any mismatch erases the namespace (settings revert to defaults). */
cfg_ver_action_t cfg_schema_decide(bool have_ver, uint32_t stored);

/* Wi-Fi TX power in quarter-dBm: 8..80 (2..20 dBm), README 3.4. */
#define CFG_TXQ_MIN 8
#define CFG_TXQ_MAX 80
bool cfg_txq_valid(int v);
/* Clamp into [CFG_TXQ_MIN, CFG_TXQ_MAX] (used for a corrupt stored value). */
int8_t cfg_txq_clamp(int v);

/* Debounce 0..1000 ms, README 2.3. Pins with a stored debounce: GPIO10, 20, 21. */
#define CFG_DEBOUNCE_MAX_MS 1000
bool cfg_debounce_valid(int ms);
uint16_t cfg_debounce_clamp(int ms);
/* Map a GPIO number to its cfg key; false if the pin is not a configurable input. */
bool cfg_debounce_key(int pin, cfg_key_t *key);

/* Output boot bitmask: bit n = OUT n boots on (n in 0..7). */
bool cfg_out_index_valid(int idx);

/* Owner chat_id: Telegram uses non-zero ids (negative for groups); 0 means unset. */
bool cfg_owner_valid(int64_t id);

/* Wi-Fi credentials: SSID 1..32 bytes, passphrase empty (open), 8..63 bytes, or exactly
 * 64 hex digits (raw PSK). Bot token: "<1-12 digits>:<16 or more of [A-Za-z0-9_-]>",
 * at most CFG_TOKEN_MAX chars. */
#define CFG_SSID_MAX 32
#define CFG_PASS_MAX 64
#define CFG_TOKEN_MAX 127
bool cfg_ssid_valid(const char *s);
bool cfg_pass_valid(const char *s);
bool cfg_token_valid(const char *s);

/* Mask a bot token for display. A token "<digits>:<secret>" with a secret of at least
 * CFG_MASK_MIN_SECRET chars becomes "<digits>:<first 2 secret chars>…<last 3 chars>"
 * ("…" is UTF-8 U+2026), e.g. 123456:AA…xyz. Anything else (empty -> "", no colon,
 * non-digit id, short secret) becomes "***", so at most 5 secret chars are ever shown.
 * Writes a NUL-terminated string into out (cap bytes, truncated if too small; cap 0 is
 * a no-op) and returns the untruncated length. CFG_MASK_BUF_SIZE always suffices. */
#define CFG_MASK_MIN_SECRET 16
#define CFG_MASK_BUF_SIZE (CFG_TOKEN_MAX + 8)
size_t cfg_mask_token(const char *token, char *out, size_t cap);

#endif /* EMBPET_CFG_CORE_H */
