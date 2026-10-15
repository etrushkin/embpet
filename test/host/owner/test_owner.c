#include <string.h>

#include "owner_core.h"
#include "unity.h"

#define BOT "ugenes_bot"
/* every pair_check below judges as this bot */
#define pair_check(p, text, now) pair_check((p), (text), BOT, (now))

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------ code generation */

static void test_code_make(void)
{
    char c[PAIR_CODE_BUF];
    TEST_ASSERT_TRUE(pair_code_make(0, c));
    TEST_ASSERT_EQUAL_STRING("000000", c);
    TEST_ASSERT_TRUE(pair_code_make(42, c));
    TEST_ASSERT_EQUAL_STRING("000042", c);
    TEST_ASSERT_TRUE(pair_code_make(1234567, c));
    TEST_ASSERT_EQUAL_STRING("234567", c);
    TEST_ASSERT_TRUE(pair_code_make(999999, c));
    TEST_ASSERT_EQUAL_STRING("999999", c);
    TEST_ASSERT_TRUE(pair_code_make(4293999999u, c));       /* last unbiased word */
    TEST_ASSERT_EQUAL_STRING("999999", c);
}

static void test_code_make_rejects_biased_tail(void)
{
    char c[PAIR_CODE_BUF] = "xxxxxx";
    TEST_ASSERT_FALSE(pair_code_make(4294000000u, c));
    TEST_ASSERT_FALSE(pair_code_make(UINT32_MAX, c));
    TEST_ASSERT_EQUAL_STRING("xxxxxx", c);                  /* untouched */
}

static void test_code_make_always_six_digits(void)
{
    char c[PAIR_CODE_BUF];
    for (uint32_t r = 0; r < 4294000000u; r += 1000003u) {
        TEST_ASSERT_TRUE(pair_code_make(r, c));
        TEST_ASSERT_EQUAL_UINT(PAIR_CODE_LEN, strspn(c, "0123456789"));
        TEST_ASSERT_EQUAL_UINT(PAIR_CODE_LEN, strlen(c));
    }
}

/* ------------------------------------------------------------ /start parsing */

static const char *parse(const char *text)
{
    static char code[PAIR_CODE_BUF];
    return pair_parse_start(text, BOT, code) ? code : NULL;
}

static void test_parse_start_bot_name(void)
{
    char code[PAIR_CODE_BUF];
    TEST_ASSERT_TRUE(pair_parse_start("/start@Ugenes_Bot 123456", BOT, code));   /* any case */
    TEST_ASSERT_FALSE(pair_parse_start("/start@other_bot 123456", BOT, code));
    TEST_ASSERT_FALSE(pair_parse_start("/start@ugenes_bo 123456", BOT, code));
    TEST_ASSERT_FALSE(pair_parse_start("/start@ugenes_bot2 123456", BOT, code));
    TEST_ASSERT_FALSE(pair_parse_start("/start@ 123456", BOT, code));
    TEST_ASSERT_FALSE(pair_parse_start("/start@ugenes_bot 123456", NULL, code));  /* unknown */
    TEST_ASSERT_FALSE(pair_parse_start("/start@ugenes_bot 123456", "", code));
    TEST_ASSERT_TRUE(pair_parse_start("/start 123456", NULL, code));              /* plain ok */
}

static void test_parse_start_ok(void)
{
    TEST_ASSERT_EQUAL_STRING("123456", parse("/start 123456"));
    TEST_ASSERT_EQUAL_STRING("000123", parse("/start 000123"));
    TEST_ASSERT_EQUAL_STRING("123456", parse("/start    123456"));
    TEST_ASSERT_EQUAL_STRING("123456", parse("/start\t123456"));
    TEST_ASSERT_EQUAL_STRING("123456", parse("/start 123456  \n"));
    TEST_ASSERT_EQUAL_STRING("123456", parse("/start@ugenes_bot 123456"));
}

static void test_parse_start_rejects(void)
{
    TEST_ASSERT_NULL(parse(NULL));
    TEST_ASSERT_NULL(parse(""));
    TEST_ASSERT_NULL(parse("/start"));
    TEST_ASSERT_NULL(parse("/start "));
    TEST_ASSERT_NULL(parse("/start@ugenes_bot"));
    TEST_ASSERT_NULL(parse("/start@ugenes_bot "));
    TEST_ASSERT_NULL(parse("/start hello"));
    TEST_ASSERT_NULL(parse("/start 12345"));
    TEST_ASSERT_NULL(parse("/start 1234567"));
    TEST_ASSERT_NULL(parse("/start 123456 7"));
    TEST_ASSERT_NULL(parse("/start 123456x"));
    TEST_ASSERT_NULL(parse("/starts 123456"));
    TEST_ASSERT_NULL(parse("/START 123456"));
    TEST_ASSERT_NULL(parse(" /start 123456"));
    TEST_ASSERT_NULL(parse("start 123456"));
    TEST_ASSERT_NULL(parse("hello /start 123456"));
    TEST_ASSERT_NULL(parse("/start -12345"));
}

