/* Console commands: tg send, tg pending, tg ack, tg status, tg token, tg pair, tg owner
 * (README 3.4). */
#include "bridge_priv.h"

#include <stdlib.h>
#include <string.h>

#include "argtable3/argtable3.h"
#include "cfg.h"
#include "console.h"
#include "mbedtls/platform_util.h"
#include "owner_core.h"
#include "state.h"

#define MSG_ID_MAX ((1LL << 53) - 1)    /* larger ids would not survive a JSON number */

static const char *const s_state_names[] = {
    [STATE_TG_UNCONFIGURED] = "unconfigured",
    [STATE_TG_OFFLINE] = "offline",
    [STATE_TG_ONLINE] = "online",
    [STATE_TG_AUTH_ERROR] = "auth_error",
};

static void oom(console_req_t *req)
{
    console_err(req, CONSOLE_ERR_INTERNAL, "out of memory");
}

static bool all_digits(const char *s, size_t max_len)
{
    size_t n = strlen(s);
    if (n == 0 || n > max_len) {
        return false;
    }
    for (size_t k = 0; k < n; k++) {
        if (s[k] < '0' || s[k] > '9') {
            return false;
        }
    }
    return true;
}

/* Telegram message id: 1 .. 2^53-1, decimal. */
static bool parse_msg_id(const char *s, int64_t *out)
{
    if (!all_digits(s, 16)) {
        return false;
    }
    long long v = strtoll(s, NULL, 10);
    if (v < 1 || v > MSG_ID_MAX) {
        return false;
    }
    *out = v;
    return true;
}

