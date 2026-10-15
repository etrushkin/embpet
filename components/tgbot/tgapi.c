#include "tgapi.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/platform_util.h"
#include "proto.h"
#include "sdkconfig.h"
#include "state.h"
#include "tgapi_priv.h"

static const char *TAG = "tgapi";

#define API_HOST_URL "https://api.telegram.org/"
#define API_BOT_URL  API_HOST_URL "bot"
#define METHOD_MAX   32                  /* method name, NUL included */
#define URL_SIZE     (sizeof(API_BOT_URL) + CFG_TOKEN_MAX + 1 + METHOD_MAX)
#define PEEK_BYTES   256                 /* read from a too-large body to find its first id */
#define IDLE_CLOSE_US (60 * 1000000LL)   /* reconnect instead of reusing a session idle this long */

/* The tags whose DEBUG output can contain the request line (and so the token), plus the
 * transport and TLS tags below them. esp_http_client, tcp_transport and esp-tls are also
 * compiled with LOG_LOCAL_LEVEL = WARN (top-level CMakeLists.txt), which closes the window
 * between esp_log_level_set("*") and the next tgapi_log_guard(). */
static const char *const s_http_tags[] = {
    "HTTP_CLIENT", "HTTP_HEADER", "HTTP_AUTH", "http_utils",
    "transport", "transport_base", "esp-tls", "esp-tls-mbedtls", "mbedtls",
};

struct tgapi_conn {
    esp_http_client_handle_t h;
    bool kept;                  /* a session kept alive by an earlier request may be open */
    int64_t last_us;            /* end of the latest request on the kept session */
};

/* s_lock guards s_bot and s_token_gen. state_set_tg() is never called with it held. */
static SemaphoreHandle_t s_lock;
static char s_bot[STATE_BOT_LEN];       /* username from the latest getMe with this token */
static uint32_t s_token_gen;            /* bumped on every token change */

/* Event emission follows the state.h subscriber rule. */
static SemaphoreHandle_t s_emit_lock;
static uint32_t s_emit_gen;
static state_tg_state_t s_emit_state = STATE_TG_UNCONFIGURED;
static char s_emit_bot[STATE_BOT_LEN];

static const char *const s_state_names[] = {
    [STATE_TG_UNCONFIGURED] = "unconfigured",
    [STATE_TG_OFFLINE] = "offline",
    [STATE_TG_ONLINE] = "online",
    [STATE_TG_AUTH_ERROR] = "auth_error",
};

static void on_state(const state_snapshot_t *snap, uint32_t changed, void *ctx)
{
    (void)changed;
    (void)ctx;
    xSemaphoreTake(s_emit_lock, portMAX_DELAY);
    if (snap->gen > s_emit_gen) {
        s_emit_gen = snap->gen;
        if (snap->tg.state != s_emit_state || strcmp(snap->tg.bot, s_emit_bot) != 0) {
            cJSON *f = cJSON_CreateObject();
            if (f != NULL) {
                /* Remember what was emitted only if the event could be built, so a later
                 * snapshot still reports the change after an out-of-memory. */
                s_emit_state = snap->tg.state;
                memcpy(s_emit_bot, snap->tg.bot, sizeof(s_emit_bot));
                cJSON_AddStringToObject(f, "state", s_state_names[snap->tg.state]);
                cJSON_AddStringToObject(f, "detail", snap->tg.detail);
                proto_evt("tg", f);
            }
        }
    }
    xSemaphoreGive(s_emit_lock);

    /* Wi-Fi down means Telegram is unreachable now, not after the next request times out.
     * Checked on the current state (snap may be stale) and outside s_emit_lock, because
     * state_set_tg() calls back into this function. */
    if (snap->wifi.state == STATE_WIFI_DOWN) {
        state_snapshot_t now;
        state_get(&now);
        if (now.wifi.state == STATE_WIFI_DOWN && now.tg.state == STATE_TG_ONLINE) {
            state_set_tg(STATE_TG_OFFLINE, now.tg.bot, "wifi down");
        }
    }
}

static bool have_token(void)
{
    char tok[CFG_TOKEN_MAX + 1];
    bool ok = cfg_get_tg_token(tok, sizeof(tok)) == ESP_OK && cfg_token_valid(tok);
    mbedtls_platform_zeroize(tok, sizeof(tok));
    return ok;
}

