/* Test-only build (CONFIG_EMBPET_TG_TEST, tools/tg_test.py), never in production.
 *
 * Two worker tasks mirror the planned runtime model (docs/PLAN.md 1.2): "tg_t_poll" owns
 * the poll connection, "tg_t_send" the send connection. Console handlers hand a job to a
 * worker and wait for it, so no TLS runs on the console task. Every command answers ok:true
 * with the raw call outcome (st = tg_status_name, http, code, desc) for the host to check.
 *
 *  - tg t_token -- <token>    cfg_set_tg_token; answers the masked token only
 *  - tg t_getme / tg t_hook   getMe / deleteWebhook on the send connection
 *  - tg t_poll [-o N] [-t S]  getUpdates (offset N, timeout S, limit 5); answers the count,
 *                             the next offset and how many are private messages, never
 *                             chat ids or message text. Does not confirm anything beyond
 *                             the given offset.
 *  - tg t_send <chat> [-r M] [-a A] -- <text>
 *                             sendMessage ("[A] text", or plain text without -a); answers
 *                             the message id, the bold entity and reply id as Telegram
 *                             echoed them, and the response size in bytes
 *  - tg t_soak <s> <chat> <every_s> [-g ms]
 *                             for s seconds: long polling (confirming every update) and,
 *                             on the send connection, getMe every 15 s (or -g ms) plus
 *                             sendMessage to chat every every_s seconds (chat 0: getMe
 *                             only). Chats are only ever the caller's explicit choice.
 *  - tg t_stat                soak counters, heap and worker stack headroom */
#include "tgapi_priv.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "argtable3/argtable3.h"
#include "cfg.h"
#include "console.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "prefix_core.h"
#include "sdkconfig.h"
#include "state.h"
#include "tgapi.h"

#define WORKER_STACK   8192
#define GETME_EVERY_MS 15000

typedef enum { JOB_GETME, JOB_HOOK, JOB_POLL, JOB_SEND } job_kind_t;

typedef struct {
    job_kind_t kind;
    int64_t chat;
    int64_t reply_to;
    int64_t offset;
    unsigned timeout_s;
    char actor[ACTOR_BUF_SIZE];
    char *text;                 /* JOB_SEND, malloc'd copy; freed by the worker */
    /* results */
    tg_status_t st;
    tg_resp_t err;              /* details of a failed call (root always NULL) */
    char bot[STATE_BOT_LEN];
    int64_t msg_id;
    int64_t ent_off;
    int64_t ent_len;
    int64_t reply_echo;
    int count;
    int64_t next_offset;
    int private_msgs;
    uint32_t ms;
} job_t;

typedef struct {
    const char *name;
    tgapi_conn_kind_t kind;
    tgapi_conn_t *conn;
    TaskHandle_t task;
    SemaphoreHandle_t go;
    SemaphoreHandle_t done;
    volatile bool busy;         /* a job is posted or running (cleared by the worker) */
    job_t job;
    /* soak counters, written by the worker only */
    uint32_t ok;
    uint32_t fail;
    uint32_t streak;            /* consecutive failures, indexes the backoff */
    uint32_t max_ms;
    tg_status_t last;
    int64_t next_getme_us;
    int64_t next_send_us;
    uint32_t sent;
} worker_t;

static worker_t s_poll = {.name = "tg_t_poll", .kind = TGAPI_CONN_POLL};
static worker_t s_send = {.name = "tg_t_send", .kind = TGAPI_CONN_SEND};

static portMUX_TYPE s_soak_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_soak_until_us;         /* under s_soak_mux (64-bit, not atomic here) */
static int64_t s_soak_chat;             /* under s_soak_mux: set by both workers */
static uint32_t s_soak_every_s;
static uint32_t s_soak_getme_ms = GETME_EVERY_MS;
static int64_t s_soak_offset;           /* poll worker only */

static const char *const s_state_names[] = {
    [STATE_TG_UNCONFIGURED] = "unconfigured",
    [STATE_TG_OFFLINE] = "offline",
    [STATE_TG_ONLINE] = "online",
    [STATE_TG_AUTH_ERROR] = "auth_error",
};

static int64_t soak_until(void)
{
    portENTER_CRITICAL(&s_soak_mux);
    int64_t t = s_soak_until_us;
    portEXIT_CRITICAL(&s_soak_mux);
    return t;
}

