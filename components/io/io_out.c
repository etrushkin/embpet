/* Outputs OUT0..OUT7 on GPIO0..7, sink wiring: logical on = pin low (README 1.3).
 * One lock guards the per-pin pulse state and the pin writes; the esp_timer callback that
 * ends a pulse takes it too. A callback that fires late (pulse cancelled or restarted) is
 * harmless: pulse_expire() checks the deadline. */
#include "io.h"

#include <stdint.h>

#include "cfg.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "pulse_core.h"
#include "sdkconfig.h"

static pulse_t s_pin[IO_OUT_COUNT];
static esp_timer_handle_t s_timer[IO_OUT_COUNT];
static SemaphoreHandle_t s_lock;

static uint64_t now_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000u;
}

static void write_pin(int pin)
{
    gpio_set_level((gpio_num_t)pin, s_pin[pin].on ? 0 : 1);
}

static void fill(int pin, io_out_state_t *st)
{
    st->on = s_pin[pin].on;
    st->pulsing = s_pin[pin].pulsing;
    st->remaining_ms = pulse_remaining_ms(&s_pin[pin], now_ms());
}

static void timer_stop(int pin)
{
    esp_timer_stop(s_timer[pin]);                   /* INVALID_STATE when not armed: fine */
}

static void pulse_end_cb(void *arg)
{
    int pin = (int)(intptr_t)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (pulse_expire(&s_pin[pin], now_ms())) {
        write_pin(pin);
    }
    xSemaphoreGive(s_lock);
}

void io_outputs_init(void)
{
    for (int pin = 0; pin < IO_OUT_COUNT; pin++) {
        gpio_set_level((gpio_num_t)pin, 1);         /* latch off first ... */
        const gpio_config_t c = {
            .pin_bit_mask = 1ULL << pin,
#ifdef CONFIG_EMBPET_IO_TEST
            .mode = GPIO_MODE_INPUT_OUTPUT,         /* readback for the on-chip pulse check */
#else
            .mode = GPIO_MODE_OUTPUT,               /* ... then enable the driver */
#endif
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&c));
        pulse_init(&s_pin[pin], false);
    }
    s_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    for (int pin = 0; pin < IO_OUT_COUNT; pin++) {
        const esp_timer_create_args_t a = {
            .callback = pulse_end_cb,
            .arg = (void *)(intptr_t)pin,
            .name = "pulse",
        };
        ESP_ERROR_CHECK(esp_timer_create(&a, &s_timer[pin]));
    }
}

void io_outputs_apply_boot(void)
{
    uint8_t mask = cfg_get_out_boot();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int pin = 0; pin < IO_OUT_COUNT; pin++) {
        pulse_set(&s_pin[pin], (mask >> pin) & 1u);
        write_pin(pin);
    }
    xSemaphoreGive(s_lock);
}

void io_out_set(int pin, bool on, io_out_state_t *st)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    timer_stop(pin);
    pulse_set(&s_pin[pin], on);
    write_pin(pin);
    fill(pin, st);
    xSemaphoreGive(s_lock);
}

void io_out_toggle(int pin, io_out_state_t *st)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    timer_stop(pin);
    pulse_toggle(&s_pin[pin]);
    write_pin(pin);
    fill(pin, st);
    xSemaphoreGive(s_lock);
}

esp_err_t io_out_pulse(int pin, uint32_t ms, io_out_state_t *st)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    timer_stop(pin);
    /* Edge first, then the timer, so the pulse is ms long from the edge (the lock keeps
     * the end callback out until we are done). The deadline in ms is taken before the
     * timer starts, so the callback never sees it as not yet reached. */
    pulse_start(&s_pin[pin], ms, now_ms());
    write_pin(pin);
    esp_err_t err = esp_timer_start_once(s_timer[pin], (uint64_t)ms * 1000u);
    if (err != ESP_OK) {
        pulse_set(&s_pin[pin], s_pin[pin].restore_on);   /* cannot time it: back out */
        write_pin(pin);
    }
    fill(pin, st);
    xSemaphoreGive(s_lock);
    return err;
}

void io_out_get(int pin, io_out_state_t *st)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    fill(pin, st);
    xSemaphoreGive(s_lock);
}
