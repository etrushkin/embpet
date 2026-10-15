#include "unity.h"
#include "wifi_core.h"

void setUp(void) {}
void tearDown(void) {}

static void test_backoff_schedule(void)
{
    const uint32_t want[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000};
    for (unsigned k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
        TEST_ASSERT_EQUAL_UINT32(want[k], wifi_backoff_ms(k));
    }
    TEST_ASSERT_EQUAL_UINT32(30000, wifi_backoff_ms(100));
    TEST_ASSERT_EQUAL_UINT32(30000, wifi_backoff_ms(0xffffffffu));
}

static void check_ok(const char *s, int want)
{
    int q = -1;
    TEST_ASSERT_TRUE_MESSAGE(wifi_parse_txpower(s, &q), s);
    TEST_ASSERT_EQUAL_INT_MESSAGE(want, q, s);
}

static void check_bad(const char *s)
{
    int q = 1234;
    TEST_ASSERT_FALSE_MESSAGE(wifi_parse_txpower(s, &q), s);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1234, q, "output untouched on failure");
}

static void test_txpower_valid(void)
{
    check_ok("2", 8);
    check_ok("2.0", 8);
    check_ok("8.5", 34);
    check_ok("8.50", 34);
    check_ok("8.25", 33);
    check_ok("8.75", 35);
    check_ok("19.5", 78);
    check_ok("20", 80);
    check_ok("20.00", 80);
    check_ok("02", 8);
}

static void test_txpower_invalid(void)
{
    check_bad(NULL);
    check_bad("");
    check_bad("1.75");     /* below 2 dBm */
    check_bad("20.25");    /* above 20 dBm */
    check_bad("21");
    check_bad("8.3");      /* not a 0.25 step */
    check_bad("8.05");
    check_bad("8.");
    check_bad(".5");
    check_bad("-8");
    check_bad("+8");
    check_bad("8.5.1");
    check_bad("8.125");
    check_bad("8 ");
    check_bad(" 8");
    check_bad("1e1");
    check_bad("0x10");
    check_bad("abc");
    check_bad("1000");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_backoff_schedule);
    RUN_TEST(test_txpower_valid);
    RUN_TEST(test_txpower_invalid);
    return UNITY_END();
}