static bool soak_active(void)
{
    return esp_timer_get_time() < soak_until();
}

static int64_t soak_chat(void)
{
    portENTER_CRITICAL(&s_soak_mux);
    int64_t c = s_soak_chat;
    portEXIT_CRITICAL(&s_soak_mux);
    return c;
}

/* Chat id of a private message in update u, or 0. */
static int64_t private_chat(const cJSON *u)
{
    const cJSON *chat = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(u, "message"), "chat");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(chat, "type");
    int64_t cid = 0;
    if (cJSON_IsString(type) && strcmp(type->valuestring, "private") == 0 &&
        tg_json_i64(chat, "id", &cid)) {
        return cid;
    }
    return 0;
}

/* ------------------------------------------------------------ jobs */

static void job_poll(worker_t *w, job_t *j)
{
    char *body = tg_build_get_updates(j->offset, TG_UPDATES_LIMIT, j->timeout_s);
    if (body == NULL) {
        j->st = TG_ERR_NO_MEM;
        return;
    }
    tg_resp_t r;
    j->st = tgapi_call(w->conn, "getUpdates", body, &r);
    cJSON_free(body);
    j->next_offset = j->offset;
    if (j->st == TG_OK) {
        const cJSON *u;
        cJSON_ArrayForEach(u, r.result) {
            int64_t id;
            if (tg_json_i64(u, "update_id", &id) && id + 1 > j->next_offset) {
                j->next_offset = id + 1;
            }
            if (private_chat(u) != 0) {
                j->private_msgs++;
            }
            j->count++;
        }
    }
    j->err = r;
    tg_resp_free(&j->err);
}

static void job_send(worker_t *w, job_t *j)
{
    char *body = NULL;
    j->st = tg_build_send_message(j->chat, j->actor[0] != '\0' ? j->actor : NULL, j->text,
                                  j->reply_to, &body);
    if (j->st != TG_OK) {
        return;
    }
    tg_resp_t r;
    j->st = tgapi_call(w->conn, "sendMessage", body, &r);
    cJSON_free(body);
    if (j->st == TG_OK) {
        tg_parse_message_id(r.result, &j->msg_id);
        const cJSON *e = cJSON_GetArrayItem(
            cJSON_GetObjectItemCaseSensitive(r.result, "entities"), 0);
        tg_json_i64(e, "offset", &j->ent_off);
        tg_json_i64(e, "length", &j->ent_len);
        tg_parse_message_id(cJSON_GetObjectItemCaseSensitive(r.result, "reply_to_message"),
                            &j->reply_echo);
    }
    j->err = r;
    tg_resp_free(&j->err);
}

static void run_job(worker_t *w)
{
    job_t *j = &w->job;
    int64_t t0 = esp_timer_get_time();
    switch (j->kind) {
    case JOB_GETME:
        j->st = tgapi_get_me(w->conn, j->bot, sizeof(j->bot), &j->err);
        break;
    case JOB_HOOK:
        j->st = tgapi_delete_webhook(w->conn, &j->err);
        break;
    case JOB_POLL:
        job_poll(w, j);
        break;
    case JOB_SEND:
        job_send(w, j);
        break;
    }
    free(j->text);
    j->text = NULL;
    j->ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
}

/* ------------------------------------------------------------ soak */

static void count(worker_t *w, tg_status_t st, int64_t t0)
{
    uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    w->last = st;
    if (st == TG_OK) {
        w->ok++;
        w->streak = 0;
    } else {
        w->fail++;
        w->streak++;
    }
    if (ms > w->max_ms) {
        w->max_ms = ms;
    }
}

static void wait_ms(worker_t *w, uint32_t ms)
{
    /* wake early for a console job */
    if (xSemaphoreTake(w->go, pdMS_TO_TICKS(ms)) == pdTRUE) {
        xSemaphoreGive(w->go);
    }
}

