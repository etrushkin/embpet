/* Shared runtime state (docs/PLAN.md 1.2): wifi, tg, pending count, booting and
 * configured flags. Mutex-protected; subscribers get change callbacks. The LED and the
 * wifi / tg / pending events are driven from here. */
#ifndef EMBPET_STATE_H
#define EMBPET_STATE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    STATE_WIFI_DOWN = 0,
    STATE_WIFI_UP,
} state_wifi_state_t;

typedef enum {
    STATE_TG_UNCONFIGURED = 0,
    STATE_TG_OFFLINE,
    STATE_TG_ONLINE,
    STATE_TG_AUTH_ERROR,
} state_tg_state_t;

#define STATE_IP_LEN     16     /* "255.255.255.255" + NUL */
#define STATE_BOT_LEN    33     /* Telegram usernames are at most 32 chars */
#define STATE_DETAIL_LEN 48     /* tg detail: short human-readable reason */

typedef struct {
    struct {
        state_wifi_state_t state;
        char ip[STATE_IP_LEN];
        int8_t rssi;
    } wifi;
    struct {
        state_tg_state_t state;
        char bot[STATE_BOT_LEN];
        char detail[STATE_DETAIL_LEN];  /* reason of the latest tg change (`tg` event) */
    } tg;
    uint32_t pending;
    bool booting;               /* true from init until the end of app_main */
    bool configured;            /* Wi-Fi credentials present */
    uint32_t gen;               /* bumped under the lock on every notified change */
} state_snapshot_t;

/* Bits of the `changed` mask passed to callbacks. */
#define STATE_CH_WIFI       (1u << 0)
#define STATE_CH_TG         (1u << 1)
#define STATE_CH_PENDING    (1u << 2)
#define STATE_CH_BOOTING    (1u << 3)
#define STATE_CH_CONFIGURED (1u << 4)

/* Called in the context of the task that made the change, after the lock is released
 * (so the callback may call proto_evt or any state_* function). `snap` is a copy taken
 * at the time of the change.
 *
 * Ordering: callbacks of concurrent changes made by different tasks can run out of order.
 * Each notified change increments snap->gen under the lock. A subscriber keeps the values
 * it last emitted plus the newest gen it has handled, and runs each callback under its
 * own mutex:
 *   1. skip the callback if snap->gen is not newer than the newest gen handled;
 *   2. otherwise compare the fields it cares about in `snap` with what it last emitted
 *      and emit when they differ, regardless of `changed` (which is only a hint);
 *   3. record snap->gen and the emitted values.
 * Race this prevents: task A sets pending 1->2 and task B sets 2->3 right after; B's
 * callback runs first and emits 3, then A's stale snapshot (gen older, count 2) arrives
 * and would emit 2, leaving the host with a wrong final value. Rule 1 drops it. Rule 2
 * also covers a callback that was skipped: a later snapshot still carries every field. */
typedef void (*state_cb_t)(const state_snapshot_t *snap, uint32_t changed, void *ctx);

#define STATE_MAX_SUBSCRIBERS 8

/* Initial state: wifi down, tg unconfigured, pending 0, booting true, configured false. */
void state_init(void);

/* Register a callback. Returns false if the subscriber table is full. Register during
 * init; there is no unsubscribe. */
bool state_subscribe(state_cb_t cb, void *ctx);

void state_get(state_snapshot_t *out);

/* Setters notify subscribers only when something observable changed. For wifi that is
 * state or ip; rssi is stored silently (it would otherwise flood). For tg it is state or
 * bot; detail is stored silently, so a changing error text cannot flood either. `ip`, `bot`
 * and `detail` may be NULL (= empty) and are truncated to fit. */
void state_set_wifi(state_wifi_state_t state, const char *ip, int8_t rssi);
void state_set_tg(state_tg_state_t state, const char *bot, const char *detail);
void state_set_pending(uint32_t count);
void state_set_booting(bool booting);
void state_set_configured(bool configured);

#endif /* EMBPET_STATE_H */
