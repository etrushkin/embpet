#include "state.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_lock;
static state_snapshot_t s_state;
static struct {
    state_cb_t cb;
    void *ctx;
} s_subs[STATE_MAX_SUBSCRIBERS];
static int s_nsubs;

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (src == NULL) {
        src = "";
    }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

void state_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock != NULL);
    memset(&s_state, 0, sizeof(s_state));
    s_state.wifi.state = STATE_WIFI_DOWN;
    s_state.tg.state = STATE_TG_UNCONFIGURED;
    s_state.booting = true;
    s_nsubs = 0;
}

bool state_subscribe(state_cb_t cb, void *ctx)
{
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (cb != NULL && s_nsubs < STATE_MAX_SUBSCRIBERS) {
        s_subs[s_nsubs].cb = cb;
        s_subs[s_nsubs].ctx = ctx;
        s_nsubs++;
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

void state_get(state_snapshot_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_lock);
}

/* Notify outside the lock. Subscribers are append-only, so reading the count and the
 * entries taken under the lock stays valid. */
static void notify(const state_snapshot_t *snap, uint32_t changed, int nsubs)
{
    for (int i = 0; i < nsubs; i++) {
        s_subs[i].cb(snap, changed, s_subs[i].ctx);
    }
}

void state_set_wifi(state_wifi_state_t state, const char *ip, int8_t rssi)
{
    char new_ip[STATE_IP_LEN];
    copy_str(new_ip, sizeof(new_ip), ip);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = s_state.wifi.state != state || strcmp(s_state.wifi.ip, new_ip) != 0;
    s_state.wifi.state = state;
    memcpy(s_state.wifi.ip, new_ip, sizeof(new_ip));
    s_state.wifi.rssi = rssi;
    s_state.gen += changed;
    state_snapshot_t snap = s_state;
    int n = s_nsubs;
    xSemaphoreGive(s_lock);

    if (changed) {
        notify(&snap, STATE_CH_WIFI, n);
    }
}

void state_set_tg(state_tg_state_t state, const char *bot, const char *detail)
{
    char new_bot[STATE_BOT_LEN];
    char new_detail[STATE_DETAIL_LEN];
    copy_str(new_bot, sizeof(new_bot), bot);
    copy_str(new_detail, sizeof(new_detail), detail);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = s_state.tg.state != state || strcmp(s_state.tg.bot, new_bot) != 0;
    s_state.tg.state = state;
    memcpy(s_state.tg.bot, new_bot, sizeof(new_bot));
    memcpy(s_state.tg.detail, new_detail, sizeof(new_detail));
    s_state.gen += changed;
    state_snapshot_t snap = s_state;
    int n = s_nsubs;
    xSemaphoreGive(s_lock);

    if (changed) {
        notify(&snap, STATE_CH_TG, n);
    }
}

void state_set_pending(uint32_t count)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = s_state.pending != count;
    s_state.pending = count;
    s_state.gen += changed;
    state_snapshot_t snap = s_state;
    int n = s_nsubs;
    xSemaphoreGive(s_lock);

    if (changed) {
        notify(&snap, STATE_CH_PENDING, n);
    }
}

void state_set_booting(bool booting)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = s_state.booting != booting;
    s_state.booting = booting;
    s_state.gen += changed;
    state_snapshot_t snap = s_state;
    int n = s_nsubs;
    xSemaphoreGive(s_lock);

    if (changed) {
        notify(&snap, STATE_CH_BOOTING, n);
    }
}

void state_set_configured(bool configured)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = s_state.configured != configured;
    s_state.configured = configured;
    s_state.gen += changed;
    state_snapshot_t snap = s_state;
    int n = s_nsubs;
    xSemaphoreGive(s_lock);

    if (changed) {
        notify(&snap, STATE_CH_CONFIGURED, n);
    }
}