/* ------------------------------------------------------------ state machine */

static pair_t P;

static void begin(uint64_t now_ms)
{
    pair_init(&P);
    pair_start(&P, "424242", now_ms, PAIR_TTL_S);
}

static void test_inactive_passes(void)
{
    pair_init(&P);
    TEST_ASSERT_EQUAL(PAIR_PASS, pair_check(&P, "/start 424242", 0));
    TEST_ASSERT_EQUAL_UINT32(0, pair_left_s(&P, 0));
}

static void test_match_keeps_active_until_cancelled(void)
{
    begin(1000);
    TEST_ASSERT_EQUAL_UINT32(300, pair_left_s(&P, 1000));
    TEST_ASSERT_EQUAL(PAIR_MATCH, pair_check(&P, "/start 424242", 2000));
    TEST_ASSERT_TRUE(P.active);                     /* a failed write can retry */
    TEST_ASSERT_EQUAL(PAIR_MATCH, pair_check(&P, "/start@ugenes_bot 424242", 2500));
    /* another bot's command in a group is no attempt: no lockout count */
    TEST_ASSERT_EQUAL(PAIR_PASS, pair_check(&P, "/start@other_bot 000000", 2600));
    TEST_ASSERT_EQUAL_UINT(0, P.wrong);
    pair_cancel(&P);
    TEST_ASSERT_EQUAL(PAIR_PASS, pair_check(&P, "/start 424242", 3000));
}

static void test_wrong_code_is_ignored_and_pairing_stays(void)
{
    begin(0);
    TEST_ASSERT_EQUAL(PAIR_WRONG, pair_check(&P, "/start 111111", 10));
    TEST_ASSERT_TRUE(P.active);
    TEST_ASSERT_EQUAL(PAIR_MATCH, pair_check(&P, "/start 424242", 20));
}

static void test_non_pairing_text_passes(void)
{
    begin(0);
    TEST_ASSERT_EQUAL(PAIR_PASS, pair_check(&P, "hello", 10));
    TEST_ASSERT_EQUAL(PAIR_PASS, pair_check(&P, "/start", 10));
    TEST_ASSERT_EQUAL(PAIR_PASS, pair_check(&P, "/start abc", 10));
    TEST_ASSERT_EQUAL(PAIR_PASS, pair_check(&P, "", 10));
    TEST_ASSERT_EQUAL_UINT(0, P.wrong);             /* none of them counts as an attempt */
}

static void test_expiry(void)
{
    begin(5000);
    uint64_t end = 5000 + PAIR_TTL_S * 1000ull;
    TEST_ASSERT_EQUAL(PAIR_MATCH, pair_check(&P, "/start 424242", end - 1));
    TEST_ASSERT_EQUAL_UINT32(1, pair_left_s(&P, end - 1));
    TEST_ASSERT_EQUAL(PAIR_PASS, pair_check(&P, "/start 424242", end));
    TEST_ASSERT_FALSE(P.active);                    /* expired: gone for good */
    TEST_ASSERT_EQUAL(PAIR_PASS, pair_check(&P, "/start 424242", end + 1));
}

static void test_short_ttl(void)
{
    pair_init(&P);
    pair_start(&P, "424242", 100, 2);
    TEST_ASSERT_EQUAL_UINT32(2, pair_left_s(&P, 100));
    TEST_ASSERT_EQUAL_UINT32(1, pair_left_s(&P, 1100));
    TEST_ASSERT_EQUAL(PAIR_PASS, pair_check(&P, "/start 424242", 2100));
}

static void test_lockout_after_wrong_codes(void)
{
    begin(0);
    for (unsigned k = 1; k < PAIR_MAX_WRONG; k++) {
        TEST_ASSERT_EQUAL(PAIR_WRONG, pair_check(&P, "/start 000000", k));
    }
    TEST_ASSERT_EQUAL(PAIR_LOCKED, pair_check(&P, "/start 000001", 99));
    TEST_ASSERT_FALSE(P.active);
    TEST_ASSERT_EQUAL(PAIR_PASS, pair_check(&P, "/start 424242", 100)); /* right code too late */
}

static void test_restart_resets_attempts(void)
{
    begin(0);
    pair_check(&P, "/start 000000", 1);
    pair_check(&P, "/start 000000", 2);
    pair_start(&P, "777777", 10, PAIR_TTL_S);
    TEST_ASSERT_EQUAL_UINT(0, P.wrong);
    TEST_ASSERT_EQUAL(PAIR_WRONG, pair_check(&P, "/start 424242", 11));     /* old code dead */
    TEST_ASSERT_EQUAL(PAIR_MATCH, pair_check(&P, "/start 777777", 12));
}

