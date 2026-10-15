#ifndef EMBPET_BRIDGE_PRIV_H
#define EMBPET_BRIDGE_PRIV_H
/* Between bridge.c (tasks, pending table) and the console commands (tg_cmd.c, and
 * bridge_test.c in the test build). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "pending_core.h"
#include "sdkconfig.h"

/* `tg send` on the console task: check the text, the settings and the Wi-Fi / Telegram
 * state (failing fast, no TLS here), post the request to tg_send and wait for it at most
 * CONFIG_EMBPET_TG_SEND_TIMEOUT_MS. Returns NULL on success (*msg_id set; the message is in
 * the pending table) or a CONSOLE_ERR_* code with a human message in msg (cap bytes).
 * actor must be a valid actor id; reply_to 0 = not a reply. */
const char *bridge_send(const char *actor, const char *text, int64_t reply_to,
                        int64_t *msg_id, char *msg, size_t cap);

/* Pending table access under the bridge lock (README 3.4); the ack functions publish the
 * new count (`pending` event). */
size_t bridge_pending_list(const char *actor, pending_item_t *out, size_t max);
size_t bridge_ack_older(uint32_t older_s);
size_t bridge_ack_msg(int64_t msg_id);

/* Register tg send, tg pending, tg ack, tg status (tg_cmd.c). */
void tg_cmd_register(void);

#ifdef CONFIG_EMBPET_TG_TEST
/* Test build only (bridge_test.c). */

/* Route one update as if tg_poll had received it; false if its event could not be
 * recorded (the poller would fetch it again). */
bool bridge_test_inject(const cJSON *update);

/* Record msg_id for actor exactly as a successful tg send does (pending_dropped event on
 * overflow, `pending` event on a count change). */
void bridge_test_add_pending(int64_t msg_id, const char *actor);

typedef struct {
    uint32_t hints_sent;
    uint32_t hints_dropped;     /* rate limit, Wi-Fi down, no token, queue full, error */
    uint32_t polls;             /* getUpdates attempts (faulted ones included) */
    uint32_t probes;            /* getMe reachability probes after a Wi-Fi reconnect */
} bridge_test_counters_t;

void bridge_test_counters(bridge_test_counters_t *c);

/* While on, every getUpdates fails as a network error would (tg state offline "test
 * fault") without touching the network; getMe and sendMessage are unaffected. */
void bridge_test_fault(bool on);

/* Register the `tg t_*` bridge test commands. */
void bridge_test_register(void);
#endif

#endif /* EMBPET_BRIDGE_PRIV_H */
