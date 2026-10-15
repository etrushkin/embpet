#include "led.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "led_core.h"
#include "led_priv.h"
#include "sdkconfig.h"
#include "state.h"

#define LED_TICK_US 50000

#ifdef CONFIG_EMBPET_LED_ACTIVE_LOW
#define LED_PIN_LEVEL(lit) ((lit) ? 0 : 1)
#else
#define LED_PIN_LEVEL(lit) ((lit) ? 1 : 0)
#endif

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static led_inputs_t s_in;
static uint32_t s_gen;                  /* newest state gen handled (state.h ordering rule) */

static void led_apply(void)
{
    led_inputs_t in;
    taskENTER_CRITICAL(&s_mux);
    in = s_in;
    taskEXIT_CRITICAL(&s_mux);
    bool lit = led_core_level(&in, (uint64_t)(esp_timer_get_time() / 1000));
    gpio_set_level((gpio_num_t)CONFIG_EMBPET_LED_GPIO, LED_PIN_LEVEL(lit));
}

static void on_state(const state_snapshot_t *snap, uint32_t changed, void *ctx)
{
    (void)changed;
    (void)ctx;
    taskENTER_CRITICAL(&s_mux);
    if ((int32_t)(snap->gen - s_gen) > 0) {
        s_gen = snap->gen;
        s_in.booting = snap->booting;
        s_in.configured = snap->configured;
        s_in.wifi_up = snap->wifi.state == STATE_WIFI_UP;
        s_in.tg_online = snap->tg.state == STATE_TG_ONLINE;
        s_in.pending = snap->pending;
    }
    taskEXIT_CRITICAL(&s_mux);
}

static void tick_cb(void *arg)
{
    (void)arg;
    led_apply();
}

void led_init(void)
{
    const gpio_config_t c = {
        .pin_bit_mask = 1ULL << CONFIG_EMBPET_LED_GPIO,
#ifdef CONFIG_EMBPET_LED_TEST
        .mode = GPIO_MODE_INPUT_OUTPUT,         /* readback: the on-chip LED check */
#else
        .mode = GPIO_MODE_OUTPUT,
#endif
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    /* Subscribe first and take the snapshot under the same gen rule, so no change is lost. */
    ESP_ERROR_CHECK(state_subscribe(on_state, NULL) ? ESP_OK : ESP_ERR_NO_MEM);
    state_snapshot_t snap;
    state_get(&snap);
    s_gen = snap.gen - 1u;
    on_state(&snap, 0, NULL);
    led_apply();                                /* pre-loads the latch: no glitch when the driver enables */
    ESP_ERROR_CHECK(gpio_config(&c));
    led_apply();

    static esp_timer_handle_t timer;
    const esp_timer_create_args_t a = {.callback = tick_cb, .name = "led"};
    ESP_ERROR_CHECK(esp_timer_create(&a, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, LED_TICK_US));
#ifdef CONFIG_EMBPET_LED_TEST
    led_test_register();
#endif
}