/* ------------------------------------------------------------ update fields */

static void test_update_text(void)
{
    int64_t chat = 0;
    const char *text = NULL;
    cJSON *u = cJSON_Parse("{\"update_id\":1,\"message\":{\"message_id\":5,"
                           "\"chat\":{\"id\":-100123456,\"type\":\"group\"},"
                           "\"text\":\"/start 424242\"}}");
    TEST_ASSERT_NOT_NULL(u);
    TEST_ASSERT_TRUE(owner_update_text(u, &chat, &text));
    TEST_ASSERT_EQUAL_INT64(-100123456, chat);
    TEST_ASSERT_EQUAL_STRING("/start 424242", text);
    cJSON_Delete(u);

    u = cJSON_Parse("{\"message\":{\"chat\":{\"id\":7},\"photo\":[]}}");
    TEST_ASSERT_TRUE(owner_update_text(u, &chat, &text));
    TEST_ASSERT_EQUAL_INT64(7, chat);
    TEST_ASSERT_EQUAL_STRING("", text);
    cJSON_Delete(u);

    u = cJSON_Parse("{\"edited_message\":{\"chat\":{\"id\":7},\"text\":\"x\"}}");
    TEST_ASSERT_FALSE(owner_update_text(u, &chat, &text));
    cJSON_Delete(u);
    u = cJSON_Parse("{\"message\":{\"text\":\"x\"}}");
    TEST_ASSERT_FALSE(owner_update_text(u, &chat, &text));
    cJSON_Delete(u);
}

/* ------------------------------------------------------------ tg owner / notice */

static void test_parse_chat(void)
{
    int64_t v = 0;
    TEST_ASSERT_TRUE(owner_parse_chat("123456789", &v));
    TEST_ASSERT_EQUAL_INT64(123456789, v);
    TEST_ASSERT_TRUE(owner_parse_chat("-1001234567890", &v));
    TEST_ASSERT_EQUAL_INT64(-1001234567890LL, v);
    TEST_ASSERT_TRUE(owner_parse_chat("999999999999999", &v));
    TEST_ASSERT_EQUAL_INT64(999999999999999LL, v);
    const char *bad[] = {"", "-", "0", "-0", "000", "12a", "1.5", "+5", " 5", "5 ",
                         "1000000000000000", "--5"};
    for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
        TEST_ASSERT_FALSE_MESSAGE(owner_parse_chat(bad[k], &v), bad[k]);
    }
}

static void test_notice_format(void)
{
    char out[96];
    size_t n = notice_format(out, sizeof(out), "0.3.1", "software");
    TEST_ASSERT_EQUAL_STRING("embpet online \xC2\xB7 fw 0.3.1 \xC2\xB7 reset software", out);
    TEST_ASSERT_EQUAL_UINT(strlen(out), n);
    TEST_ASSERT_EQUAL_STRING("embpet online \xC2\xB7 fw v1-4-gabc-dirty \xC2\xB7 reset poweron",
                             (notice_format(out, sizeof(out), "v1-4-gabc-dirty", "poweron"), out));
}

static void test_notice_format_truncates(void)
{
    char out[10];
    size_t n = notice_format(out, sizeof(out), "1", "panic");
    TEST_ASSERT_EQUAL_UINT(strlen("embpet online \xC2\xB7 fw 1 \xC2\xB7 reset panic"), n);
    TEST_ASSERT_EQUAL_UINT(9, strlen(out));
    TEST_ASSERT_EQUAL_INT(0, strncmp(out, "embpet on", 9));
    TEST_ASSERT_TRUE(notice_format(NULL, 0, "1", "x") > 0);     /* cap 0: length only */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_code_make);
    RUN_TEST(test_code_make_rejects_biased_tail);
    RUN_TEST(test_code_make_always_six_digits);
    RUN_TEST(test_parse_start_bot_name);
    RUN_TEST(test_parse_start_ok);
    RUN_TEST(test_parse_start_rejects);
    RUN_TEST(test_inactive_passes);
    RUN_TEST(test_match_keeps_active_until_cancelled);
    RUN_TEST(test_wrong_code_is_ignored_and_pairing_stays);
    RUN_TEST(test_non_pairing_text_passes);
    RUN_TEST(test_expiry);
    RUN_TEST(test_short_ttl);
    RUN_TEST(test_lockout_after_wrong_codes);
    RUN_TEST(test_restart_resets_attempts);
    RUN_TEST(test_update_text);
    RUN_TEST(test_parse_chat);
    RUN_TEST(test_notice_format);
    RUN_TEST(test_notice_format_truncates);
    return UNITY_END();
}
