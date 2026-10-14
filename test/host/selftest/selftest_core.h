#pragma once
/* Trivial pure-logic module that exists only to prove the host harness works.
 * Real *_core modules live in components/<comp>/ and are tested the same way. */

#include <stddef.h>
#include <stdint.h>

/* Sum of all bytes modulo 256. */
uint8_t selftest_sum8(const uint8_t *data, size_t len);

/* Elapsed milliseconds from `since` to `now`; 0 if `now` is before `since`. */
uint64_t selftest_elapsed_ms(uint64_t now_ms, uint64_t since_ms);
