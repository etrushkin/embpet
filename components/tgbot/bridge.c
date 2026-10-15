/* Telegram bridge (README 2.1, docs/PLAN.md 1.2-1.3).
 *
 * Tasks: tg_poll owns the poll connection and the getUpdates offset, routes replies and
 * emits `reply` events. tg_send owns the send connection and the request queue (`tg send`
 * from the console, hint replies), and once per bot token runs getMe and deleteWebhook; the
 * poller starts only after that succeeded. Each task closes its connection while Wi-Fi is
 * down. The console never does TLS: bridge_send() posts a request and waits on the
 * request's own semaphore with its own deadline, since one Bot API call can take longer
 * than the HTTP timeout (tgapi.h).
 *
 * Getting back online, without flapping: after a failed getUpdates or a Wi-Fi drop the
 * poller's next getUpdates uses timeout 0, so a success turns tg online at once rather
 * than after a whole long poll; failures back off (1, 2, 4 .. 30 s) and only Wi-Fi changes
 * and token readiness wake the poller early, never tg transitions. After a Wi-Fi reconnect
 * tg_send also probes once with getMe (backing off until it succeeds), because the poller
 * may still be blocked in a long poll from before the drop. Nothing else re-arms that
 * probe, so a getUpdates that keeps failing while getMe works leaves tg offline instead of
 * flapping.
 *
 * Pending table: guarded by s_lock. Every change publishes the count to the shared state
 * while still holding s_lock, so two changes cannot publish their counts out of order; the
 * `pending` event is emitted from the state callback (state.h gen rule).
 *
 * Delivery is at-least-once: an update's offset advances only after its `reply` event was
 * recorded (proto_evt != 0: written to the host if connected, and in the replay ring), and
 * Telegram drops it only when the next getUpdates carries that offset. A reboot or crash
 * in between delivers the update again after boot: hosts dedupe on the reply's msg.
 *
 * Owner binding: `tg pair` arms s_pair (code, 5 min); handle_update asks pair_check about
 * every update BEFORE routing, because with no owner every update would be ignored. The
 * right `/start <code>` from any chat stores that chat as owner, emits `paired`, and queues a
 * plain confirmation. `tg token` validates the candidate with getMe on tg_send (the console
 * never does TLS) and stores it only if Telegram accepts it. The online notice is sent by
 * tg_send itself, once per boot: decided when Telegram is first online (an owner set then
 * gets it, retried a few times; no owner then means none this boot).
 *
 * Message texts, chat ids, pairing codes and tokens are never logged. */
#include "bridge_priv.h"
#include "tgbot.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bridge_core.h"
#include "cfg.h"
#include "console.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"
#include "owner_core.h"
#include "proto.h"
#include "state.h"
#include "tgapi.h"

static const char *TAG = "tgbot";

#define TASK_STACK   8192
#define TASK_PRIO    2          /* below the console (3): TLS work never delays a command */
#define QUEUE_LEN    4
#define IDLE_TICK_MS 1000       /* the tasks re-check Wi-Fi and setup at least this often */
#define OOM_RETRY_MS 1000
#define HINT_MIN_INTERVAL_US (10 * 1000000LL)
#define NOTICE_MAX_TRIES     3
#define NOTICE_RETRY_MIN_MS  2000

/* REQ_SEND and REQ_TOKEN are waited for by the console (done != NULL); REQ_HINT and
 * REQ_CONFIRM are plain texts to a chat, best effort. */
typedef enum { REQ_SEND, REQ_HINT, REQ_CONFIRM, REQ_TOKEN } req_kind_t;
typedef enum { PLAIN_HINT, PLAIN_CONFIRM, PLAIN_NOTICE } plain_kind_t;

typedef struct {
    req_kind_t kind;
    int64_t chat;
    int64_t reply_to;
    char actor[ACTOR_BUF_SIZE];         /* REQ_SEND */
    char bot[STATE_BOT_LEN];            /* REQ_TOKEN result: the bot's username */
    bool stored;                        /* REQ_TOKEN result: the token was saved */
    SemaphoreHandle_t done;             /* waited kinds: given by tg_send when finished */
    bool finished;                      /* under s_req_mux */
    bool abandoned;                     /* under s_req_mux: the console stopped waiting, so
                                           tg_send frees the request */
    tg_status_t st;
    tg_resp_t err;
    int64_t msg_id;
    char text[];                        /* REQ_TOKEN: the candidate token (wiped on free) */
} send_req_t;

static SemaphoreHandle_t s_bind;        /* owner / pairing / token writes (try_pair); taken
                                           before s_lock, never inside it */
static SemaphoreHandle_t s_lock;        /* s_table, s_bot_id, s_pair */
static pending_table_t s_table;
static pair_t s_pair;
static int64_t s_bot_id;                /* user id of the latest valid token's bot; kept
                                           when the token is cleared (bridge_route) */
static QueueHandle_t s_queue;           /* send_req_t *, consumed by tg_send */
static TaskHandle_t s_poll_task;
static portMUX_TYPE s_req_mux = portMUX_INITIALIZER_UNLOCKED;
static atomic_uint s_tok_gen = 1;       /* bumped on every token change */
static atomic_uint s_ready_gen;         /* token generation whose setup succeeded; 0 = none */
static atomic_bool s_probe_armed;       /* Wi-Fi went down: one getMe probe after it is back */
static int64_t s_last_hint_us;          /* under s_req_mux */
static atomic_uint s_hints_sent;
static atomic_uint s_hints_dropped;
static atomic_uint s_polls;             /* getUpdates attempts */
static atomic_uint s_probes;            /* getMe probes (not the per-token setup) */
#ifdef CONFIG_EMBPET_TG_TEST
static atomic_bool s_fault_poll;        /* every getUpdates fails as a network error */
/* Online notice and pairing confirmation are recorded instead of sent, until `tg t_live 1`:
 * a test boot must not message a chat. */
