/* Console commands: sys info, sys log, sys reboot, evt since (README 3.4). */
#include "cmds.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "argtable3/argtable3.h"
#include "console.h"
#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "proto.h"

static const char *const s_level_names[] = {
    [ESP_LOG_NONE] = "none",
    [ESP_LOG_ERROR] = "error",
    [ESP_LOG_WARN] = "warn",
    [ESP_LOG_INFO] = "info",
    [ESP_LOG_DEBUG] = "debug",
};
#define N_LEVELS (sizeof(s_level_names) / sizeof(s_level_names[0]))

static esp_log_level_t s_log_level = ESP_LOG_INFO;     /* CONFIG_LOG_DEFAULT_LEVEL_INFO */

void cmd_sys_set_log_level(esp_log_level_t level)
{
    esp_log_level_set("*", level);
    s_log_level = level;
}

/* ---- sys info ---- */

static void sys_info(console_req_t *req)
{
    uint8_t mac[6] = {0};
    char mac_s[18];
    esp_efuse_mac_get_default(mac);
    snprintf(mac_s, sizeof(mac_s), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    cJSON *p = cJSON_CreateObject();
    if (p == NULL) {
        console_err(req, CONSOLE_ERR_INTERNAL, "out of memory");
        return;
    }
    cJSON_AddStringToObject(p, "fw", esp_app_get_description()->version);
    cJSON_AddStringToObject(p, "idf", esp_get_idf_version());
    cJSON_AddNumberToObject(p, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(p, "heap_free", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(p, "heap_min", (double)esp_get_minimum_free_heap_size());
    cJSON_AddStringToObject(p, "mac", mac_s);
    cJSON_AddStringToObject(p, "reset_reason", proto_reset_reason());
    cJSON_AddStringToObject(p, "log", (size_t)s_log_level < N_LEVELS ? s_level_names[s_log_level]
                                                                      : "verbose");
    cJSON_AddNumberToObject(p, "log_dropped", (double)console_log_dropped());
    console_ok(req, p);
}

static const console_cmd_t s_sys_info = {
    .name = "sys info",
    .help = "Firmware/IDF version, uptime, heap, MAC, reset reason, log level",
    .handler = sys_info,
};

/* ---- sys log <level> ---- */

static struct arg_str *s_log_arg;
static void *s_log_args[2];

static void sys_log(console_req_t *req)
{
    const char *want = s_log_arg->sval[0];
    for (size_t k = 0; k < N_LEVELS; k++) {
        if (strcmp(want, s_level_names[k]) == 0) {
            cmd_sys_set_log_level((esp_log_level_t)k);
            cJSON *p = cJSON_CreateObject();
            if (p == NULL) {
                console_err(req, CONSOLE_ERR_INTERNAL, "out of memory");
                return;
            }
            cJSON_AddStringToObject(p, "level", s_level_names[k]);
            console_ok(req, p);
            return;
        }
    }
    console_err(req, CONSOLE_ERR_BAD_ARGS, "level must be none, error, warn, info or debug");
}

static const console_cmd_t s_sys_log = {
    .name = "sys log",
    .help = "Set the console log level",
    .args = s_log_args,
    .handler = sys_log,
};

/* ---- sys reboot ---- */

static esp_timer_handle_t s_reboot_timer;

static void reboot_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static void sys_reboot(console_req_t *req)
{
    if (s_reboot_timer == NULL) {
        const esp_timer_create_args_t args = {.callback = reboot_cb, .name = "reboot"};
        if (esp_timer_create(&args, &s_reboot_timer) != ESP_OK) {
            console_err(req, CONSOLE_ERR_INTERNAL, "no timer");
            return;
        }
    }
    /* Arm first so a failure can still be reported; the res is written long before the
     * timer fires 100 ms later. */
    if (!esp_timer_is_active(s_reboot_timer) &&
        esp_timer_start_once(s_reboot_timer, 100 * 1000) != ESP_OK) {
        console_err(req, CONSOLE_ERR_INTERNAL, "cannot start reboot timer");
        return;
    }
    console_ok(req, NULL);
}

static const console_cmd_t s_sys_reboot = {
    .name = "sys reboot",
    .help = "Restart about 100 ms after the response",
    .handler = sys_reboot,
};

/* ---- evt since <seq> ---- */

static struct arg_str *s_since_arg;
static void *s_since_args[2];

static bool parse_u32(const char *s, uint32_t *out)
{
    size_t n = strlen(s);
    if (n == 0 || n > 10) {
        return false;
    }
    for (size_t k = 0; k < n; k++) {
        if (s[k] < '0' || s[k] > '9') {
            return false;
        }
    }
    unsigned long long v = strtoull(s, NULL, 10);
    if (v > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static void evt_since(console_req_t *req)
{
    uint32_t since;
    if (!parse_u32(s_since_arg->sval[0], &since)) {
        console_err(req, CONSOLE_ERR_BAD_ARGS, "seq must be 0..4294967295");
        return;
    }
    bool truncated = false;
    size_t n = proto_evt_replay(since, &truncated);
    cJSON *p = cJSON_CreateObject();
    if (p == NULL) {
        console_err(req, CONSOLE_ERR_INTERNAL, "out of memory");
        return;
    }
    cJSON_AddNumberToObject(p, "count", (double)n);
    cJSON_AddBoolToObject(p, "truncated", truncated);
    console_ok(req, p);
}

static const console_cmd_t s_evt_since = {
    .name = "evt since",
    .help = "Re-emit buffered events with seq > <seq> (0 = all), then respond",
    .args = s_since_args,
    .handler = evt_since,
};

void cmd_sys_register(void)
{
    s_log_arg = arg_str1(NULL, NULL, "<level>", "none|error|warn|info|debug");
    s_since_arg = arg_str1(NULL, NULL, "<seq>", "last seq the host has seen");
    if (s_log_arg == NULL || s_since_arg == NULL) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    s_log_args[0] = s_log_arg;
    s_since_args[0] = s_since_arg;

    ESP_ERROR_CHECK(console_register(&s_sys_info));
    ESP_ERROR_CHECK(console_register(&s_sys_log));
    ESP_ERROR_CHECK(console_register(&s_sys_reboot));
    ESP_ERROR_CHECK(console_register(&s_evt_since));
}