static void soak_poll(worker_t *w)
{
    tg_resp_t r;
    int64_t t0 = esp_timer_get_time();
    tg_status_t st = tgapi_get_updates(w->conn, s_soak_offset, &r);
    if (st == TG_OK) {
        const cJSON *u;
        cJSON_ArrayForEach(u, r.result) {
            int64_t id;
            if (tg_json_i64(u, "update_id", &id) && id + 1 > s_soak_offset) {
                s_soak_offset = id + 1;
            }
        }
    } else if (st == TG_ERR_TOO_LARGE && r.first_update_id > 0) {
        s_soak_offset = r.first_update_id + 1;
    }
    tg_resp_free(&r);
    count(w, st, t0);
    uint32_t delay = tg_retry_delay_ms(st, r.retry_after_s, w->streak > 0 ? w->streak - 1 : 0);
    if (delay > 0) {
        wait_ms(w, delay);
    }
}

static void soak_send(worker_t *w)
{
    int64_t now = esp_timer_get_time();
    int64_t t0 = now;
    tg_status_t st;
    tg_resp_t err;
    int64_t chat = soak_chat();
    if (chat != 0 && now >= w->next_send_us) {
        w->next_send_us = now + (int64_t)s_soak_every_s * 1000000;
        char text[96];
        snprintf(text, sizeof(text),
                 "soak %" PRIu32 " \xF0\x9F\x94\xA7 \xD1\x82\xD0\xB5\xD1\x81\xD1\x82 heap %" PRIu32,
                 w->sent + 1, (uint32_t)esp_get_free_heap_size());
        int64_t msg = 0;
        st = tgapi_send_message(w->conn, chat, "soak", text, 0, &msg, &err);
        if (st == TG_OK) {
            w->sent++;
        }
    } else if (now >= w->next_getme_us) {
        w->next_getme_us = now + (int64_t)s_soak_getme_ms * 1000;
        char bot[STATE_BOT_LEN];
        st = tgapi_get_me(w->conn, bot, sizeof(bot), &err);
    } else {
        wait_ms(w, s_soak_getme_ms < 500 ? s_soak_getme_ms : 500);
        return;
    }
    count(w, st, t0);
    uint32_t delay = tg_retry_delay_ms(st, err.retry_after_s, w->streak > 0 ? w->streak - 1 : 0);
    if (delay > 0) {
        wait_ms(w, delay);
    }
}

static void worker_task(void *arg)
{
    worker_t *w = arg;
    for (;;) {
        TickType_t wait = soak_active() ? 0 : pdMS_TO_TICKS(1000);
        if (xSemaphoreTake(w->go, wait) == pdTRUE) {
            if (w->busy) {
                run_job(w);
                w->busy = false;
                xSemaphoreGive(w->done);
            }
            continue;
        }
        if (soak_active()) {
            if (w->kind == TGAPI_CONN_POLL) {
                soak_poll(w);
            } else {
                soak_send(w);
            }
        }
    }
}

/* ------------------------------------------------------------ console glue */

static void oom(console_req_t *req)
{
    console_err(req, CONSOLE_ERR_INTERNAL, "out of memory");
}

/* Start filling a new job; false (answered busy) while the worker still owns the previous
 * one (after a timeout). */
static bool claim(worker_t *w, console_req_t *req, job_kind_t kind)
{
    if (w->busy) {
        console_err(req, CONSOLE_ERR_BUSY, "worker busy");
        return false;
    }
    memset(&w->job, 0, sizeof(w->job));
    w->job.kind = kind;
    return true;
}

/* Post the claimed job and wait. False (answered timeout) if it did not complete; the
 * worker still owns the job then. */
