#include "led_core.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static const led_inputs_t ONLINE = {.configured = true, .wifi_up = true, .tg_online = true};

/* Sample every ms over [from, from+len) and return the on-time; fills the number of
 * off->on transitions. */
static unsigned sample(const led_inputs_t *in, uint64_t from, unsigned len, unsigned *rises)
{
    unsigned on = 0;
    unsigned r = 0;
    bool prev = led_core_level(in, from - 1);
    for (uint64_t t = from; t < from + len; t++) {
        bool l = led_core_level(in, t);
        on += l;
        r += (l && !prev);
        prev = l;
    }
    if (rises != NULL) {
        *rises = r;
    }
    return on;
}

static void test_booting_solid_on_and_wins_everything(void)
{
    led_inputs_t in = {.booting = true, .pending = 3};
    unsigned rises;
    TEST_ASSERT_EQUAL_UINT(4000, sample(&in, 12345, 4000, &rises));
    TEST_ASSERT_EQUAL_UINT(0, rises);
    in.configured = true;
    in.wifi_up = true;
    in.tg_online = true;
    TEST_ASSERT_EQUAL_UINT(4000, sample(&in, 0, 4000, NULL));
}

static void test_blink_1hz_50_percent(void)
{
    led_inputs_t cases[4];
    for (int i = 0; i < 4; i++) {
        cases[i] = ONLINE;
    }
    cases[0].configured = false;
    cases[1].wifi_up = false;
    cases[2].tg_online = false;
    cases[3].configured = false;
    cases[3].wifi_up = false;
    cases[3].tg_online = false;
    for (int i = 0; i < 4; i++) {
        unsigned rises;
        cases[i].pending = (unsigned)i;     /* pending is hidden while offline */
        TEST_ASSERT_EQUAL_UINT(2000, sample(&cases[i], 777, 4000, &rises));
        TEST_ASSERT_EQUAL_UINT(4, rises);
        TEST_ASSERT_TRUE(led_core_level(&cases[i], 0));
        TEST_ASSERT_TRUE(led_core_level(&cases[i], 499));
        TEST_ASSERT_FALSE(led_core_level(&cases[i], 500));
        TEST_ASSERT_FALSE(led_core_level(&cases[i], 999));
        TEST_ASSERT_TRUE(led_core_level(&cases[i], 1000));
    }
}

static void test_pending_burst_then_three_seconds_off(void)
{
    led_inputs_t in = ONLINE;
    in.pending = 1;
    unsigned rises;
    TEST_ASSERT_EQUAL_UINT(500, sample(&in, 0, 4000, &rises));      /* 5 x 100 ms */
    TEST_ASSERT_EQUAL_UINT(5, rises);
    for (uint64_t t = 0; t < 1000; t++) {
        TEST_ASSERT_EQUAL((t % 200) < 100, led_core_level(&in, t));
    }
    for (uint64_t t = 1000; t < 4000; t++) {
        TEST_ASSERT_FALSE(led_core_level(&in, t));
    }
    /* the cycle repeats and is the same for any count */
    in.pending = 64;
    TEST_ASSERT_EQUAL_UINT(500, sample(&in, 8000, 4000, &rises));
    TEST_ASSERT_EQUAL_UINT(5, rises);
    TEST_ASSERT_TRUE(led_core_level(&in, 12000));
    TEST_ASSERT_FALSE(led_core_level(&in, 12100));
}

static void test_online_nothing_pending_is_off(void)
{
    TEST_ASSERT_EQUAL_UINT(0, sample(&ONLINE, 0, 8000, NULL));
}

static void test_priority_order(void)
{
    led_inputs_t in = {.booting = true, .configured = false, .pending = 2};
    TEST_ASSERT_TRUE(led_core_level(&in, 700));         /* booting beats blink */
    in.booting = false;
    TEST_ASSERT_FALSE(led_core_level(&in, 700));        /* blink (off phase) beats pending */
    TEST_ASSERT_TRUE(led_core_level(&in, 100));         /* blink on phase; pending is off at 100 */
    in.configured = true;
    in.wifi_up = true;
    in.tg_online = true;
    TEST_ASSERT_FALSE(led_core_level(&in, 100));        /* pending burst off phase at 100 */
    TEST_ASSERT_TRUE(led_core_level(&in, 50));          /* pending on phase */
    in.pending = 0;
    TEST_ASSERT_FALSE(led_core_level(&in, 50));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_booting_solid_on_and_wins_everything);
    RUN_TEST(test_blink_1hz_50_percent);
    RUN_TEST(test_pending_burst_then_three_seconds_off);
    RUN_TEST(test_online_nothing_pending_is_off);
    RUN_TEST(test_priority_order);
    return UNITY_END();
}