/* cfg change callback: runs on the caller's task (the console task for `tg token`). */
static void on_cfg_change(cfg_key_t key, void *ctx)
{
    (void)ctx;
    if (key != CFG_KEY_TG_TOKEN) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_token_gen++;
    s_bot[0] = '\0';
    xSemaphoreGive(s_lock);
    if (have_token()) {
        state_set_tg(STATE_TG_OFFLINE, NULL, "token changed");
    } else {
        state_set_tg(STATE_TG_UNCONFIGURED, NULL, "no token");
    }
}

void tgapi_log_guard(esp_log_level_t level)
{
    esp_log_level_t cap = level < ESP_LOG_WARN ? level : ESP_LOG_WARN;
    for (size_t i = 0; i < sizeof(s_http_tags) / sizeof(s_http_tags[0]); i++) {
        esp_log_level_set(s_http_tags[i], cap);
    }
}

esp_err_t tgapi_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_emit_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL || s_emit_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (!state_subscribe(on_state, NULL)) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = cfg_subscribe(on_cfg_change, NULL);
    if (err != ESP_OK) {
        return err;
    }
    tgapi_log_guard(esp_log_get_default_level());
    if (have_token()) {
        state_set_tg(STATE_TG_OFFLINE, NULL, "starting");
    } else {
        state_set_tg(STATE_TG_UNCONFIGURED, NULL, "no token");
    }
#ifdef CONFIG_EMBPET_TG_TEST
    tgapi_test_register();
#endif
    return ESP_OK;
}

tgapi_conn_t *tgapi_conn_create(tgapi_conn_kind_t kind)
{
    tgapi_conn_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    const esp_http_client_config_t cfg = {
        .url = API_HOST_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = kind == TGAPI_CONN_POLL ? (CONFIG_EMBPET_TG_POLL_TIMEOUT_S + 10) * 1000
                                              : CONFIG_EMBPET_TG_SEND_TIMEOUT_MS,
        .disable_auto_redirect = true,
        .max_authorization_retries = -1,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* TCP keepalive notices a dead peer on an idle kept session (30 s + 3 x 5 s) */
        .keep_alive_enable = true,
        .keep_alive_idle = 30,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
    };
    c->h = esp_http_client_init(&cfg);
    if (c->h == NULL ||
        esp_http_client_set_header(c->h, "Content-Type", "application/json") != ESP_OK) {
        if (c->h != NULL) {
            esp_http_client_cleanup(c->h);
        }
        free(c);
        return NULL;
    }
    return c;
}

void tgapi_conn_close(tgapi_conn_t *c)
{
    esp_http_client_close(c->h);
    c->kept = false;
}

/* Point the connection at bot<token>/<method>. The host never changes, so a kept-alive
 * session stays usable. Local copies of the token are wiped. */
static tg_status_t set_url(tgapi_conn_t *c, const char *method, const char *token)
{
    if (method == NULL || strlen(method) >= METHOD_MAX) {
        return TG_ERR_BAD_ARG;
    }
    char tok[CFG_TOKEN_MAX + 1];
    char url[URL_SIZE];
    tg_status_t st = TG_ERR_NO_TOKEN;
    if (token == NULL) {
        if (cfg_get_tg_token(tok, sizeof(tok)) != ESP_OK) {
            tok[0] = '\0';
        }
    } else if (strlen(token) <= CFG_TOKEN_MAX) {
        strcpy(tok, token);             /* a candidate token, not stored yet */
    } else {
        tok[0] = '\0';
    }
    /* A valid token is [0-9]+:[A-Za-z0-9_-]+, so the URL always parses and
     * esp_http_client_set_url never logs it in its parse-error message. */
    if (cfg_token_valid(tok)) {
        snprintf(url, sizeof(url), API_BOT_URL "%s/%s", tok, method);
        esp_err_t err = esp_http_client_set_url(c->h, url);
        st = err == ESP_OK ? TG_OK : err == ESP_ERR_NO_MEM ? TG_ERR_NO_MEM : TG_ERR_BAD_ARG;
    }
    mbedtls_platform_zeroize(url, sizeof(url));
    mbedtls_platform_zeroize(tok, sizeof(tok));
    return st;
}