static atomic_bool s_capture = true;
static atomic_uint s_pair_ttl_s = PAIR_TTL_S;
static atomic_uint s_notice_n;          /* delivered (or captured) online notices */
static atomic_uint s_confirm_n;         /* delivered (or captured) pairing confirmations */
static char s_notice_text[128];         /* under s_req_mux */
static char s_confirm_text[128];        /* under s_req_mux */
#endif

/* `pending` event, following the state.h subscriber rule. */
static SemaphoreHandle_t s_emit_lock;
static uint32_t s_emit_gen;
static uint32_t s_emit_pending;

static uint64_t uptime_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static bool wifi_up(void)
{
    state_snapshot_t s;
    state_get(&s);
    return s.wifi.state == STATE_WIFI_UP;
}

/* ------------------------------------------------------------ pending table */

/* Caller holds s_lock. */
static void publish_locked(void)
{
    state_set_pending((uint32_t)pending_count(&s_table));
}

static void add_pending(int64_t msg_id, const char *actor)
{
    pending_drop_t drop;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (pending_add(&s_table, msg_id, actor, uptime_ms(), &drop) && drop.dropped) {
        cJSON *f = cJSON_CreateObject();
        if (f == NULL || cJSON_AddStringToObject(f, "actor", drop.actor) == NULL ||
            cJSON_AddNumberToObject(f, "msg", (double)drop.msg_id) == NULL) {
            cJSON_Delete(f);
            ESP_LOGW(TAG, "pending_dropped event lost (out of memory), actor %s", drop.actor);
        } else {
            proto_evt("pending_dropped", f);
        }
    }
    publish_locked();
    xSemaphoreGive(s_lock);
}

size_t bridge_pending_list(const char *actor, pending_item_t *out, size_t max)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = pending_list(&s_table, actor, uptime_ms(), out, max);
    xSemaphoreGive(s_lock);
    return n;
}

size_t bridge_ack_older(uint32_t older_s)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = pending_ack_older(&s_table, uptime_ms(), older_s);
    publish_locked();
    xSemaphoreGive(s_lock);
    return n;
}

size_t bridge_ack_msg(int64_t msg_id)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = pending_ack_msg(&s_table, msg_id);
    publish_locked();
    xSemaphoreGive(s_lock);
    return n;
}

/* ------------------------------------------------------------ subscriptions */

/* May run with s_lock held (publish_locked), so it never takes s_lock. */
static void on_state(const state_snapshot_t *snap, uint32_t changed, void *ctx)
{
    (void)ctx;
    xSemaphoreTake(s_emit_lock, portMAX_DELAY);
    if (snap->gen > s_emit_gen) {
        s_emit_gen = snap->gen;
        if (snap->pending != s_emit_pending) {
            cJSON *f = cJSON_CreateObject();
            if (f != NULL) {
                /* Remember it only if the event could be built (see tgapi.c on_state). */
                s_emit_pending = snap->pending;
                cJSON_AddNumberToObject(f, "count", snap->pending);
                proto_evt("pending", f);
            }
        }
    }
    xSemaphoreGive(s_emit_lock);
    /* Only Wi-Fi changes wake tg_poll (token readiness and token changes notify it
     * directly). A tg transition must not: tg_poll's own failure turns tg offline, and a
     * wake would cut its backoff short. */
    if ((changed & STATE_CH_WIFI) != 0) {
        if (snap->wifi.state == STATE_WIFI_DOWN) {
            atomic_store(&s_probe_armed, true);
        }
        if (s_poll_task != NULL) {
            xTaskNotifyGive(s_poll_task);
        }
    }
}

/* The bot's user id from the stored token, for the prefix fallback of bridge_route (only
 * the bot's own "[A]" messages route). A cleared or invalid token keeps the previous id. */
static void load_bot_id(void)
{
    char tok[CFG_TOKEN_MAX + 1];
    int64_t id = cfg_get_tg_token(tok, sizeof(tok)) == ESP_OK ? bridge_bot_id(tok) : 0;
    mbedtls_platform_zeroize(tok, sizeof(tok));
    if (id != 0) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_bot_id = id;
        xSemaphoreGive(s_lock);
    }
}

static void on_cfg(cfg_key_t key, void *ctx)
{
    (void)ctx;
    if (key == CFG_KEY_TG_TOKEN || key == CFG_KEY_TG_OWNER) {
        /* Another bot, or an owner now bound (also by this very pairing): a code shown
         * earlier is void. */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        pair_cancel(&s_pair);
        xSemaphoreGive(s_lock);
    }
    if (key == CFG_KEY_TG_TOKEN) {
        load_bot_id();
        atomic_fetch_add(&s_tok_gen, 1);
        if (s_poll_task != NULL) {
            xTaskNotifyGive(s_poll_task);
        }
    }
}

/* ------------------------------------------------------------ requests */