static bool run_on(worker_t *w, console_req_t *req, uint32_t timeout_ms)
{
    xSemaphoreTake(w->done, 0);         /* drop a completion left over from a timeout */
    w->busy = true;
    xSemaphoreGive(w->go);
    if (xSemaphoreTake(w->done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        console_err(req, CONSOLE_ERR_TIMEOUT, "worker did not finish");
        return false;
    }
    return true;
}

/* {st, http, code, desc, retry_after, ms} of a finished job. */
static cJSON *outcome(const job_t *j)
{
    cJSON *p = cJSON_CreateObject();
    if (p != NULL) {
        cJSON_AddStringToObject(p, "st", tg_status_name(j->st));
        cJSON_AddNumberToObject(p, "http", j->err.http_status);
        cJSON_AddNumberToObject(p, "code", j->err.error_code);
        cJSON_AddStringToObject(p, "desc", j->err.description);
        cJSON_AddNumberToObject(p, "retry_after", j->err.retry_after_s);
        cJSON_AddNumberToObject(p, "bytes", (double)j->err.body_len);
        cJSON_AddNumberToObject(p, "ms", j->ms);
    }
    return p;
}

static bool parse_i64(const char *s, int64_t *out)
{
    char *end;
    long long v = strtoll(s, &end, 10);
    if (*s == '\0' || *end != '\0') {
        return false;
    }
    *out = v;
    return true;
}

/* ---- tg t_token -- <token> ---- */

static struct arg_str *s_tok_arg;
static void *s_tok_args[2];

static void t_token(console_req_t *req)
{
    const char *tok = s_tok_arg->sval[0];
    esp_err_t err = cfg_set_tg_token(tok);
    if (err == ESP_ERR_INVALID_ARG) {
        console_err(req, CONSOLE_ERR_BAD_ARGS, "invalid token");
        return;
    }
    if (err != ESP_OK) {
        console_err(req, CONSOLE_ERR_INTERNAL, esp_err_to_name(err));
        return;
    }
    char masked[CFG_MASK_BUF_SIZE];
    cfg_mask_token(tok, masked, sizeof(masked));
    cJSON *p = cJSON_CreateObject();
    if (p == NULL) {
        oom(req);
        return;
    }
    cJSON_AddStringToObject(p, "token", masked);
    console_ok(req, p);
}

static const console_cmd_t s_t_token = {
    .name = "tg t_token",
    .help = "TEST BUILD: store a bot token (answers it masked)",
    .args = s_tok_args,
    .handler = t_token,
};

/* ---- tg t_getme / tg t_hook ---- */

static void t_getme(console_req_t *req)
{
    if (!claim(&s_send, req, JOB_GETME) ||
        !run_on(&s_send, req, 2 * CONFIG_EMBPET_TG_SEND_TIMEOUT_MS + 5000)) {
        return;
    }
    cJSON *p = outcome(&s_send.job);
    if (p == NULL) {
        oom(req);
        return;
    }
    cJSON_AddStringToObject(p, "bot", s_send.job.bot);
    console_ok(req, p);
}

static const console_cmd_t s_t_getme = {
    .name = "tg t_getme",
    .help = "TEST BUILD: getMe on the send connection",
    .handler = t_getme,
};

static void t_hook(console_req_t *req)
{
    if (!claim(&s_send, req, JOB_HOOK) ||
        !run_on(&s_send, req, 2 * CONFIG_EMBPET_TG_SEND_TIMEOUT_MS + 5000)) {
        return;
    }
    cJSON *p = outcome(&s_send.job);
    if (p == NULL) {
        oom(req);
        return;
    }
    console_ok(req, p);
}

static const console_cmd_t s_t_hook = {
    .name = "tg t_hook",
    .help = "TEST BUILD: deleteWebhook on the send connection",
    .handler = t_hook,
};

/* ---- tg t_poll [-o offset] [-t timeout_s] ---- */

static struct arg_str *s_poll_off;
static struct arg_int *s_poll_to;
static void *s_poll_args[3];

static void t_poll(console_req_t *req)
{
    if (!claim(&s_poll, req, JOB_POLL)) {
        return;
    }
    int to = s_poll_to->count > 0 ? s_poll_to->ival[0] : 0;
    if ((s_poll_off->count > 0 && !parse_i64(s_poll_off->sval[0], &s_poll.job.offset)) ||
        to < 0 || to > CONFIG_EMBPET_TG_POLL_TIMEOUT_S) {
        console_err(req, CONSOLE_ERR_BAD_ARGS, "-o <offset> -t <0..poll timeout s>");
        return;
    }
    s_poll.job.timeout_s = (unsigned)to;
    if (!run_on(&s_poll, req, (uint32_t)(2 * (CONFIG_EMBPET_TG_POLL_TIMEOUT_S + 10) + 5) * 1000)) {
        return;
    }
    cJSON *p = outcome(&s_poll.job);
    if (p == NULL) {
        oom(req);
        return;
    }
    cJSON_AddNumberToObject(p, "count", s_poll.job.count);
    cJSON_AddNumberToObject(p, "next", (double)s_poll.job.next_offset);
    cJSON_AddNumberToObject(p, "private", s_poll.job.private_msgs);
    console_ok(req, p);
}

static const console_cmd_t s_t_poll = {
    .name = "tg t_poll",
    .help = "TEST BUILD: getUpdates; answers count, next offset, private message count",
    .args = s_poll_args,
    .handler = t_poll,
};

/* ---- tg t_send <chat> [-r msg] -a A -- <text> ---- */

static struct arg_str *s_send_chat;
static struct arg_str *s_send_reply;
static struct arg_str *s_send_text;
static void *s_send_args[4];

static void t_send(console_req_t *req)
{
    const char *actor = console_ctx(req)->actor;
    if (!claim(&s_send, req, JOB_SEND)) {
        return;
    }
    job_t *j = &s_send.job;
    if (!parse_i64(s_send_chat->sval[0], &j->chat) ||
        (s_send_reply->count > 0 && !parse_i64(s_send_reply->sval[0], &j->reply_to))) {
        console_err(req, CONSOLE_ERR_BAD_ARGS, "<chat> [-r <msg>] [-a <actor>] -- <text>");
        return;
    }
    if (actor != NULL) {
        strcpy(j->actor, actor);        /* validated by the console */
    }
    j->text = strdup(s_send_text->sval[0]);
    if (j->text == NULL) {
        oom(req);
        return;
    }
    if (!run_on(&s_send, req, 2 * CONFIG_EMBPET_TG_SEND_TIMEOUT_MS + 5000)) {
        return;
    }
    cJSON *p = outcome(j);
    if (p == NULL) {
        oom(req);
        return;
    }
    cJSON_AddNumberToObject(p, "msg", (double)j->msg_id);
    cJSON_AddNumberToObject(p, "ent_off", (double)j->ent_off);
    cJSON_AddNumberToObject(p, "ent_len", (double)j->ent_len);
    cJSON_AddNumberToObject(p, "reply_echo", (double)j->reply_echo);
    console_ok(req, p);
}

static const console_cmd_t s_t_send = {
    .name = "tg t_send",
    .help = "TEST BUILD: sendMessage \"[A] text\" (plain without -a); answers msg id, entity",
    .args = s_send_args,
    .handler = t_send,
};

/* ---- tg t_soak <seconds> <chat> <every_s> ---- */

static struct arg_int *s_soak_s;
static struct arg_str *s_soak_chat_arg;
static struct arg_int *s_soak_every;
static struct arg_int *s_soak_getme;
static void *s_soak_args[5];

static void t_soak(console_req_t *req)
{
    int64_t chat = 0;
    int secs = s_soak_s->ival[0];
    int every = s_soak_every->ival[0];
    int getme = s_soak_getme->count > 0 ? s_soak_getme->ival[0] : GETME_EVERY_MS;
    if (!parse_i64(s_soak_chat_arg->sval[0], &chat) || secs < 0 || secs > 86400 ||
        every < 10 || every > 86400 || getme < 100 || getme > 600000) {
        console_err(req, CONSOLE_ERR_BAD_ARGS,
                    "<0..86400 s> <chat|0> <10..86400 s> [-g <100..600000 ms>]");
        return;
    }
    s_soak_every_s = (uint32_t)every;
    s_soak_getme_ms = (uint32_t)getme;
    s_send.next_send_us = esp_timer_get_time();
    s_send.next_getme_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_soak_mux);
    s_soak_chat = chat;
    s_soak_until_us = esp_timer_get_time() + (int64_t)secs * 1000000;
    portEXIT_CRITICAL(&s_soak_mux);
    /* wake the idle workers at once */
    xSemaphoreGive(s_poll.go);
    xSemaphoreGive(s_send.go);
    console_ok(req, NULL);
}

