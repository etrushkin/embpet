#include <string.h>

#include "cJSON.h"
#include "selftest_core.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_sum8_adds_bytes(void)
{
    const uint8_t d[] = {1, 2, 3};
    TEST_ASSERT_EQUAL_UINT8(6, selftest_sum8(d, sizeof d));
}

static void test_sum8_wraps_at_256(void)
{
    const uint8_t d[] = {200, 100};
    TEST_ASSERT_EQUAL_UINT8(44, selftest_sum8(d, sizeof d));
}

static void test_sum8_of_empty_input_is_zero(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, selftest_sum8(NULL, 0));
}

static void test_elapsed_ms_returns_difference(void)
{
    TEST_ASSERT_EQUAL_UINT64(250, selftest_elapsed_ms(1250, 1000));
}

static void test_elapsed_ms_clamps_to_zero_when_time_goes_backwards(void)
{
    TEST_ASSERT_EQUAL_UINT64(0, selftest_elapsed_ms(10, 20));
}

static void test_cjson_round_trips_a_frame(void)
{
    cJSON *obj = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(obj);
    cJSON_AddStringToObject(obj, "t", "evt");
    cJSON_AddNumberToObject(obj, "seq", 24);

    char *text = cJSON_PrintUnformatted(obj);
    TEST_ASSERT_NOT_NULL(text);
    cJSON_Delete(obj);

    cJSON *parsed = cJSON_Parse(text);
    TEST_ASSERT_NOT_NULL(parsed);
    const cJSON *t = cJSON_GetObjectItem(parsed, "t");
    const cJSON *seq = cJSON_GetObjectItem(parsed, "seq");
    TEST_ASSERT_TRUE(cJSON_IsString(t));
    TEST_ASSERT_TRUE(cJSON_IsNumber(seq));
    TEST_ASSERT_EQUAL_STRING("evt", t->valuestring);
    TEST_ASSERT_EQUAL_DOUBLE(24.0, seq->valuedouble);

    cJSON_Delete(parsed);
    cJSON_free(text);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sum8_adds_bytes);
    RUN_TEST(test_sum8_wraps_at_256);
    RUN_TEST(test_sum8_of_empty_input_is_zero);
    RUN_TEST(test_elapsed_ms_returns_difference);
    RUN_TEST(test_elapsed_ms_clamps_to_zero_when_time_goes_backwards);
    RUN_TEST(test_cjson_round_trips_a_frame);
    return UNITY_END();
}