static void free_req(send_req_t *r)
{
    if (r->done != NULL) {
        vSemaphoreDelete(r->done);
    }
    if (r->kind == REQ_TOKEN) {
        mbedtls_platform_zeroize(r->text, strlen(r->text));
    }
    free(r);
}

/* Queue a plain text for chat (kind REQ_HINT or REQ_CONFIRM). False if it was not queued
 * (out of memory or queue full). */
static bool queue_plain(req_kind_t kind, int64_t chat, int64_t reply_to, const char *text,
                        bool front)
{
    size_t n = strlen(text);
    send_req_t *r = calloc(1, sizeof(*r) + n + 1);
    if (r == NULL) {
        return false;
    }
    r->kind = kind;
    r->chat = chat;
    r->reply_to = reply_to;
    memcpy(r->text, text, n + 1);
    if ((front ? xQueueSendToFront(s_queue, &r, 0) : xQueueSend(s_queue, &r, 0)) != pdTRUE) {
        free_req(r);
        return false;
    }
    return true;
}

/* Best effort and at most one per HINT_MIN_INTERVAL_US, so a burst of owner messages
 * cannot fill the queue ahead of `tg send` or hit Telegram's rate limit. */
static void post_hint(int64_t chat, int64_t reply_to)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_req_mux);
    int64_t last = s_last_hint_us;
    bool too_soon = last != 0 && now - last < HINT_MIN_INTERVAL_US;
    if (!too_soon) {
        s_last_hint_us = now;
    }
    portEXIT_CRITICAL(&s_req_mux);
    if (too_soon) {
        atomic_fetch_add(&s_hints_dropped, 1);
        ESP_LOGW(TAG, "hint dropped: one already sent in the last %d s",
                 (int)(HINT_MIN_INTERVAL_US / 1000000));
        return;
    }
    if (queue_plain(REQ_HINT, chat, reply_to, BRIDGE_HINT_TEXT, false)) {
        return;
    }
    /* Not queued: give the slot back, unless another hint took it meanwhile. */
    portENTER_CRITICAL(&s_req_mux);
    if (s_last_hint_us == now) {
        s_last_hint_us = last;
    }
    portEXIT_CRITICAL(&s_req_mux);
    atomic_fetch_add(&s_hints_dropped, 1);
    ESP_LOGW(TAG, "hint dropped: send queue full or out of memory");
}

/* REQ_SEND result: hand it to the waiting console, or free it if the console gave up. */
static void finish_send(send_req_t *r)
{
    portENTER_CRITICAL(&s_req_mux);
    bool gone = r->abandoned;
    r->finished = true;
    portEXIT_CRITICAL(&s_req_mux);
    if (gone) {
        free_req(r);
    } else {
        xSemaphoreGive(r->done);
    }
}

/* tg_send only: no sendMessage before this time after a 429 (retry_after). */
static int64_t s_blocked_until_us;

#ifdef CONFIG_EMBPET_TG_TEST
static void record_plain(plain_kind_t kind, const char *text)
{
    char *dst = kind == PLAIN_NOTICE ? s_notice_text : s_confirm_text;
    size_t n = strlen(text);
    if (n >= sizeof(s_notice_text)) {
        n = sizeof(s_notice_text) - 1;
    }
    portENTER_CRITICAL(&s_req_mux);
    memcpy(dst, text, n);
    dst[n] = '\0';
    portEXIT_CRITICAL(&s_req_mux);
    atomic_fetch_add(kind == PLAIN_NOTICE ? &s_notice_n : &s_confirm_n, 1);
}
#endif

/* A plain text (no actor tag, no pending entry) to chat: hint, pairing confirmation or
 * online notice. Honours a 429 block; nothing is sent while Wi-Fi is down. tg_send only. */
static tg_status_t deliver_plain(tgapi_conn_t *conn, plain_kind_t kind, int64_t chat,
                                 const char *text, int64_t reply_to, tg_resp_t *err)
{
    memset(err, 0, sizeof(*err));
    int64_t now = esp_timer_get_time();
    if (now < s_blocked_until_us) {
        err->retry_after_s = (uint32_t)((s_blocked_until_us - now + 999999) / 1000000);
        return TG_ERR_RETRY;
    }
    if (!wifi_up()) {
        return TG_ERR_NET;
    }
#ifdef CONFIG_EMBPET_TG_TEST
    if (kind != PLAIN_HINT && atomic_load(&s_capture)) {
        record_plain(kind, text);
        return TG_OK;
    }
#endif
    int64_t msg_id = 0;
    tg_status_t st = tgapi_send_message(conn, chat, NULL, text, reply_to, &msg_id, err);
    if (st == TG_ERR_RETRY) {
        s_blocked_until_us = esp_timer_get_time() + (int64_t)err->retry_after_s * 1000000;
    }
#ifdef CONFIG_EMBPET_TG_TEST
    if (st == TG_OK && kind != PLAIN_HINT) {
        record_plain(kind, text);
    }
#endif
    return st;
}

/* REQ_TOKEN: Telegram must accept the candidate before it is stored. Storing runs the cfg
 * callbacks here (tgapi: tg offline "token changed"; bridge: new token generation), and the
 * loop then sets the new bot up (getMe, deleteWebhook) and restarts the poller. */
