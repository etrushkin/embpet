#include "pulse_core.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_init_and_set(void)
{
    pulse_t p;
    pulse_init(&p, false);
    TEST_ASSERT_FALSE(p.on);
    TEST_ASSERT_FALSE(p.pulsing);
    pulse_set(&p, true);
    TEST_ASSERT_TRUE(p.on);
    TEST_ASSERT_EQUAL_UINT32(0, pulse_remaining_ms(&p, 5));
}

static void test_pulse_from_off(void)
{
    pulse_t p;
    pulse_init(&p, false);
    pulse_start(&p, 100, 1000);
    TEST_ASSERT_TRUE(p.on);
    TEST_ASSERT_TRUE(p.pulsing);
    TEST_ASSERT_EQUAL_UINT32(100, pulse_remaining_ms(&p, 1000));
    TEST_ASSERT_EQUAL_UINT32(40, pulse_remaining_ms(&p, 1060));
    TEST_ASSERT_FALSE(pulse_expire(&p, 1099));
    TEST_ASSERT_TRUE(p.on);
    TEST_ASSERT_TRUE(pulse_expire(&p, 1100));
    TEST_ASSERT_FALSE(p.on);
    TEST_ASSERT_FALSE(p.pulsing);
    TEST_ASSERT_EQUAL_UINT32(0, pulse_remaining_ms(&p, 1100));
}

static void test_pulse_from_on_goes_off_then_back(void)
{
    pulse_t p;
    pulse_init(&p, true);
    pulse_start(&p, 50, 0);
    TEST_ASSERT_FALSE(p.on);
    TEST_ASSERT_TRUE(pulse_expire(&p, 50));
    TEST_ASSERT_TRUE(p.on);
}

static void test_expire_when_idle_is_noop(void)
{
    pulse_t p;
    pulse_init(&p, true);
    TEST_ASSERT_FALSE(pulse_expire(&p, 1u << 30));
    TEST_ASSERT_TRUE(p.on);
}

static void test_repulse_restarts_and_keeps_original_level(void)
{
    pulse_t p;
    pulse_init(&p, false);
    pulse_start(&p, 100, 0);
    pulse_start(&p, 100, 80);                     /* restart: ends at 180, not 100 */
    TEST_ASSERT_TRUE(p.on);                       /* still the pulse level */
    TEST_ASSERT_EQUAL_UINT32(100, pulse_remaining_ms(&p, 80));
    TEST_ASSERT_FALSE(pulse_expire(&p, 100));     /* stale timer for the first deadline */
    TEST_ASSERT_TRUE(p.on);
    TEST_ASSERT_TRUE(pulse_expire(&p, 180));
    TEST_ASSERT_FALSE(p.on);                      /* original level, not inverted twice */
}

static void test_repulse_can_change_length(void)
{
    pulse_t p;
    pulse_init(&p, false);
    pulse_start(&p, 1000, 0);
    pulse_start(&p, 10, 5);                       /* shorter: ends at 15 */
    TEST_ASSERT_EQUAL_UINT32(10, pulse_remaining_ms(&p, 5));
    TEST_ASSERT_TRUE(pulse_expire(&p, 15));
    TEST_ASSERT_FALSE(p.on);
}

static void test_set_during_pulse_cancels(void)
{
    pulse_t p;
    pulse_init(&p, false);
    pulse_start(&p, 100, 0);
    pulse_set(&p, false);                         /* explicit level wins, no restore */
    TEST_ASSERT_FALSE(p.on);
    TEST_ASSERT_FALSE(p.pulsing);
    TEST_ASSERT_FALSE(pulse_expire(&p, 100));     /* stale timer */
    TEST_ASSERT_FALSE(p.on);

    pulse_start(&p, 100, 200);
    pulse_set(&p, true);                          /* same as pulse level: stays on for good */
    TEST_ASSERT_FALSE(pulse_expire(&p, 300));
    TEST_ASSERT_TRUE(p.on);
}

static void test_toggle_during_pulse_cancels(void)
{
    pulse_t p;
    pulse_init(&p, false);
    pulse_start(&p, 100, 0);                      /* on (pulse level) */
    TEST_ASSERT_FALSE(pulse_toggle(&p));          /* inverts the current level */
    TEST_ASSERT_FALSE(p.pulsing);
    TEST_ASSERT_FALSE(pulse_expire(&p, 100));
    TEST_ASSERT_FALSE(p.on);
    TEST_ASSERT_TRUE(pulse_toggle(&p));
    TEST_ASSERT_TRUE(p.on);
}

static void test_pulse_after_cancel_uses_new_level(void)
{
    pulse_t p;
    pulse_init(&p, false);
    pulse_start(&p, 100, 0);
    pulse_set(&p, true);
    pulse_start(&p, 20, 10);                      /* now inverts the new level */
    TEST_ASSERT_FALSE(p.on);
    TEST_ASSERT_TRUE(pulse_expire(&p, 30));
    TEST_ASSERT_TRUE(p.on);
}

static void test_repulse_after_deadline_before_expire(void)
{
    pulse_t p;
    pulse_init(&p, false);
    pulse_start(&p, 100, 0);
    pulse_start(&p, 100, 100);                    /* deadline passed, timer callback not run yet */
    TEST_ASSERT_TRUE(p.on);
    TEST_ASSERT_FALSE(pulse_expire(&p, 100));     /* the late callback must not end it */
    TEST_ASSERT_EQUAL_UINT32(100, pulse_remaining_ms(&p, 100));
    TEST_ASSERT_TRUE(pulse_expire(&p, 200));
    TEST_ASSERT_FALSE(p.on);                      /* original level restored */
}

static void test_long_deadline_no_overflow(void)
{
    pulse_t p;
    pulse_init(&p, false);
    pulse_start(&p, 86400000u, 4000000000ull);    /* past 32-bit ms */
    TEST_ASSERT_EQUAL_UINT32(86400000u, pulse_remaining_ms(&p, 4000000000ull));
    TEST_ASSERT_FALSE(pulse_expire(&p, 4086399999ull));
    TEST_ASSERT_TRUE(pulse_expire(&p, 4086400000ull));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_and_set);
    RUN_TEST(test_pulse_from_off);
    RUN_TEST(test_pulse_from_on_goes_off_then_back);
    RUN_TEST(test_expire_when_idle_is_noop);
    RUN_TEST(test_repulse_restarts_and_keeps_original_level);
    RUN_TEST(test_repulse_can_change_length);
    RUN_TEST(test_set_during_pulse_cancels);
    RUN_TEST(test_toggle_during_pulse_cancels);
    RUN_TEST(test_pulse_after_cancel_uses_new_level);
    RUN_TEST(test_repulse_after_deadline_before_expire);
    RUN_TEST(test_long_deadline_no_overflow);
    return UNITY_END();
}
