/* Console commands: io set, io pulse, io get, io boot (README 2.4, 3.4). */
#include <stdio.h>
#include <string.h>

#include "argtable3/argtable3.h"
#include "cfg.h"
#include "console.h"
#include "io.h"
#include "sdkconfig.h"

#ifdef CONFIG_EMBPET_IO_TEST
#include "io_priv.h"
#endif

static const char *level_str(bool on)
{
    return on ? "on" : "off";
}

/* Pin of an output command, or false after answering bad_pin. */
static bool out_pin(console_req_t *req, int pin)
{
    if (pin < 0 || pin >= IO_OUT_COUNT) {
        char msg[48];
        snprintf(msg, sizeof(msg), "pin %d is not an output", pin);
        console_err(req, CONSOLE_ERR_BAD_PIN, msg);
        return false;
    }
    return true;
}

static void oom(console_req_t *req)
{
    console_err(req, CONSOLE_ERR_INTERNAL, "out of memory");
}

/* ---- io set <pin> on|off|toggle ---- */

static struct arg_int *s_set_pin;
static struct arg_str *s_set_level;
static void *s_set_args[3];

static void io_set(console_req_t *req)
{
    int pin = s_set_pin->ival[0];
    const char *what = s_set_level->sval[0];
    if (!out_pin(req, pin)) {
        return;
    }
    io_out_state_t st;
    if (strcmp(what, "on") == 0) {
        io_out_set(pin, true, &st);
    } else if (strcmp(what, "off") == 0) {
        io_out_set(pin, false, &st);
    } else if (strcmp(what, "toggle") == 0) {
        io_out_toggle(pin, &st);
    } else {
        console_err(req, CONSOLE_ERR_BAD_ARGS, "level must be on, off or toggle");
        return;
    }
    cJSON *p = cJSON_CreateObject();
    if (p == NULL) {
        oom(req);
        return;
    }
    cJSON_AddNumberToObject(p, "pin", pin);
    cJSON_AddStringToObject(p, "level", level_str(st.on));
    console_ok(req, p);
}

static const console_cmd_t s_io_set = {
    .name = "io set",
    .help = "Set an output (pin 0-7) on, off or toggle; cancels a pulse",
    .args = s_set_args,
    .handler = io_set,
};

/* ---- io pulse <pin> [-d ms] ---- */

static struct arg_int *s_pulse_pin;
static struct arg_int *s_pulse_ms;
static void *s_pulse_args[3];

static void io_pulse(console_req_t *req)
{
    int pin = s_pulse_pin->ival[0];
    int ms = s_pulse_ms->count > 0 ? s_pulse_ms->ival[0] : CONFIG_EMBPET_PULSE_DEFAULT_MS;
    if (!out_pin(req, pin)) {
        return;
    }
    if (ms < 1 || ms > CONFIG_EMBPET_PULSE_MAX_MS) {
        char msg[48];
        snprintf(msg, sizeof(msg), "-d must be 1..%d ms", CONFIG_EMBPET_PULSE_MAX_MS);
        console_err(req, CONSOLE_ERR_BAD_ARGS, msg);
        return;
    }
    io_out_state_t st;
    if (io_out_pulse(pin, (uint32_t)ms, &st) != ESP_OK) {
        console_err(req, CONSOLE_ERR_INTERNAL, "cannot start pulse timer");
        return;
    }
    cJSON *p = cJSON_CreateObject();
    if (p == NULL) {
        oom(req);
        return;
    }
    cJSON_AddNumberToObject(p, "pin", pin);
    cJSON_AddStringToObject(p, "level", level_str(st.on));
    cJSON_AddNumberToObject(p, "remaining_ms", (double)st.remaining_ms);
    console_ok(req, p);
}

static const console_cmd_t s_io_pulse = {
    .name = "io pulse",
    .help = "Invert an output for -d ms (default 100), then restore it",
    .args = s_pulse_args,
    .handler = io_pulse,
};

/* ---- io get ---- */