static void run_token(tgapi_conn_t *conn, send_req_t *r)
{
    r->st = tgapi_check_token(conn, r->text, r->bot, sizeof(r->bot), &r->err);
    if (r->st == TG_OK) {
        portENTER_CRITICAL(&s_req_mux);
        bool gone = r->abandoned;       /* the console already answered timeout */
        portEXIT_CRITICAL(&s_req_mux);
        if (!gone) {
            xSemaphoreTake(s_bind, portMAX_DELAY);
            r->stored = cfg_set_tg_token(r->text) == ESP_OK;
            xSemaphoreGive(s_bind);
        }
    } else {
        ESP_LOGW(TAG, "token not stored: %s (code %d)", tg_status_name(r->st), r->err.error_code);
    }
    finish_send(r);
}

static void run_req(tgapi_conn_t *conn, send_req_t *r)
{
    if (r->done != NULL) {
        portENTER_CRITICAL(&s_req_mux);
        bool gone = r->abandoned;
        portEXIT_CRITICAL(&s_req_mux);
        if (gone) {
            free_req(r);                /* timed out while queued: never sent */
            return;
        }
    }
    if (r->kind == REQ_TOKEN) {
        run_token(conn, r);
        return;
    }
    if (r->kind != REQ_SEND) {
        tg_status_t st = deliver_plain(conn, r->kind == REQ_HINT ? PLAIN_HINT : PLAIN_CONFIRM,
                                       r->chat, r->text, r->reply_to, &r->err);
        if (r->kind == REQ_HINT) {
            atomic_fetch_add(st == TG_OK ? &s_hints_sent : &s_hints_dropped, 1);
        }
        if (st != TG_OK) {
            ESP_LOGW(TAG, "%s not sent: %s", r->kind == REQ_HINT ? "hint" : "pairing confirmation",
                     tg_status_name(st));
        }
        free_req(r);
        return;
    }
    int64_t now = esp_timer_get_time();
    if (now < s_blocked_until_us) {
        r->st = TG_ERR_RETRY;
        r->err.retry_after_s = (uint32_t)((s_blocked_until_us - now + 999999) / 1000000);
    } else {
        r->st = tgapi_send_message(conn, r->chat, r->actor, r->text, r->reply_to, &r->msg_id,
                                   &r->err);
        if (r->st == TG_ERR_RETRY) {
            s_blocked_until_us = esp_timer_get_time() + (int64_t)r->err.retry_after_s * 1000000;
        }
    }
    if (r->st == TG_OK) {
        /* Also when the console has already answered timeout: the message exists, so a
         * reply to it still routes from the table. */
        add_pending(r->msg_id, r->actor);
    }
    finish_send(r);
}

/* Wait for tg_send to finish a waited request, at most CONFIG_EMBPET_TG_SEND_TIMEOUT_MS.
 * False on timeout: the request is then abandoned and tg_send frees it, so the caller must
 * not touch it any more. */
static bool await_done(send_req_t *r)
{
    if (xSemaphoreTake(r->done, pdMS_TO_TICKS(CONFIG_EMBPET_TG_SEND_TIMEOUT_MS)) == pdTRUE) {
        return true;
    }
    portENTER_CRITICAL(&s_req_mux);
    bool finished = r->finished;
    r->abandoned = !finished;
    portEXIT_CRITICAL(&s_req_mux);
    if (!finished) {
        return false;
    }
    xSemaphoreTake(r->done, portMAX_DELAY);     /* given right after finished was set */
    return true;
}

const char *bridge_send(const char *actor, const char *text, int64_t reply_to,
                        int64_t *msg_id, char *msg, size_t cap)
{
    msg[0] = '\0';
    switch (bridge_check_text(actor, text, CONFIG_EMBPET_MAX_TEXT_BYTES)) {
    case BRIDGE_TEXT_TOO_LONG:
        snprintf(msg, cap, "text over %d bytes, or [%s] text over %d UTF-16 units",
                 CONFIG_EMBPET_MAX_TEXT_BYTES, actor, TG_TEXT_MAX_UTF16);
        return CONSOLE_ERR_TOO_LONG;
    case BRIDGE_TEXT_BAD_UTF8:
        snprintf(msg, cap, "text is not valid UTF-8");
        return CONSOLE_ERR_BAD_ARGS;
    case BRIDGE_TEXT_OK:
        break;
    }

    state_snapshot_t s;
    state_get(&s);
    int64_t owner = cfg_get_tg_owner();
    if (!s.configured || s.tg.state == STATE_TG_UNCONFIGURED || owner == 0) {
        snprintf(msg, cap, "%s", !s.configured ? "no Wi-Fi credentials"
                                 : s.tg.state == STATE_TG_UNCONFIGURED ? "no bot token"
                                                                        : "no owner chat");
        return CONSOLE_ERR_NOT_CONFIGURED;
    }
    if (s.wifi.state != STATE_WIFI_UP) {
        snprintf(msg, cap, "wifi down");
        return CONSOLE_ERR_OFFLINE;
    }
    if (s.tg.state == STATE_TG_AUTH_ERROR) {
        snprintf(msg, cap, "token rejected: %s", s.tg.detail);
        return CONSOLE_ERR_AUTH_ERROR;
    }
    if (s.tg.state != STATE_TG_ONLINE) {
        snprintf(msg, cap, "telegram offline: %s", s.tg.detail);
        return CONSOLE_ERR_OFFLINE;
    }

    size_t n = strlen(text);
    send_req_t *r = calloc(1, sizeof(*r) + n + 1);
    if (r != NULL) {
        r->done = xSemaphoreCreateBinary();
    }
    if (r == NULL || r->done == NULL) {
        if (r != NULL) {
            free_req(r);
        }
        snprintf(msg, cap, "out of memory");
        return CONSOLE_ERR_INTERNAL;
    }
    r->kind = REQ_SEND;
    r->chat = owner;
    r->reply_to = reply_to;
    strcpy(r->actor, actor);            /* validated by the console */
    memcpy(r->text, text, n + 1);
    /* Ahead of any queued hint: only one request runs before it. */
    if (xQueueSendToFront(s_queue, &r, 0) != pdTRUE) {
        free_req(r);
        snprintf(msg, cap, "send queue full");
        return CONSOLE_ERR_BUSY;
    }

    if (!await_done(r)) {
        snprintf(msg, cap, "no answer from Telegram within %d ms",
                 CONFIG_EMBPET_TG_SEND_TIMEOUT_MS);
        return CONSOLE_ERR_TIMEOUT;
    }

    const char *code = NULL;
    if (r->st == TG_OK) {
        *msg_id = r->msg_id;
    } else {
        code = bridge_send_err(r->st, r->err.error_code, r->err.description);
        if (r->st == TG_ERR_RETRY) {
            snprintf(msg, cap, "rate limited, retry after %u s", (unsigned)r->err.retry_after_s);
        } else if (r->err.error_code != 0) {
            snprintf(msg, cap, "telegram: %d %s", r->err.error_code, r->err.description);
        } else {
            snprintf(msg, cap, "telegram: %s", tg_status_name(r->st));
        }
    }
    free_req(r);
    return code;
}