static const console_cmd_t s_t_soak = {
    .name = "tg t_soak",
    .help = "TEST BUILD: poll + send soak for <s> seconds (0 stops), send to <chat> every <s>",
    .args = s_soak_args,
    .handler = t_soak,
};

/* ---- tg t_stat ---- */

static cJSON *worker_stat(const worker_t *w)
{
    cJSON *o = cJSON_CreateObject();
    if (o != NULL) {
        cJSON_AddNumberToObject(o, "ok", w->ok);
        cJSON_AddNumberToObject(o, "fail", w->fail);
        cJSON_AddStringToObject(o, "last", tg_status_name(w->last));
        cJSON_AddNumberToObject(o, "max_ms", w->max_ms);
        cJSON_AddNumberToObject(o, "stack_free", (double)uxTaskGetStackHighWaterMark(w->task));
    }
    return o;
}

static void t_stat(console_req_t *req)
{
    cJSON *p = cJSON_CreateObject();
    if (p == NULL) {
        oom(req);
        return;
    }
    int64_t left = (soak_until() - esp_timer_get_time()) / 1000000;
    state_snapshot_t s;
    state_get(&s);
    cJSON_AddNumberToObject(p, "soak_left_s", left > 0 ? (double)left : 0);
    cJSON_AddItemToObject(p, "poll", worker_stat(&s_poll));
    cJSON_AddItemToObject(p, "send", worker_stat(&s_send));
    cJSON_AddNumberToObject(p, "sent", s_send.sent);
    cJSON_AddNumberToObject(p, "chat", (double)soak_chat());
    cJSON_AddNumberToObject(p, "offset", (double)s_soak_offset);
    cJSON_AddNumberToObject(p, "heap_free", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(p, "heap_min", (double)esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(p, "heap_largest",
                            (double)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    cJSON_AddStringToObject(p, "tg", s_state_names[s.tg.state]);
    cJSON_AddStringToObject(p, "bot", s.tg.bot);
    cJSON_AddStringToObject(p, "detail", s.tg.detail);
    console_ok(req, p);
}

static const console_cmd_t s_t_stat = {
    .name = "tg t_stat",
    .help = "TEST BUILD: soak counters, heap, worker stack headroom, tg state",
    .handler = t_stat,
};

static void start_worker(worker_t *w)
{
    w->conn = tgapi_conn_create(w->kind);
    w->go = xSemaphoreCreateBinary();
    w->done = xSemaphoreCreateBinary();
    if (w->conn == NULL || w->go == NULL || w->done == NULL ||
        xTaskCreate(worker_task, w->name, WORKER_STACK, w, 5, &w->task) != pdPASS) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
}

void tgapi_test_register(void)
{
    start_worker(&s_poll);
    start_worker(&s_send);

    s_tok_arg = arg_str1(NULL, NULL, "<token>", "bot token");
    s_poll_off = arg_str0("o", NULL, "<offset>", "first update id");
    s_poll_to = arg_int0("t", NULL, "<s>", "long-poll timeout");
    s_send_chat = arg_str1(NULL, NULL, "<chat>", "chat id");
    s_send_reply = arg_str0("r", NULL, "<msg>", "reply to message id");
    s_send_text = arg_str1(NULL, NULL, "<text>", "message text");
    s_soak_s = arg_int1(NULL, NULL, "<s>", "duration");
    s_soak_chat_arg = arg_str1(NULL, NULL, "<chat>", "chat id or 0");
    s_soak_every = arg_int1(NULL, NULL, "<every_s>", "send period");
    s_soak_getme = arg_int0("g", NULL, "<ms>", "getMe period (default 15000)");
    if (s_tok_arg == NULL || s_poll_off == NULL || s_poll_to == NULL || s_send_chat == NULL ||
        s_send_reply == NULL || s_send_text == NULL || s_soak_s == NULL ||
        s_soak_chat_arg == NULL || s_soak_every == NULL || s_soak_getme == NULL) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    s_tok_args[0] = s_tok_arg;
    s_poll_args[0] = s_poll_off;
    s_poll_args[1] = s_poll_to;
    s_send_args[0] = s_send_chat;
    s_send_args[1] = s_send_reply;
    s_send_args[2] = s_send_text;
    s_soak_args[0] = s_soak_s;
    s_soak_args[1] = s_soak_chat_arg;
    s_soak_args[2] = s_soak_every;
    s_soak_args[3] = s_soak_getme;
    ESP_ERROR_CHECK(console_register(&s_t_token));
    ESP_ERROR_CHECK(console_register(&s_t_getme));
    ESP_ERROR_CHECK(console_register(&s_t_hook));
    ESP_ERROR_CHECK(console_register(&s_t_poll));
    ESP_ERROR_CHECK(console_register(&s_t_send));
    ESP_ERROR_CHECK(console_register(&s_t_soak));
    ESP_ERROR_CHECK(console_register(&s_t_stat));
}