static void io_get(console_req_t *req)
{
    cJSON *out = cJSON_CreateArray();
    cJSON *p = cJSON_CreateObject();
    if (out == NULL || p == NULL) {
        cJSON_Delete(out);
        cJSON_Delete(p);
        oom(req);
        return;
    }
    for (int pin = 0; pin < IO_OUT_COUNT; pin++) {
        io_out_state_t st;
        io_out_get(pin, &st);
        cJSON *o = cJSON_CreateObject();
        if (o == NULL) {
            cJSON_Delete(out);
            cJSON_Delete(p);
            oom(req);
            return;
        }
        cJSON_AddNumberToObject(o, "pin", pin);
        cJSON_AddStringToObject(o, "level", level_str(st.on));
        cJSON_AddBoolToObject(o, "pulsing", st.pulsing);
        cJSON_AddNumberToObject(o, "remaining_ms", (double)st.remaining_ms);
        cJSON_AddItemToArray(out, o);
    }
    cJSON_AddItemToObject(p, "out", out);
    console_ok(req, p);
}

static const console_cmd_t s_io_get = {
    .name = "io get",
    .help = "Output levels, pulsing flag and remaining pulse time",
    .handler = io_get,
};

/* ---- io boot [<pin> on|off] ---- */

static struct arg_int *s_boot_pin;
static struct arg_str *s_boot_level;
static void *s_boot_args[3];

static void boot_reply(console_req_t *req)
{
    uint8_t mask = cfg_get_out_boot();
    cJSON *b = cJSON_CreateObject();
    cJSON *p = cJSON_CreateObject();
    if (b == NULL || p == NULL) {
        cJSON_Delete(b);
        cJSON_Delete(p);
        oom(req);
        return;
    }
    for (int pin = 0; pin < IO_OUT_COUNT; pin++) {
        char key[4];
        snprintf(key, sizeof(key), "%d", pin);
        cJSON_AddStringToObject(b, key, level_str((mask >> pin) & 1u));
    }
    /* Not "boot": that key is the frame's boot id and is reserved (proto.h). */
    cJSON_AddItemToObject(p, "levels", b);
    console_ok(req, p);
}

static void io_boot(console_req_t *req)
{
    if (s_boot_pin->count == 0) {
        if (s_boot_level->count > 0) {
            console_err(req, CONSOLE_ERR_BAD_ARGS, "usage: io boot [<pin> on|off]");
            return;
        }
        boot_reply(req);
        return;
    }
    int pin = s_boot_pin->ival[0];
    if (!out_pin(req, pin)) {
        return;
    }
    if (s_boot_level->count == 0) {
        console_err(req, CONSOLE_ERR_BAD_ARGS, "usage: io boot [<pin> on|off]");
        return;
    }
    const char *what = s_boot_level->sval[0];
    bool on;
    if (strcmp(what, "on") == 0) {
        on = true;
    } else if (strcmp(what, "off") == 0) {
        on = false;
    } else {
        console_err(req, CONSOLE_ERR_BAD_ARGS, "level must be on or off");
        return;
    }
    uint8_t mask = cfg_get_out_boot();
    mask = (uint8_t)(on ? (mask | (1u << pin)) : (mask & ~(1u << pin)));
    if (cfg_set_out_boot(mask) != ESP_OK) {
        console_err(req, CONSOLE_ERR_INTERNAL, "cannot store boot level");
        return;
    }
    boot_reply(req);
}

static const console_cmd_t s_io_boot = {
    .name = "io boot",
    .help = "Show, or set for one output (pin 0-7), the level taken at app start",
    .args = s_boot_args,
    .handler = io_boot,
};

/* ---- io in ---- */