const char *bridge_set_token(const char *token, char *bot, size_t bot_cap, char *msg,
                             size_t cap)
{
    msg[0] = '\0';
    if (!cfg_token_valid(token)) {
        snprintf(msg, cap, "not a bot token (<digits>:<secret>)");
        return CONSOLE_ERR_BAD_ARGS;
    }
    if (!wifi_up()) {
        snprintf(msg, cap, "wifi down: the token is checked with Telegram before it is stored");
        return CONSOLE_ERR_OFFLINE;
    }
    size_t n = strlen(token);
    send_req_t *r = calloc(1, sizeof(*r) + n + 1);
    if (r != NULL) {
        r->done = xSemaphoreCreateBinary();
    }
    if (r == NULL || r->done == NULL) {
        if (r != NULL) {
            free_req(r);
        }
        snprintf(msg, cap, "out of memory");
        return CONSOLE_ERR_INTERNAL;
    }
    r->kind = REQ_TOKEN;
    memcpy(r->text, token, n + 1);
    if (xQueueSendToFront(s_queue, &r, 0) != pdTRUE) {
        free_req(r);
        snprintf(msg, cap, "send queue full");
        return CONSOLE_ERR_BUSY;
    }
    if (!await_done(r)) {
        snprintf(msg, cap, "no answer from Telegram within %d ms (the token may still be "
                 "stored: check tg status)", CONFIG_EMBPET_TG_SEND_TIMEOUT_MS);
        return CONSOLE_ERR_TIMEOUT;
    }

    const char *code = NULL;
    if (r->st != TG_OK) {
        code = bridge_send_err(r->st, r->err.error_code, r->err.description);
        if (r->st == TG_ERR_AUTH) {
            snprintf(msg, cap, "token rejected by Telegram (%d), not stored", r->err.error_code);
        } else {
            snprintf(msg, cap, "telegram: %s, token not stored", tg_status_name(r->st));
        }
    } else if (!r->stored) {
        code = CONSOLE_ERR_INTERNAL;
        snprintf(msg, cap, "could not store the token");
    } else {
        snprintf(bot, bot_cap, "%s", r->bot);
    }
    free_req(r);
    return code;
}

const char *bridge_pair_start(char *code, uint32_t *expires_s, char *msg, size_t cap)
{
    msg[0] = '\0';
    state_snapshot_t s;
    state_get(&s);
    if (s.tg.state == STATE_TG_UNCONFIGURED) {
        snprintf(msg, cap, "no bot token");
        return CONSOLE_ERR_NOT_CONFIGURED;
    }
    uint32_t ttl = PAIR_TTL_S;
#ifdef CONFIG_EMBPET_TG_TEST
    ttl = atomic_load(&s_pair_ttl_s);
#endif
    uint32_t rnd;
    do {
        esp_fill_random(&rnd, sizeof(rnd));
    } while (!pair_code_make(rnd, code));
    xSemaphoreTake(s_bind, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pair_start(&s_pair, code, uptime_ms(), ttl);
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_bind);
    mbedtls_platform_zeroize(&rnd, sizeof(rnd));
    *expires_s = ttl;
    return NULL;
}

/* ------------------------------------------------------------ online notice */

typedef struct {
    bool done;                          /* sent, given up, or not wanted this boot */
    unsigned tries;
    int64_t next_us;
} notice_t;

/* Once per boot, from the first moment Telegram is online: with an owner set, post the
 * notice (a few attempts for transient failures); with none, there is no notice this boot.
 * Later reconnects never come back here. True if it used the connection. */
