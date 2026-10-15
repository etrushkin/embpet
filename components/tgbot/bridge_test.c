/* Test-only build (CONFIG_EMBPET_TG_TEST with the bridge, tools/bridge_test.py), never in
 * production. Commands to drive the bridge without a Telegram user and to measure it:
 *
 *  - tg t_owner <chat>         store the owner chat id (0 clears it); bypasses `tg owner`
 *                              (accepts a negative id without --). Answers ok only.
 *  - tg t_pairttl <s>          validity of the next `tg pair` (expiry tests)
 *  - tg t_live <0|1>           1: the online notice / pairing confirmation really go out
 *                              (default 0: recorded only)
 *  - tg t_notice               notices / confirmations since boot, their latest text,
 *                              pairing seconds left
 *  - tg t_untoken              clear the bot token (tg state unconfigured: nothing is sent)
 *  - tg t_pend <msg> -a A      record msg as pending for A, exactly as a successful
 *                              tg send does (pending / pending_dropped events)
 *  - tg t_inject -- <json>     route one getUpdates update object as tg_poll would
 *                              (reply event, answered mark, or a hint request)
 *  - tg t_bridge               hint, getUpdates and probe counters, stack headroom of
 *                              console / tg_poll / tg_send, heap
 *  - tg t_fault <0|1>          make every getUpdates fail like a network error while getMe
 *                              still works (flap / backoff test) */
#include "bridge_priv.h"

#include <stdlib.h>
#include <string.h>