static void io_in(console_req_t *req)
{
    cJSON *arr = cJSON_CreateArray();
    cJSON *p = cJSON_CreateObject();
    if (arr == NULL || p == NULL) {
        cJSON_Delete(arr);
        cJSON_Delete(p);
        oom(req);
        return;
    }
    for (int k = 0; k < IO_IN_COUNT; k++) {
        io_in_state_t st;
        io_in_get(k, &st);
        cJSON *o = cJSON_CreateObject();
        if (o == NULL) {
            cJSON_Delete(arr);
            cJSON_Delete(p);
            oom(req);
            return;
        }
        cJSON_AddNumberToObject(o, "pin", st.pin);
        cJSON_AddNumberToObject(o, "active", st.active ? 1 : 0);
        cJSON_AddNumberToObject(o, "level", st.level);
        cJSON_AddNumberToObject(o, "extra", (double)st.extra);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddItemToObject(p, "in", arr);
    console_ok(req, p);
}

static const console_cmd_t s_io_in = {
    .name = "io in",
    .help = "Inputs: debounced active flag, raw level, edges since the last event",
    .handler = io_in,
};

/* ---- io debounce [<pin> <ms>] ---- */

static struct arg_int *s_deb_pin;
static struct arg_int *s_deb_ms;
static void *s_deb_args[3];

static void deb_reply(console_req_t *req)
{
    cJSON *d = cJSON_CreateObject();
    cJSON *p = cJSON_CreateObject();
    if (d == NULL || p == NULL) {
        cJSON_Delete(d);
        cJSON_Delete(p);
        oom(req);
        return;
    }
    for (int k = 0; k < IO_IN_COUNT; k++) {
        io_in_state_t st;
        io_in_get(k, &st);
        char key[4];
        snprintf(key, sizeof(key), "%d", st.pin);
        cJSON_AddNumberToObject(d, key, (double)io_in_debounce_ms(k));
    }
    cJSON_AddItemToObject(p, "debounce", d);
    console_ok(req, p);
}

static void io_debounce(console_req_t *req)
{
    if (s_deb_pin->count == 0) {
        deb_reply(req);
        return;
    }
    int pin = s_deb_pin->ival[0];
    int idx = io_in_index(pin);
    if (idx < 0) {
        char msg[48];
        snprintf(msg, sizeof(msg), "pin %d is not an input", pin);
        console_err(req, CONSOLE_ERR_BAD_PIN, msg);
        return;
    }
    if (s_deb_ms->count == 0) {
        console_err(req, CONSOLE_ERR_BAD_ARGS, "usage: io debounce [<pin> <ms>]");
        return;
    }
    int ms = s_deb_ms->ival[0];
    if (ms < 0 || ms > 1000) {
        console_err(req, CONSOLE_ERR_BAD_ARGS, "debounce must be 0..1000 ms");
        return;
    }
    if (io_in_set_debounce(idx, ms) != ESP_OK) {
        console_err(req, CONSOLE_ERR_INTERNAL, "cannot store debounce time");
        return;
    }
    deb_reply(req);
}

static const console_cmd_t s_io_debounce = {
    .name = "io debounce",
    .help = "Show, or set for one input (pin 10, 20, 21), the debounce time in ms (0-1000)",
    .args = s_deb_args,
    .handler = io_debounce,
};

void io_register_cmds(void)
{
    s_set_pin = arg_int1(NULL, NULL, "<pin>", "output 0-7");
    s_set_level = arg_str1(NULL, NULL, "<on|off|toggle>", "logical level");
    s_pulse_pin = arg_int1(NULL, NULL, "<pin>", "output 0-7");
    s_pulse_ms = arg_int0("d", NULL, "<ms>", "pulse length in ms (default 100)");
    s_boot_pin = arg_int0(NULL, NULL, "<pin>", "output 0-7");
    s_boot_level = arg_str0(NULL, NULL, "<on|off>", "level at app start");
    s_deb_pin = arg_int0(NULL, NULL, "<pin>", "input 10, 20 or 21");
    s_deb_ms = arg_int0(NULL, NULL, "<ms>", "0-1000");
    if (s_set_pin == NULL || s_set_level == NULL || s_pulse_pin == NULL ||
        s_pulse_ms == NULL || s_boot_pin == NULL || s_boot_level == NULL ||
        s_deb_pin == NULL || s_deb_ms == NULL) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    s_set_args[0] = s_set_pin;
    s_set_args[1] = s_set_level;
    s_pulse_args[0] = s_pulse_pin;
    s_pulse_args[1] = s_pulse_ms;
    s_boot_args[0] = s_boot_pin;
    s_boot_args[1] = s_boot_level;
    s_deb_args[0] = s_deb_pin;
    s_deb_args[1] = s_deb_ms;

    ESP_ERROR_CHECK(console_register(&s_io_set));
    ESP_ERROR_CHECK(console_register(&s_io_pulse));
    ESP_ERROR_CHECK(console_register(&s_io_get));
    ESP_ERROR_CHECK(console_register(&s_io_boot));
    ESP_ERROR_CHECK(console_register(&s_io_in));
    ESP_ERROR_CHECK(console_register(&s_io_debounce));
#ifdef CONFIG_EMBPET_IO_TEST
    io_test_register();
#endif
}