static bool notice_step(tgapi_conn_t *conn, notice_t *n, const state_snapshot_t *s)
{
    if (n->done || s->wifi.state != STATE_WIFI_UP || s->tg.state != STATE_TG_ONLINE) {
        return false;
    }
    int64_t owner = cfg_get_tg_owner();
    if (owner == 0) {
        n->done = true;
        return false;
    }
    int64_t now = esp_timer_get_time();
    if (now < n->next_us) {
        return false;
    }
    char text[96];
    notice_format(text, sizeof(text), esp_app_get_description()->version, proto_reset_reason());
    tg_resp_t err;
    tg_status_t st = deliver_plain(conn, PLAIN_NOTICE, owner, text, 0, &err);
    if (st == TG_OK) {
        n->done = true;
        return true;
    }
    bool transient = st == TG_ERR_NET || st == TG_ERR_TIMEOUT || st == TG_ERR_SERVER ||
                     st == TG_ERR_RETRY;
    if (!transient || ++n->tries >= NOTICE_MAX_TRIES) {
        ESP_LOGW(TAG, "online notice not sent: %s", tg_status_name(st));
        n->done = true;
        return true;
    }
    uint32_t delay = tg_retry_delay_ms(st, err.retry_after_s, n->tries - 1);
    n->next_us = esp_timer_get_time() +
                 (int64_t)(delay > NOTICE_RETRY_MIN_MS ? delay : NOTICE_RETRY_MIN_MS) * 1000;
    return true;
}

/* ------------------------------------------------------------ tg_send task */

/* getMe (bot name, online state) and, for a new token (full), deleteWebhook; then the
 * poller may start. Without full it is the reachability probe after a Wi-Fi reconnect:
 * tg_poll may still be blocked in a long poll it started before the drop, and tg would
 * stay offline until that returns (up to the poll timeout). The probe is disarmed by its
 * first success, so a getUpdates that keeps failing while getMe works cannot make the
 * state flap (tg_poll handles that case with its own backoff and timeout-0 retries).
 * Returns 0 on success, else the delay before the next attempt in ms. */
static uint32_t run_setup(tgapi_conn_t *conn, uint32_t gen, bool full, unsigned *streak)
{
    char bot[STATE_BOT_LEN];
    tg_resp_t err;
    if (!full) {
        atomic_fetch_add(&s_probes, 1);
    }
    tg_status_t st = tgapi_get_me(conn, bot, sizeof(bot), &err);
    if (st == TG_OK && full) {
        st = tgapi_delete_webhook(conn, &err);
    }
    if (st == TG_OK) {
        *streak = 0;
        atomic_store(&s_probe_armed, false);
        if (full) {
            atomic_store(&s_ready_gen, gen);
            xTaskNotifyGive(s_poll_task);
        }
        return 0;
    }
    (*streak)++;
    uint32_t delay = tg_retry_delay_ms(st, err.retry_after_s, *streak - 1);
    return delay > 0 ? delay : tg_retry_delay_ms(TG_ERR_SERVER, 0, *streak - 1);
}

static void send_task(void *arg)
{
    tgapi_conn_t *conn = arg;
    uint32_t seen_gen = 0;
    unsigned streak = 0;
    int64_t next_setup_us = 0;
    bool open = false;                  /* a session may be open */
    notice_t notice = {0};
    for (;;) {
        uint32_t gen = atomic_load(&s_tok_gen);
        if (gen != seen_gen) {          /* new token: set up again at once */
            seen_gen = gen;
            streak = 0;
            next_setup_us = 0;
        }
        state_snapshot_t s;
        state_get(&s);
        TickType_t wait = pdMS_TO_TICKS(IDLE_TICK_MS);
        bool full = atomic_load(&s_ready_gen) != gen;
        if (s.wifi.state != STATE_WIFI_UP) {
            if (open) {
                tgapi_conn_close(conn);
                open = false;
            }
            streak = 0;                 /* probe at once when Wi-Fi is back */
            next_setup_us = 0;
        } else if (!full && s.tg.state != STATE_TG_OFFLINE) {
            atomic_store(&s_probe_armed, false);    /* tg_poll got through by itself */
        } else if (s.tg.state != STATE_TG_UNCONFIGURED &&
                   (full || atomic_load(&s_probe_armed))) {
            int64_t now = esp_timer_get_time();
            if (now >= next_setup_us) {
                open = true;
                uint32_t delay = run_setup(conn, gen, full, &streak);
                next_setup_us = esp_timer_get_time() + (int64_t)delay * 1000;
                continue;
            }
            int64_t left_ms = (next_setup_us - now) / 1000 + 1;
            if (left_ms < IDLE_TICK_MS) {
                wait = pdMS_TO_TICKS(left_ms);
            }
        }
        if (notice_step(conn, &notice, &s)) {
            open = true;
        }
        send_req_t *r;
        if (xQueueReceive(s_queue, &r, wait) == pdTRUE) {
            open = true;
            run_req(conn, r);
        }
    }
}

/* ------------------------------------------------------------ tg_poll task */

/* Pairing comes before routing: with no owner every update would be ignored. A message of
 * any chat that is `/start <code>` while pairing is active is judged here; *consumed is set
 * unless the update is to be routed as usual. False if the owner could not be stored: the
 * caller does not advance the offset, so the update (still a valid code until it expires)
 * is fetched again. Neither the code nor the chat is logged. */
