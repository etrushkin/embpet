/* io: GPIO inputs and outputs, pulses, debounce and the status LED. */
#ifndef EMBPET_IO_H
#define EMBPET_IO_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_attr.h"
#include "esp_err.h"

#define IO_OUT_COUNT 8                  /* OUT0..OUT7 on GPIO4,3,2,1,0,5,6,7 (README 1.2) */

/* GPIO number of logical output n (0..IO_OUT_COUNT-1, the caller validates); the one place
 * the wiring is defined. ISR callbacks read it with the flash cache off, so the table is in
 * DRAM (io_out.c) and the accessor is always inlined, also at -O0. */
extern const uint8_t io_out_gpio_tab[IO_OUT_COUNT];
FORCE_INLINE_ATTR int io_out_gpio(int n)
{
    return io_out_gpio_tab[n];
}

typedef struct {
    bool on;                            /* logical level: on = opto conducting = pin low */
    bool pulsing;
    uint32_t remaining_ms;              /* 0 when not pulsing */
} io_out_state_t;

/* PLAN 1.2 step 1, before NVS/cfg: for each output's GPIO write the latch high (off), then
 * enable the pin as output, so there is no glitch. Boot levels follow in io_outputs_apply_boot().
 * Creates the lock and the per-pin pulse timers (aborts if that fails). */
void io_outputs_init(void);

/* After cfg_init(): apply the stored per-pin boot levels (cfg_get_out_boot). */
void io_outputs_apply_boot(void);

/* Output control for pin 0..7 (callers validate the pin). Safe from any task; not from an
 * ISR. A set or toggle cancels a running pulse. Each returns the resulting state. */
void io_out_set(int pin, bool on, io_out_state_t *st);
void io_out_toggle(int pin, io_out_state_t *st);
/* Invert the level for ms (1..CONFIG_EMBPET_PULSE_MAX_MS), then restore it. If the end
 * cannot be scheduled, returns the esp_timer error, cancels the pulse and restores the
 * pre-pulse level. */
esp_err_t io_out_pulse(int pin, uint32_t ms, io_out_state_t *st);
void io_out_get(int pin, io_out_state_t *st);

#define IO_IN_COUNT 3                   /* inputs on GPIO10, GPIO20, GPIO21 */

typedef struct {
    int pin;
    bool active;                        /* debounced: opto energised = pin low */
    int level;                          /* raw pin level now */
    uint32_t extra;                     /* raw edges since the last `input` event */
} io_in_state_t;

/* Configure the inputs (pull-up, any-edge ISR, one debounce timer each) and start
 * reporting `input` events. Needs cfg_init() and proto_init() to have run. */
void io_inputs_init(void);

/* Index 0..IO_IN_COUNT-1 of an input GPIO number (10, 20, 21), or -1. */
int io_in_index(int pin);
void io_in_get(int idx, io_in_state_t *st);
uint32_t io_in_debounce_ms(int idx);
/* Store (NVS) and apply a new debounce time, 0..1000 ms. */
esp_err_t io_in_set_debounce(int idx, int ms);

/* Register the io console commands (io_cmd.c). Call before console_start(). */
void io_register_cmds(void);

#endif /* EMBPET_IO_H */
