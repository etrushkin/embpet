#include "selftest_core.h"

uint8_t selftest_sum8(const uint8_t *data, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + data[i]);
    }
    return sum;
}

uint64_t selftest_elapsed_ms(uint64_t now_ms, uint64_t since_ms)
{
    return now_ms >= since_ms ? now_ms - since_ms : 0;
}