static bool try_pair_locked(const cJSON *u, const char *bot, bool *consumed)
{
    *consumed = false;
    int64_t chat;
    const char *text;
    if (!owner_update_text(u, &chat, &text)) {
        return true;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pair_result_t res = pair_check(&s_pair, text, bot, uptime_ms());
    xSemaphoreGive(s_lock);
    switch (res) {
    case PAIR_PASS:
        return true;
    case PAIR_WRONG:
        ESP_LOGW(TAG, "pairing: wrong code ignored");
        *consumed = true;
        return true;
    case PAIR_LOCKED:
        ESP_LOGW(TAG, "pairing cancelled after %d wrong codes, run tg pair again",
                 PAIR_MAX_WRONG);
        *consumed = true;
        return true;
    case PAIR_MATCH:
        break;
    }
    /* The cfg callback (on_cfg) ends the pairing. */
    esp_err_t err = cfg_set_tg_owner(chat);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pairing: storing the owner failed: %s", esp_err_to_name(err));
        return false;
    }
    *consumed = true;
    cJSON *f = cJSON_CreateObject();
    if (f == NULL || cJSON_AddNumberToObject(f, "chat_id", (double)chat) == NULL) {
        cJSON_Delete(f);
        ESP_LOGW(TAG, "paired event lost (out of memory)");
    } else if (proto_evt("paired", f) == 0) {
        ESP_LOGW(TAG, "paired event lost (out of memory)");
    }
    /* Best effort, one try: the `paired` event is what hosts rely on. */
    if (!queue_plain(REQ_CONFIRM, chat, 0, PAIR_CONFIRM_TEXT, true)) {
        ESP_LOGW(TAG, "pairing confirmation dropped: send queue full or out of memory");
    }
    return true;
}

/* s_bind makes judging the code and storing the owner one step with respect to everything
 * else that ends or replaces a pairing or writes the owner (`tg owner`, `tg pair`, the
 * token store), so a right code cannot bind an owner after the pairing was cancelled. */
static bool try_pair(const cJSON *u, bool *consumed)
{
    state_snapshot_t s;
    state_get(&s);
    xSemaphoreTake(s_bind, portMAX_DELAY);
    bool ok = try_pair_locked(u, s.tg.bot, consumed);
    xSemaphoreGive(s_bind);
    return ok;
}

esp_err_t bridge_owner_write(int64_t chat)
{
    xSemaphoreTake(s_bind, portMAX_DELAY);
    esp_err_t err = chat == 0 ? cfg_clear_tg_owner() : cfg_set_tg_owner(chat);
    xSemaphoreGive(s_bind);
    return err;
}

/* Route one update. False if its event could not be recorded: the caller does not advance
 * the offset, so the update is fetched again. */
static bool handle_update(const cJSON *u)
{
    bool consumed;
    if (!try_pair(u, &consumed)) {
        return false;
    }
    if (consumed) {
        return true;
    }
    int64_t owner = cfg_get_tg_owner();
    bridge_route_t rt;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bridge_route_kind_t kind = bridge_route(u, owner, s_bot_id, &s_table, &rt);
    xSemaphoreGive(s_lock);
    if (kind == BRIDGE_HINT) {
        post_hint(owner, rt.msg);       /* best effort, no event */
        return true;
    }
    if (kind != BRIDGE_REPLY) {
        return true;
    }
    cJSON *f = cJSON_CreateObject();
    if (f == NULL || cJSON_AddStringToObject(f, "actor", rt.actor) == NULL ||
        cJSON_AddNumberToObject(f, "to", (double)rt.to) == NULL ||
        cJSON_AddNumberToObject(f, "msg", (double)rt.msg) == NULL ||
        cJSON_AddStringToObject(f, "text", rt.text) == NULL ||
        cJSON_AddNumberToObject(f, "date", (double)rt.date) == NULL ||
        cJSON_AddStringToObject(f, "src", rt.from_table ? "table" : "prefix") == NULL) {
        cJSON_Delete(f);
        return false;
    }
    if (proto_evt("reply", f) == 0) {
        return false;
    }
    if (rt.from_table) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        pending_answer(&s_table, rt.to, NULL);
        publish_locked();
        xSemaphoreGive(s_lock);
    }
    return true;
}

/* Route the updates of one getUpdates result in order. *offset moves past each update
 * once it is handled; false if one could not be (out of memory). */
static bool process_batch(const cJSON *result, int64_t *offset)
{
    const cJSON *u;
    cJSON_ArrayForEach(u, result) {
        int64_t id;
        if (!tg_json_i64(u, "update_id", &id) || id < *offset) {
            continue;
        }
        if (!handle_update(u)) {
            return false;
        }
        *offset = id + 1;
    }
    return true;
}

/* One getUpdates: a long poll, or with quick a timeout-0 call that answers at once, so
 * after a failure or a Wi-Fi drop the tg state is online again as soon as Telegram is
 * reachable instead of after a whole long poll. */
static tg_status_t get_updates(tgapi_conn_t *conn, int64_t offset, bool quick, tg_resp_t *r)
{
    atomic_fetch_add(&s_polls, 1);
#ifdef CONFIG_EMBPET_TG_TEST
    if (atomic_load(&s_fault_poll)) {
        /* What tgapi does for a network error, without the network. */
        state_snapshot_t s;
        state_get(&s);
        state_set_tg(STATE_TG_OFFLINE, s.tg.bot, "test fault");
        memset(r, 0, sizeof(*r));
        return TG_ERR_NET;
    }
#endif
    if (!quick) {
        return tgapi_get_updates(conn, offset, r);
    }
    char *body = tg_build_get_updates(offset, TG_UPDATES_LIMIT, 0);
    if (body == NULL) {
        memset(r, 0, sizeof(*r));
        return TG_ERR_NO_MEM;
    }
    tg_status_t st = tgapi_call(conn, "getUpdates", body, r);
    cJSON_free(body);
    return st;
}