/* Too large: read the start of the body for its first update_id, then drop the session
 * (the rest is never read). `head` / `n` are what was already read, if anything. */
static tg_status_t too_large(tgapi_conn_t *c, int status, size_t total, size_t max_len,
                             const char *head, size_t n, tg_resp_t *out)
{
    char *peek = NULL;
    if (head == NULL) {
        peek = malloc(PEEK_BYTES);
        if (peek != NULL) {
            int r = esp_http_client_read_response(c->h, peek, PEEK_BYTES);
            n = r > 0 ? (size_t)r : 0;
        }
        head = peek;
    }
    tgapi_conn_close(c);
    tg_status_t st = tg_parse_response(status, NULL, total, max_len, out);
    int64_t id = 0;
    if (head != NULL && tg_updates_first_id(head, n, &id)) {
        out->first_update_id = id;
    }
    free(peek);
    return st;
}

/* Send the request and read the answer (at most max_len bytes) into a buffer sized by
 * Content-Length. */
static tg_status_t request(tgapi_conn_t *c, const char *body, size_t max_len, tg_resp_t *out,
                           esp_err_t *net_err)
{
    int blen = (int)strlen(body);
    int64_t clen = 0;
    if (c->kept && esp_timer_get_time() - c->last_us > IDLE_CLOSE_US) {
        /* The server may drop an idle session at any moment; do not race it. */
        tgapi_conn_close(c);
    }
    for (int attempt = 0;; attempt++) {
        bool kept = c->kept;
        c->kept = false;
        esp_err_t err = esp_http_client_open(c->h, blen);
        if (err == ESP_OK && esp_http_client_write(c->h, body, blen) != blen) {
            err = ESP_ERR_HTTP_WRITE_DATA;
        }
        if (err == ESP_OK) {
            clen = esp_http_client_fetch_headers(c->h);
            if (clen == -ESP_ERR_HTTP_EAGAIN) {
                tgapi_conn_close(c);
                *net_err = ESP_ERR_HTTP_EAGAIN;
                return TG_ERR_TIMEOUT;
            }
            if (clen < 0) {
                err = ESP_ERR_HTTP_FETCH_HEADER;
            }
        }
        if (err == ESP_OK) {
            break;
        }
        tgapi_conn_close(c);
        *net_err = err;
        if (!kept || attempt > 0) {
            return TG_ERR_NET;
        }
        /* The server closed the idle session before this request reached it: retry once
         * on a fresh one. */
        ESP_LOGD(TAG, "kept-alive session gone (%s), reconnecting", esp_err_to_name(err));
    }

    int status = esp_http_client_get_status_code(c->h);
    if (clen > 0 && (uint64_t)clen > max_len) {
        return too_large(c, status, (size_t)clen, max_len, NULL, 0, out);
    }
    /* Content-Length known: exactly that. Chunked (or none): one byte over the cap, so an
     * oversized body shows up as a full buffer. */
    size_t cap = clen > 0 ? (size_t)clen : max_len + 1;
    char *buf = malloc(cap);
    if (buf == NULL) {
        tgapi_conn_close(c);
        return TG_ERR_NO_MEM;
    }
    int r = esp_http_client_read_response(c->h, buf, (int)cap);
    size_t n = r > 0 ? (size_t)r : 0;
    tg_status_t st;
    if (n > max_len) {
        st = too_large(c, status, n, max_len, buf, n, out);
    } else if (!esp_http_client_is_complete_data_received(c->h)) {
        tgapi_conn_close(c);
        *net_err = ESP_ERR_HTTP_INCOMPLETE_DATA;
        st = TG_ERR_NET;
    } else {
        if (esp_http_client_is_persistent_connection(c->h)) {
            c->kept = true;
            c->last_us = esp_timer_get_time();
        } else {
            tgapi_conn_close(c);
        }
        st = tg_parse_response(status, buf, n, max_len, out);
    }
    free(buf);
    return st;
}