#include "argtable3/argtable3.h"
#include "cfg.h"
#include "console.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void oom(console_req_t *req)
{
    console_err(req, CONSOLE_ERR_INTERNAL, "out of memory");
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

/* ---- tg t_owner <chat> ---- */

static struct arg_str *s_owner_arg;
static void *s_owner_args[2];

static void t_owner(console_req_t *req)
{
    int64_t chat;
    if (!parse_i64(s_owner_arg->sval[0], &chat)) {
        console_err(req, CONSOLE_ERR_BAD_ARGS, "<chat> must be an integer (0 clears)");
        return;
    }
    esp_err_t err = chat == 0 ? cfg_clear_tg_owner() : cfg_set_tg_owner(chat);
    if (err != ESP_OK) {
        console_err(req, err == ESP_ERR_INVALID_ARG ? CONSOLE_ERR_BAD_ARGS : CONSOLE_ERR_INTERNAL,
                    esp_err_to_name(err));
        return;
    }
    console_ok(req, NULL);
}

static const console_cmd_t s_t_owner = {
    .name = "tg t_owner",
    .help = "TEST BUILD: store the owner chat id (0 clears)",
    .args = s_owner_args,
    .handler = t_owner,
};

/* ---- tg t_untoken ---- */

static void t_untoken(console_req_t *req)
{
    esp_err_t err = cfg_clear_tg_token();
    if (err != ESP_OK) {
        console_err(req, CONSOLE_ERR_INTERNAL, esp_err_to_name(err));
        return;
    }
    console_ok(req, NULL);
}

static const console_cmd_t s_t_untoken = {
    .name = "tg t_untoken",
    .help = "TEST BUILD: clear the bot token",
    .handler = t_untoken,
};

/* ---- tg t_pend <msg> -a A ---- */

static struct arg_str *s_pend_arg;
static void *s_pend_args[2];

static void t_pend(console_req_t *req)
{
    const char *actor = console_ctx(req)->actor;
    int64_t msg;
    if (actor == NULL || !parse_i64(s_pend_arg->sval[0], &msg) || msg <= 0) {
        console_err(req, CONSOLE_ERR_BAD_ARGS, "<msg> > 0 and -a <actor>");
        return;
    }
    bridge_test_add_pending(msg, actor);
    console_ok(req, NULL);
}

static const console_cmd_t s_t_pend = {
    .name = "tg t_pend",
    .help = "TEST BUILD: record <msg> as pending for -a A, as tg send does",
    .args = s_pend_args,
    .handler = t_pend,
};

/* ---- tg t_inject -- <json> ---- */

static struct arg_str *s_inject_arg;
static void *s_inject_args[2];

static void t_inject(console_req_t *req)
{
    cJSON *u = cJSON_Parse(s_inject_arg->sval[0]);
    if (u == NULL) {
        console_err(req, CONSOLE_ERR_BAD_ARGS, "not JSON");
        return;
    }
    bool handled = bridge_test_inject(u);
    cJSON_Delete(u);
    cJSON *p = cJSON_CreateObject();
    if (p == NULL || cJSON_AddBoolToObject(p, "handled", handled) == NULL) {
        cJSON_Delete(p);
        oom(req);
        return;
    }
    console_ok(req, p);
}

static const console_cmd_t s_t_inject = {
    .name = "tg t_inject",
    .help = "TEST BUILD: route one update object as the poller would",
    .args = s_inject_args,
    .handler = t_inject,
};

/* ---- tg t_bridge ---- */

static void add_stack(cJSON *p, const char *key, const char *task)
{
    TaskHandle_t h = xTaskGetHandle(task);
    if (h != NULL) {
        cJSON_AddNumberToObject(p, key, (double)uxTaskGetStackHighWaterMark(h));
    }
}

static void t_bridge(console_req_t *req)
{
    bridge_test_counters_t n;
    bridge_test_counters(&n);
    cJSON *p = cJSON_CreateObject();
    if (p == NULL) {
        oom(req);
        return;
    }
    cJSON_AddNumberToObject(p, "hints_sent", n.hints_sent);
    cJSON_AddNumberToObject(p, "hints_dropped", n.hints_dropped);
    cJSON_AddNumberToObject(p, "polls", n.polls);
    cJSON_AddNumberToObject(p, "probes", n.probes);
    add_stack(p, "stack_console", "console");
    add_stack(p, "stack_tg_poll", "tg_poll");
    add_stack(p, "stack_tg_send", "tg_send");
    cJSON_AddNumberToObject(p, "heap_free", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(p, "heap_min", (double)esp_get_minimum_free_heap_size());
    console_ok(req, p);
}

static const console_cmd_t s_t_bridge = {
    .name = "tg t_bridge",
    .help = "TEST BUILD: hint/poll/probe counters, console/tg task stack headroom, heap",
    .handler = t_bridge,
};

/* ---- tg t_fault <0|1> ---- */

static struct arg_int *s_fault_arg;
static void *s_fault_args[2];

static void t_fault(console_req_t *req)
{
    int v = s_fault_arg->ival[0];
    if (v != 0 && v != 1) {
        console_err(req, CONSOLE_ERR_BAD_ARGS, "0 or 1");
        return;
    }
    bridge_test_fault(v == 1);
    console_ok(req, NULL);
}

static const console_cmd_t s_t_fault = {
    .name = "tg t_fault",
    .help = "TEST BUILD: 1 = every getUpdates fails like a network error (getMe still works)",
    .args = s_fault_args,
    .handler = t_fault,
};

/* ---- tg t_pairttl <s> / tg t_live <0|1> / tg t_notice ---- */

static struct arg_int *s_ttl_arg;
static void *s_ttl_args[2];

static void t_pairttl(console_req_t *req)
{
    int v = s_ttl_arg->ival[0];
    if (v < 1 || v > 300) {
        console_err(req, CONSOLE_ERR_BAD_ARGS, "1..300");
        return;
    }
    bridge_test_pair_ttl((uint32_t)v);
    console_ok(req, NULL);
}

static const console_cmd_t s_t_pairttl = {
    .name = "tg t_pairttl",
    .help = "TEST BUILD: validity in seconds of the next tg pair (default 300)",
    .args = s_ttl_args,
    .handler = t_pairttl,
};

static struct arg_int *s_live_arg;
static void *s_live_args[2];

static void t_live(console_req_t *req)
{
    int v = s_live_arg->ival[0];
    if (v != 0 && v != 1) {
        console_err(req, CONSOLE_ERR_BAD_ARGS, "0 or 1");
        return;
    }
    bridge_test_live(v == 1);
    console_ok(req, NULL);
}

static const console_cmd_t s_t_live = {
    .name = "tg t_live",
    .help = "TEST BUILD: 1 = online notice and pairing confirmation really go to Telegram",
    .args = s_live_args,
    .handler = t_live,
};

static void t_notice(console_req_t *req)
{
    bridge_test_notice_t n;
    bridge_test_notice(&n);
    cJSON *p = cJSON_CreateObject();
    if (p == NULL) {
        oom(req);
        return;
    }
    cJSON_AddBoolToObject(p, "live", n.live);
    cJSON_AddNumberToObject(p, "notices", n.notices);
    cJSON_AddNumberToObject(p, "confirms", n.confirms);
    cJSON_AddNumberToObject(p, "pair_left_s", n.pair_left_s);
    cJSON_AddStringToObject(p, "notice_text", n.notice_text);
    cJSON_AddStringToObject(p, "confirm_text", n.confirm_text);
    console_ok(req, p);
}

static const console_cmd_t s_t_notice = {
    .name = "tg t_notice",
    .help = "TEST BUILD: online notices / pairing confirmations since boot, pairing time left",
    .handler = t_notice,
};

void bridge_test_register(void)
{
    s_ttl_arg = arg_int1(NULL, NULL, "<s>", "pairing validity");
    s_live_arg = arg_int1(NULL, NULL, "<0|1>", "really send notice/confirmation");
    if (s_ttl_arg == NULL || s_live_arg == NULL) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    s_ttl_args[0] = s_ttl_arg;
    s_live_args[0] = s_live_arg;
    ESP_ERROR_CHECK(console_register(&s_t_pairttl));
    ESP_ERROR_CHECK(console_register(&s_t_live));
    ESP_ERROR_CHECK(console_register(&s_t_notice));
    s_owner_arg = arg_str1(NULL, NULL, "<chat>", "owner chat id, 0 clears");
    s_pend_arg = arg_str1(NULL, NULL, "<msg>", "message id");
    s_inject_arg = arg_str1(NULL, NULL, "<json>", "one update object");
    s_fault_arg = arg_int1(NULL, NULL, "<0|1>", "getUpdates fault off/on");
    if (s_owner_arg == NULL || s_pend_arg == NULL || s_inject_arg == NULL ||
        s_fault_arg == NULL) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    s_owner_args[0] = s_owner_arg;
    s_pend_args[0] = s_pend_arg;
    s_inject_args[0] = s_inject_arg;
    s_fault_args[0] = s_fault_arg;
    ESP_ERROR_CHECK(console_register(&s_t_owner));
    ESP_ERROR_CHECK(console_register(&s_t_untoken));
    ESP_ERROR_CHECK(console_register(&s_t_pend));
    ESP_ERROR_CHECK(console_register(&s_t_inject));
    ESP_ERROR_CHECK(console_register(&s_t_bridge));
    ESP_ERROR_CHECK(console_register(&s_t_fault));
}