static void poll_task(void *arg)
{
    tgapi_conn_t *conn = arg;
    int64_t offset = 0;
    uint32_t gen = 0;
    unsigned streak = 0;
    bool open = false;
    bool quick = true;                  /* next getUpdates with timeout 0 */
    for (;;) {
        uint32_t ready = atomic_load(&s_ready_gen);
        if (!wifi_up() || ready == 0 || ready != atomic_load(&s_tok_gen)) {
            if (open) {
                tgapi_conn_close(conn);
                open = false;
            }
            quick = true;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(IDLE_TICK_MS));
            continue;
        }
        if (ready != gen) {             /* another token, maybe another bot: its own ids */
            gen = ready;
            offset = 0;
            streak = 0;
        }
        open = true;
        tg_resp_t r;
        tg_status_t st = get_updates(conn, offset, quick, &r);
        quick = st != TG_OK;
        if (st == TG_ERR_TOO_LARGE) {
            st = tgapi_get_updates_one(conn, offset, &r);
        }
        uint32_t delay = 0;
        bool hold = false;              /* wait the full delay (429 retry_after) */
        if (atomic_load(&s_tok_gen) != gen) {
            /* the token changed during the call: the answer belongs to the old bot */
        } else if (st == TG_OK) {
            streak = 0;
            if (!process_batch(r.result, &offset)) {
                delay = OOM_RETRY_MS;
            }
        } else if (st == TG_ERR_TOO_LARGE && r.first_update_id >= offset &&
                   r.first_update_id > 0) {
            streak = 0;
            offset = r.first_update_id + 1;     /* over the cap even alone: skipped, logged */
        } else {
            streak++;
            delay = tg_retry_delay_ms(st, r.retry_after_s, streak - 1);
            hold = st == TG_ERR_RETRY;
            if (delay == 0) {           /* e.g. 409 while a webhook exists, out of memory */
                delay = tg_retry_delay_ms(TG_ERR_SERVER, 0, streak - 1);
            }
        }
        tg_resp_free(&r);
        if (hold) {
            vTaskDelay(pdMS_TO_TICKS(delay));
        } else if (delay > 0) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay));     /* Wi-Fi or token wakes it */
        }
    }
}

/* ------------------------------------------------------------ start */

esp_err_t tgbot_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_bind = xSemaphoreCreateMutex();
    s_emit_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(QUEUE_LEN, sizeof(send_req_t *));
    tgapi_conn_t *poll = tgapi_conn_create(TGAPI_CONN_POLL);
    tgapi_conn_t *send = tgapi_conn_create(TGAPI_CONN_SEND);
    if (s_lock == NULL || s_bind == NULL || s_emit_lock == NULL || s_queue == NULL || poll == NULL ||
        send == NULL) {
        return ESP_ERR_NO_MEM;
    }
    pending_init(&s_table);
    pair_init(&s_pair);
    load_bot_id();
    if (xTaskCreate(poll_task, "tg_poll", TASK_STACK, poll, TASK_PRIO, &s_poll_task) != pdPASS ||
        xTaskCreate(send_task, "tg_send", TASK_STACK, send, TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (!state_subscribe(on_state, NULL)) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = cfg_subscribe(on_cfg, NULL);
    if (err != ESP_OK) {
        return err;
    }
    tg_cmd_register();
#ifdef CONFIG_EMBPET_TG_TEST
    bridge_test_register();
#endif
    return ESP_OK;
}

/* ------------------------------------------------------------ test build */

#ifdef CONFIG_EMBPET_TG_TEST
bool bridge_test_inject(const cJSON *update)
{
    return handle_update(update);
}

void bridge_test_add_pending(int64_t msg_id, const char *actor)
{
    add_pending(msg_id, actor);
}

void bridge_test_counters(bridge_test_counters_t *c)
{
    c->hints_sent = atomic_load(&s_hints_sent);
    c->hints_dropped = atomic_load(&s_hints_dropped);
    c->polls = atomic_load(&s_polls);
    c->probes = atomic_load(&s_probes);
}

void bridge_test_pair_ttl(uint32_t ttl_s)
{
    atomic_store(&s_pair_ttl_s, ttl_s);
}

void bridge_test_live(bool on)
{
    atomic_store(&s_capture, !on);
}

void bridge_test_notice(bridge_test_notice_t *n)
{
    n->live = !atomic_load(&s_capture);
    n->notices = atomic_load(&s_notice_n);
    n->confirms = atomic_load(&s_confirm_n);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    n->pair_left_s = pair_left_s(&s_pair, uptime_ms());
    xSemaphoreGive(s_lock);
    portENTER_CRITICAL(&s_req_mux);
    memcpy(n->notice_text, s_notice_text, sizeof(n->notice_text));
    memcpy(n->confirm_text, s_confirm_text, sizeof(n->confirm_text));
    portEXIT_CRITICAL(&s_req_mux);
}

void bridge_test_fault(bool on)
{
    atomic_store(&s_fault_poll, on);
    if (!on) {
        xTaskNotifyGive(s_poll_task);   /* end the fault backoff at once */
    }
}
#endif