static bool parse_u32(const char *s, uint32_t *out)
{
    if (!all_digits(s, 10)) {
        return false;
    }
    unsigned long long v = strtoull(s, NULL, 10);
    if (v > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

/* ---- tg send -a A [-r MSG] "<text>" ---- */

static struct arg_str *s_send_reply;
static struct arg_str *s_send_text;
static void *s_send_args[3];

static void tg_send(console_req_t *req)
{
    const char *actor = console_ctx(req)->actor;
    int64_t reply_to = 0;
    if (actor == NULL) {
        console_err(req, CONSOLE_ERR_BAD_ARGS, "-a <actor> is required");
        return;
    }
    if (s_send_reply->count > 0 && !parse_msg_id(s_send_reply->sval[0], &reply_to)) {
        console_err(req, CONSOLE_ERR_BAD_ARGS, "-r <msg> must be a message id");
        return;
    }
    char msg[128];
    int64_t id = 0;
    const char *code = bridge_send(actor, s_send_text->sval[0], reply_to, &id, msg, sizeof(msg));
    if (code != NULL) {
        console_err(req, code, msg);
        return;
    }
    cJSON *p = cJSON_CreateObject();
    if (p == NULL || cJSON_AddNumberToObject(p, "msg", (double)id) == NULL) {
        cJSON_Delete(p);
        oom(req);
        return;
    }
    console_ok(req, p);
}

static const console_cmd_t s_tg_send = {
    .name = "tg send",
    .help = "Send \"[A] text\" to the owner as actor A (-a required); -r replies to a message",
    .args = s_send_args,
    .handler = tg_send,
};

/* ---- tg pending [-a A] ---- */

static void tg_pending(console_req_t *req)
{
    pending_item_t *items = malloc(sizeof(*items) * EMBPET_PENDING_MAX);
    cJSON *p = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(p, "items");
    bool ok = items != NULL && arr != NULL;
    if (ok) {
        size_t n = bridge_pending_list(console_ctx(req)->actor, items, EMBPET_PENDING_MAX);
        for (size_t k = 0; ok && k < n && k < EMBPET_PENDING_MAX; k++) {
            cJSON *o = cJSON_CreateObject();
            ok = cJSON_AddItemToArray(arr, o) &&
                 cJSON_AddNumberToObject(o, "msg", (double)items[k].msg_id) != NULL &&
                 cJSON_AddStringToObject(o, "actor", items[k].actor) != NULL &&
                 cJSON_AddNumberToObject(o, "age_s", items[k].age_s) != NULL;
        }
    }
    free(items);
    if (!ok) {
        cJSON_Delete(p);
        oom(req);
        return;
    }
    console_ok(req, p);
}

static const console_cmd_t s_tg_pending = {
    .name = "tg pending",
    .help = "List unanswered messages, oldest first (-a: only that actor's)",
    .handler = tg_pending,
};

/* ---- tg ack --older <s> | -m <msg> ---- */

static struct arg_str *s_ack_older;
static struct arg_str *s_ack_msg;
static void *s_ack_args[3];

static void tg_ack(console_req_t *req)
{
    bool older = s_ack_older->count > 0;
    uint32_t secs = 0;
    int64_t id = 0;
    if (older == (s_ack_msg->count > 0)) {
        console_err(req, CONSOLE_ERR_BAD_ARGS, "give exactly one of --older <s> or -m <msg>");
        return;
    }
    if (older ? !parse_u32(s_ack_older->sval[0], &secs) : !parse_msg_id(s_ack_msg->sval[0], &id)) {
        console_err(req, CONSOLE_ERR_BAD_ARGS,
                    older ? "--older <s> must be 0..4294967295" : "-m <msg> must be a message id");
        return;
    }
    size_t n = older ? bridge_ack_older(secs) : bridge_ack_msg(id);
    cJSON *p = cJSON_CreateObject();
    if (p == NULL || cJSON_AddNumberToObject(p, "acked", (double)n) == NULL) {
        cJSON_Delete(p);
        oom(req);
        return;
    }
    console_ok(req, p);
}

static const console_cmd_t s_tg_ack = {
    .name = "tg ack",
    .help = "Mark answered: all older than <s> seconds (0 = all), or one message",
    .args = s_ack_args,
    .handler = tg_ack,
};

/* ---- tg status ---- */

static void tg_status(console_req_t *req)
{
    state_snapshot_t s;
    state_get(&s);
    char tok[CFG_TOKEN_MAX + 1];
    char masked[CFG_MASK_BUF_SIZE];
    if (cfg_get_tg_token(tok, sizeof(tok)) != ESP_OK) {
        tok[0] = '\0';
    }
    cfg_mask_token(tok, masked, sizeof(masked));
    mbedtls_platform_zeroize(tok, sizeof(tok));
    int64_t owner = cfg_get_tg_owner();

    cJSON *p = cJSON_CreateObject();
    bool ok = p != NULL &&
              cJSON_AddStringToObject(p, "state", s_state_names[s.tg.state]) != NULL &&
              cJSON_AddStringToObject(p, "detail", s.tg.detail) != NULL &&
              cJSON_AddStringToObject(p, "bot", s.tg.bot) != NULL &&
              (owner != 0 ? cJSON_AddNumberToObject(p, "owner", (double)owner)
                          : cJSON_AddNullToObject(p, "owner")) != NULL &&
              cJSON_AddStringToObject(p, "token", masked) != NULL &&
              cJSON_AddNumberToObject(p, "pending", s.pending) != NULL;
    if (!ok) {
        cJSON_Delete(p);
        oom(req);
        return;
    }
    console_ok(req, p);
}

static const console_cmd_t s_tg_status = {
    .name = "tg status",
    .help = "Telegram state and detail, bot, owner chat, masked token, pending count",
    .handler = tg_status,
};

/* ---- tg token <token> ---- */

static struct arg_str *s_token_arg;
static void *s_token_args[2];

static void tg_token(console_req_t *req)
{
    char bot[STATE_BOT_LEN];
    char msg[128];
    const char *code = bridge_set_token(s_token_arg->sval[0], bot, sizeof(bot), msg, sizeof(msg));
    if (code != NULL) {
        console_err(req, code, msg);
        return;
    }
    char masked[CFG_MASK_BUF_SIZE];
    cfg_mask_token(s_token_arg->sval[0], masked, sizeof(masked));
    cJSON *p = cJSON_CreateObject();
    if (p == NULL || cJSON_AddStringToObject(p, "bot", bot) == NULL ||
        cJSON_AddStringToObject(p, "token", masked) == NULL) {
        cJSON_Delete(p);
        oom(req);
        return;
    }
    console_ok(req, p);
}

static const console_cmd_t s_tg_token = {
    .name = "tg token",
    .help = "Check the bot token with Telegram, store it, restart the poller (give it after --)",
    .args = s_token_args,
    .handler = tg_token,
};

/* ---- tg pair ---- */

static void tg_pair(console_req_t *req)
{
    char code[PAIR_CODE_BUF];
    char msg[64];
    uint32_t expires_s = 0;
    const char *err = bridge_pair_start(code, &expires_s, msg, sizeof(msg));
    if (err != NULL) {
        console_err(req, err, msg);
        return;
    }
    cJSON *p = cJSON_CreateObject();
    if (p == NULL || cJSON_AddStringToObject(p, "code", code) == NULL ||
        cJSON_AddNumberToObject(p, "expires_s", expires_s) == NULL) {
        cJSON_Delete(p);
        oom(req);
        return;
    }
    console_ok(req, p);
}

static const console_cmd_t s_tg_pair = {
    .name = "tg pair",
    .help = "Print a 6-digit code (5 min); send /start <code> to the bot to bind the owner chat",
    .handler = tg_pair,
};

/* ---- tg owner [<chat_id> | --clear] ---- */

static struct arg_str *s_owner_id;
static struct arg_lit *s_owner_clear;
static void *s_owner_args[3];

static void tg_owner(console_req_t *req)
{
    bool clear = s_owner_clear->count > 0;
    esp_err_t err = ESP_OK;
    if (clear && s_owner_id->count > 0) {
        console_err(req, CONSOLE_ERR_BAD_ARGS, "give a chat id or --clear, not both");
        return;
    }
    if (clear) {
        err = bridge_owner_write(0);
    } else if (s_owner_id->count > 0) {
        int64_t chat;
        if (!owner_parse_chat(s_owner_id->sval[0], &chat)) {
            console_err(req, CONSOLE_ERR_BAD_ARGS,
                        "<chat_id> must be a non-zero integer (put -- before a negative one)");
            return;
        }
        err = bridge_owner_write(chat);
    }
    if (err != ESP_OK) {
        console_err(req, err == ESP_ERR_INVALID_ARG ? CONSOLE_ERR_BAD_ARGS : CONSOLE_ERR_INTERNAL,
                    esp_err_to_name(err));
        return;
    }
    int64_t owner = cfg_get_tg_owner();
    cJSON *p = cJSON_CreateObject();
    if (p == NULL || (owner != 0 ? cJSON_AddNumberToObject(p, "chat_id", (double)owner)
                                 : cJSON_AddNullToObject(p, "chat_id")) == NULL) {
        cJSON_Delete(p);
        oom(req);
        return;
    }
    console_ok(req, p);
}

static const console_cmd_t s_tg_owner = {
    .name = "tg owner",
    .help = "Show the owner chat, set it (<chat_id>, after -- if negative) or --clear it",
    .args = s_owner_args,
    .handler = tg_owner,
};

void tg_cmd_register(void)
{
    s_token_arg = arg_str1(NULL, NULL, "<token>", "bot token from @BotFather");
    s_owner_id = arg_str0(NULL, NULL, "<chat_id>", "owner chat id");
    s_owner_clear = arg_lit0(NULL, "clear", "unbind the owner chat");
    s_send_reply = arg_str0("r", NULL, "<msg>", "reply to this message id");
    s_send_text = arg_str1(NULL, NULL, "<text>", "message text (up to 4096 bytes)");
    s_ack_older = arg_str0(NULL, "older", "<s>", "ack all older than s seconds (0 = all)");
    s_ack_msg = arg_str0("m", NULL, "<msg>", "ack one message id");
    if (s_send_reply == NULL || s_send_text == NULL || s_ack_older == NULL || s_ack_msg == NULL ||
        s_token_arg == NULL || s_owner_id == NULL || s_owner_clear == NULL) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    s_token_args[0] = s_token_arg;
    s_owner_args[0] = s_owner_id;
    s_owner_args[1] = s_owner_clear;
    s_send_args[0] = s_send_reply;
    s_send_args[1] = s_send_text;
    s_ack_args[0] = s_ack_older;
    s_ack_args[1] = s_ack_msg;

    ESP_ERROR_CHECK(console_register(&s_tg_send));
    ESP_ERROR_CHECK(console_register(&s_tg_pending));
    ESP_ERROR_CHECK(console_register(&s_tg_ack));
    ESP_ERROR_CHECK(console_register(&s_tg_status));
    ESP_ERROR_CHECK(console_register(&s_tg_token));
    ESP_ERROR_CHECK(console_register(&s_tg_pair));
    ESP_ERROR_CHECK(console_register(&s_tg_owner));
}