/* Run one call without touching the tg state. *gen is the token generation it ran with. */
static tg_status_t call_raw(tgapi_conn_t *c, const char *method, const char *body,
                            size_t max_len, tg_resp_t *out, uint32_t *gen, esp_err_t *net_err,
                            const char *token)
{
    memset(out, 0, sizeof(*out));
    *net_err = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *gen = s_token_gen;                 /* before the token is read: a change makes it stale */
    xSemaphoreGive(s_lock);
    tg_status_t st = set_url(c, method, token);
    if (st == TG_OK) {
        st = request(c, body != NULL ? body : "{}", max_len, out, net_err);
    }
    return st;
}

/* Map a call result to the tg state, unless the token changed meanwhile. */
static void finish(const char *method, tg_status_t st, uint32_t gen, const tg_resp_t *r,
                   esp_err_t net_err)
{
    char bot[STATE_BOT_LEN];
    char detail[STATE_DETAIL_LEN];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool stale = gen != s_token_gen;
    memcpy(bot, s_bot, sizeof(bot));
    xSemaphoreGive(s_lock);

    if (st != TG_OK) {
        esp_log_level_t lvl = st == TG_ERR_AUTH || st == TG_ERR_BAD_ARG ? ESP_LOG_WARN
                                                                         : ESP_LOG_INFO;
        ESP_LOG_LEVEL_LOCAL(lvl, TAG, "%s: %s (http %d, code %d, %s) %s", method,
                            tg_status_name(st), r->http_status, r->error_code,
                            esp_err_to_name(net_err), r->description);
    }
    if (stale) {
        return;
    }
    switch (st) {
    case TG_OK:
    case TG_ERR_API:
    case TG_ERR_RETRY:
    case TG_ERR_TOO_LARGE: {
        /* Telegram answered. Not online if Wi-Fi went down while the answer was read. */
        state_snapshot_t now;
        state_get(&now);
        if (now.wifi.state == STATE_WIFI_UP) {
            snprintf(detail, sizeof(detail), "%s%s", bot[0] != '\0' ? "@" : "",
                     bot[0] != '\0' ? bot : "reachable");
            state_set_tg(STATE_TG_ONLINE, bot, detail);
            /* Wi-Fi may have gone down between the check and the set, and its callback
             * then saw tg not yet online: undo it here. */
            state_get(&now);
            if (now.wifi.state == STATE_WIFI_DOWN && now.tg.state == STATE_TG_ONLINE) {
                state_set_tg(STATE_TG_OFFLINE, bot, "wifi down");
            }
        }
        break;
    }
    case TG_ERR_NET:
        snprintf(detail, sizeof(detail), "network: %s", esp_err_to_name(net_err));
        state_set_tg(STATE_TG_OFFLINE, bot, detail);
        break;
    case TG_ERR_TIMEOUT:
        state_set_tg(STATE_TG_OFFLINE, bot, "timeout");
        break;
    case TG_ERR_SERVER:
        snprintf(detail, sizeof(detail), "server error %d", r->error_code);
        state_set_tg(STATE_TG_OFFLINE, bot, detail);
        break;
    case TG_ERR_AUTH:
        snprintf(detail, sizeof(detail), "%d %.32s", r->error_code, r->description);
        state_set_tg(STATE_TG_AUTH_ERROR, NULL, detail);
        break;
    case TG_ERR_NO_TOKEN:
        state_set_tg(STATE_TG_UNCONFIGURED, NULL, "no token");
        break;
    default:                    /* BAD_ARG, NO_MEM, TOO_LONG: says nothing about Telegram */
        break;
    }
}

static tg_status_t call(tgapi_conn_t *c, const char *method, const char *body, size_t max_len,
                        tg_resp_t *out)
{
    uint32_t gen;
    esp_err_t net_err;
    tg_status_t st = call_raw(c, method, body, max_len, out, &gen, &net_err, NULL);
    finish(method, st, gen, out, net_err);
    return st;
}

tg_status_t tgapi_call(tgapi_conn_t *c, const char *method, const char *body, tg_resp_t *out)
{
    return call(c, method, body, TG_RESP_MAX, out);
}

/* Hand the error details to the caller; the response itself is freed. */
static void give_err(tg_resp_t *r, tg_resp_t *err)
{
    tg_resp_free(r);
    if (err != NULL) {
        *err = *r;
    }
}

tg_status_t tgapi_get_me(tgapi_conn_t *c, char *bot, size_t cap, tg_resp_t *err)
{
    tg_resp_t r;
    uint32_t gen;
    esp_err_t net_err;
    tg_status_t st = call_raw(c, "getMe", NULL, TG_RESP_MAX, &r, &gen, &net_err, NULL);
    if (st == TG_OK) {
        char name[STATE_BOT_LEN];
        if (tg_parse_bot_username(r.result, name, sizeof(name)) && strlen(name) < cap) {
            strcpy(bot, name);
            /* Remember it before the state update, so the online event carries it. */
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (gen == s_token_gen) {
                memcpy(s_bot, name, sizeof(s_bot));
            }
            xSemaphoreGive(s_lock);
        } else {
            st = TG_ERR_SERVER;
        }
    }
    finish("getMe", st, gen, &r, net_err);
    give_err(&r, err);
    return st;
}

tg_status_t tgapi_check_token(tgapi_conn_t *c, const char *token, char *bot, size_t cap,
                              tg_resp_t *err)
{
    tg_resp_t r;
    uint32_t gen;
    esp_err_t net_err;
    tg_status_t st = call_raw(c, "getMe", NULL, TG_RESP_MAX, &r, &gen, &net_err, token);
    if (st == TG_OK) {
        char name[STATE_BOT_LEN];
        if (tg_parse_bot_username(r.result, name, sizeof(name)) && strlen(name) < cap) {
            strcpy(bot, name);
        } else {
            st = TG_ERR_SERVER;
        }
    }
    /* The token is not the stored one: the answer says nothing about the tg state. Drop the
     * session, and overwrite the URL path, which esp_http_client_close keeps, with the
     * stored token's (a candidate must not linger in the handle). */
    tgapi_conn_close(c);
    if (set_url(c, "getMe", NULL) != TG_OK) {
        esp_http_client_set_url(c->h, API_HOST_URL);    /* no stored token: no token path */
    }
    give_err(&r, err);
    return st;
}

tg_status_t tgapi_delete_webhook(tgapi_conn_t *c, tg_resp_t *err)
{
    tg_resp_t r;
    tg_status_t st = tgapi_call(c, "deleteWebhook", "{\"drop_pending_updates\":false}", &r);
    give_err(&r, err);
    return st;
}

tg_status_t tgapi_send_message(tgapi_conn_t *c, int64_t chat_id, const char *actor,
                               const char *text, int64_t reply_to, int64_t *msg_id,
                               tg_resp_t *err)
{
    tg_resp_t r;
    memset(&r, 0, sizeof(r));
    char *body = NULL;
    tg_status_t st = tg_build_send_message(chat_id, actor, text, reply_to, &body);
    if (st == TG_OK) {
        st = tgapi_call(c, "sendMessage", body, &r);
        cJSON_free(body);
    }
    if (st == TG_OK && !tg_parse_message_id(r.result, msg_id)) {
        st = TG_ERR_SERVER;
    }
    give_err(&r, err);
    return st;
}

tg_status_t tgapi_get_updates(tgapi_conn_t *c, int64_t offset, tg_resp_t *out)
{
    char *body = tg_build_get_updates(offset, TG_UPDATES_LIMIT, CONFIG_EMBPET_TG_POLL_TIMEOUT_S);
    if (body == NULL) {
        memset(out, 0, sizeof(*out));
        return TG_ERR_NO_MEM;
    }
    tg_status_t st = tgapi_call(c, "getUpdates", body, out);
    cJSON_free(body);
    return st;
}

tg_status_t tgapi_get_updates_one(tgapi_conn_t *c, int64_t offset, tg_resp_t *out)
{
    char *body = tg_build_get_updates(offset, 1, 0);
    if (body == NULL) {
        memset(out, 0, sizeof(*out));
        return TG_ERR_NO_MEM;
    }
    tg_status_t st = call(c, "getUpdates", body, TG_RESP_MAX_ONE, out);
    cJSON_free(body);
    if (st == TG_ERR_TOO_LARGE) {
        ESP_LOGW(TAG, "update %lld is %u bytes, over the %u-byte cap even alone: "
                 "the poller has to skip it", (long long)out->first_update_id,
                 (unsigned)out->body_len, (unsigned)TG_RESP_MAX_ONE);
    }
    return st;
}
