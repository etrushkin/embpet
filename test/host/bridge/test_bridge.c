#include <string.h>

#include "bridge_core.h"
#include "unity.h"

#define OWNER 4242
#define BOT   8234567           /* the bot's user id (token "8234567:...") */
#define FROM_BOT "\"from\":{\"id\":8234567,\"is_bot\":true}"

static pending_table_t T;
static cJSON *U;
static bridge_route_t R;

void setUp(void)
{
    pending_init(&T);
    U = NULL;
    memset(&R, 0xAA, sizeof R);
}

void tearDown(void)
{
    cJSON_Delete(U);
}

static bridge_route_kind_t route(const char *json)
{
    cJSON_Delete(U);
    U = cJSON_Parse(json);
    TEST_ASSERT_NOT_NULL_MESSAGE(U, json);
    return bridge_route(U, OWNER, BOT, &T, &R);
}

/* ------------------------------------------------------------ bridge_route */

static void test_reply_from_table(void)
{
    TEST_ASSERT_TRUE(pending_add(&T, 502, "B", 0, NULL));
    TEST_ASSERT_EQUAL(BRIDGE_REPLY, route(
        "{\"update_id\":7,\"message\":{\"message_id\":503,\"date\":1760450100,"
        "\"chat\":{\"id\":4242,\"type\":\"private\"},\"text\":\"yes\","
        "\"reply_to_message\":{\"message_id\":502," FROM_BOT ","
        "\"text\":\"[B] Disk at 91%\"}}}"));
    TEST_ASSERT_EQUAL_STRING("B", R.actor);
    TEST_ASSERT_TRUE(R.from_table);
    TEST_ASSERT_EQUAL_INT64(502, R.to);
    TEST_ASSERT_EQUAL_INT64(503, R.msg);
    TEST_ASSERT_EQUAL_INT64(1760450100, R.date);
    TEST_ASSERT_EQUAL_STRING("yes", R.text);
    TEST_ASSERT_EQUAL_UINT(1, pending_count(&T));       /* routing changes nothing */
}

static void test_table_wins_over_prefix(void)
{
    /* the table is authoritative even if the quoted text carries another tag */
    TEST_ASSERT_TRUE(pending_add(&T, 10, "A", 0, NULL));
    TEST_ASSERT_EQUAL(BRIDGE_REPLY, route(
        "{\"message\":{\"message_id\":11,\"chat\":{\"id\":4242},\"text\":\"x\","
        "\"reply_to_message\":{\"message_id\":10," FROM_BOT ","
        "\"text\":\"[B] y\"}}}"));
    TEST_ASSERT_EQUAL_STRING("A", R.actor);
    TEST_ASSERT_TRUE(R.from_table);
}

static void test_answered_entry_still_routes_from_table(void)
{
    TEST_ASSERT_TRUE(pending_add(&T, 10, "A", 0, NULL));
    TEST_ASSERT_EQUAL_UINT(1, pending_ack_msg(&T, 10));
    TEST_ASSERT_EQUAL(BRIDGE_REPLY, route(
        "{\"message\":{\"message_id\":11,\"chat\":{\"id\":4242},\"text\":\"again\","
        "\"reply_to_message\":{\"message_id\":10}}}"));
    TEST_ASSERT_TRUE(R.from_table);
    TEST_ASSERT_EQUAL_STRING("A", R.actor);
}

static void test_reply_from_prefix_after_reboot(void)
{
    TEST_ASSERT_EQUAL(BRIDGE_REPLY, route(
        "{\"message\":{\"message_id\":504,\"date\":5,\"chat\":{\"id\":4242},"
        "\"text\":\"no, check flaky test first\","
        "\"reply_to_message\":{\"message_id\":501," FROM_BOT ","
        "\"text\":\"[A] Build #42 failed\"}}}"));
    TEST_ASSERT_EQUAL_STRING("A", R.actor);
    TEST_ASSERT_FALSE(R.from_table);
    TEST_ASSERT_EQUAL_INT64(501, R.to);
    TEST_ASSERT_EQUAL_INT64(504, R.msg);
    TEST_ASSERT_EQUAL_STRING("no, check flaky test first", R.text);
}

static void test_prefix_null_table(void)
{
    U = cJSON_Parse("{\"message\":{\"message_id\":2,\"chat\":{\"id\":4242},\"text\":\"t\","
                    "\"reply_to_message\":{\"message_id\":1," FROM_BOT ","
                    "\"text\":\"[bot_1-x]\"}}}");
    TEST_ASSERT_EQUAL(BRIDGE_REPLY, bridge_route(U, OWNER, BOT, NULL, &R));
    TEST_ASSERT_EQUAL_STRING("bot_1-x", R.actor);
}

static void test_prefix_needs_bot_author(void)
{
    /* the owner's own message that happens to start with "[A]" */
    TEST_ASSERT_EQUAL(BRIDGE_HINT, route(
        "{\"message\":{\"message_id\":2,\"chat\":{\"id\":4242},\"text\":\"t\","
        "\"reply_to_message\":{\"message_id\":1,\"from\":{\"id\":4242,\"is_bot\":false},"
        "\"text\":\"[A] mine\"}}}"));
    TEST_ASSERT_EQUAL_INT64(2, R.msg);
    TEST_ASSERT_EQUAL(BRIDGE_HINT, route(
        "{\"message\":{\"message_id\":2,\"chat\":{\"id\":4242},\"text\":\"t\","
        "\"reply_to_message\":{\"message_id\":1,\"text\":\"[A] no from\"}}}"));
    /* another bot's message (e.g. forwarded into the chat): is_bot alone is not enough */
    TEST_ASSERT_EQUAL(BRIDGE_HINT, route(
        "{\"message\":{\"message_id\":2,\"chat\":{\"id\":4242},\"text\":\"t\","
        "\"reply_to_message\":{\"message_id\":1,\"from\":{\"id\":8234568,\"is_bot\":true},"
        "\"text\":\"[A] other bot\"}}}"));
    TEST_ASSERT_EQUAL(BRIDGE_HINT, route(
        "{\"message\":{\"message_id\":2,\"chat\":{\"id\":4242},\"text\":\"t\","
        "\"reply_to_message\":{\"message_id\":1,\"from\":{\"is_bot\":true},"
        "\"text\":\"[A] no id\"}}}"));
    /* this bot's id decides, whatever is_bot says */
    TEST_ASSERT_EQUAL(BRIDGE_REPLY, route(
        "{\"message\":{\"message_id\":2,\"chat\":{\"id\":4242},\"text\":\"t\","
        "\"reply_to_message\":{\"message_id\":1,\"from\":{\"id\":8234567},"
        "\"text\":\"[A] ours\"}}}"));
    TEST_ASSERT_EQUAL_STRING("A", R.actor);
    /* bot id unknown (0): no prefix routing at all */
    TEST_ASSERT_EQUAL(BRIDGE_HINT, bridge_route(U, OWNER, 0, &T, &R));
}

static void test_bot_id_from_token(void)
{
    TEST_ASSERT_EQUAL_INT64(8234567, bridge_bot_id("8234567:AAHk-secret_part"));
    TEST_ASSERT_EQUAL_INT64(1, bridge_bot_id("1:x"));
    TEST_ASSERT_EQUAL_INT64(999999999999999LL, bridge_bot_id("999999999999999:x"));
    TEST_ASSERT_EQUAL_INT64(0, bridge_bot_id("9999999999999999:x"));    /* 16 digits */
    TEST_ASSERT_EQUAL_INT64(0, bridge_bot_id(":x"));
    TEST_ASSERT_EQUAL_INT64(0, bridge_bot_id("123"));
    TEST_ASSERT_EQUAL_INT64(0, bridge_bot_id("12a:x"));
    TEST_ASSERT_EQUAL_INT64(0, bridge_bot_id(""));
    TEST_ASSERT_EQUAL_INT64(0, bridge_bot_id(NULL));
}

static void test_reply_to_untagged_bot_message_is_hint(void)
{
    /* e.g. a Reply to the hint itself or to the online notice */
    TEST_ASSERT_EQUAL(BRIDGE_HINT, route(
        "{\"message\":{\"message_id\":9,\"chat\":{\"id\":4242},\"text\":\"?\","
        "\"reply_to_message\":{\"message_id\":8," FROM_BOT ","
        "\"text\":\"Use Reply on an actor's message so I know who it's for.\"}}}"));
    TEST_ASSERT_EQUAL_INT64(9, R.msg);
    TEST_ASSERT_EQUAL_INT64(0, R.to);
    TEST_ASSERT_EQUAL(BRIDGE_HINT, route(
        "{\"message\":{\"message_id\":9,\"chat\":{\"id\":4242},\"text\":\"?\","
        "\"reply_to_message\":{\"message_id\":8," FROM_BOT ","
        "\"photo\":[]}}}"));
    TEST_ASSERT_EQUAL(BRIDGE_HINT, route(
        "{\"message\":{\"message_id\":9,\"chat\":{\"id\":4242},\"text\":\"?\","
        "\"reply_to_message\":{\"message_id\":8," FROM_BOT ","
        "\"text\":\"[A b] bad tag\"}}}"));
}

static void test_no_reply_is_hint(void)
{
    TEST_ASSERT_EQUAL(BRIDGE_HINT, route(
        "{\"update_id\":1,\"message\":{\"message_id\":77,\"date\":3,"
        "\"chat\":{\"id\":4242},\"text\":\"hello\"}}"));
    TEST_ASSERT_EQUAL_INT64(77, R.msg);
    TEST_ASSERT_EQUAL_INT64(3, R.date);
    TEST_ASSERT_EQUAL(BRIDGE_HINT, route(
        "{\"message\":{\"message_id\":78,\"chat\":{\"id\":4242},\"sticker\":{}}}"));
}

static void test_reply_without_message_id_is_hint(void)
{
    TEST_ASSERT_EQUAL(BRIDGE_HINT, route(
        "{\"message\":{\"message_id\":2,\"chat\":{\"id\":4242},\"text\":\"t\","
        "\"reply_to_message\":{" FROM_BOT ",\"text\":\"[A] x\"}}}"));
}

static void test_other_chat_ignored(void)
{
    TEST_ASSERT_TRUE(pending_add(&T, 10, "A", 0, NULL));
    TEST_ASSERT_EQUAL(BRIDGE_IGNORE, route(
        "{\"message\":{\"message_id\":11,\"chat\":{\"id\":4243},\"text\":\"x\","
        "\"reply_to_message\":{\"message_id\":10}}}"));
    TEST_ASSERT_EQUAL(BRIDGE_IGNORE, route(
        "{\"message\":{\"message_id\":11,\"chat\":{\"id\":-4242},\"text\":\"x\"}}"));
    TEST_ASSERT_EQUAL(BRIDGE_IGNORE, route(
        "{\"message\":{\"message_id\":11,\"chat\":{},\"text\":\"x\"}}"));
    TEST_ASSERT_EQUAL(BRIDGE_IGNORE, route(
        "{\"message\":{\"message_id\":11,\"chat\":{\"id\":\"4242\"},\"text\":\"x\"}}"));
}

static void test_no_owner_ignores_everything(void)
{
    U = cJSON_Parse("{\"message\":{\"message_id\":11,\"chat\":{\"id\":4242},\"text\":\"x\"}}");
    TEST_ASSERT_EQUAL(BRIDGE_IGNORE, bridge_route(U, 0, BOT, &T, &R));
}

static void test_non_message_updates_ignored(void)
{
    TEST_ASSERT_EQUAL(BRIDGE_IGNORE, route("{\"update_id\":1}"));
    TEST_ASSERT_EQUAL(BRIDGE_IGNORE, route(
        "{\"update_id\":1,\"edited_message\":{\"message_id\":1,\"chat\":{\"id\":4242}}}"));
    TEST_ASSERT_EQUAL(BRIDGE_IGNORE, route("{\"message\":\"x\"}"));
    TEST_ASSERT_EQUAL(BRIDGE_IGNORE, route("{\"message\":{\"chat\":{\"id\":4242}}}"));
    TEST_ASSERT_EQUAL(BRIDGE_IGNORE, route("[]"));
    TEST_ASSERT_EQUAL(BRIDGE_IGNORE, bridge_route(NULL, OWNER, BOT, &T, &R));
}

static void test_media_reply_uses_caption_or_empty(void)
{
    TEST_ASSERT_TRUE(pending_add(&T, 10, "A", 0, NULL));
    TEST_ASSERT_EQUAL(BRIDGE_REPLY, route(
        "{\"message\":{\"message_id\":11,\"chat\":{\"id\":4242},\"photo\":[],"
        "\"caption\":\"see pic\",\"reply_to_message\":{\"message_id\":10}}}"));
    TEST_ASSERT_EQUAL_STRING("see pic", R.text);
    TEST_ASSERT_EQUAL(BRIDGE_REPLY, route(
        "{\"message\":{\"message_id\":12,\"chat\":{\"id\":4242},\"sticker\":{},"
        "\"reply_to_message\":{\"message_id\":10}}}"));
    TEST_ASSERT_EQUAL_STRING("", R.text);
    TEST_ASSERT_EQUAL_INT64(0, R.date);
}

static void test_large_ids_and_utf8_text(void)
{
    TEST_ASSERT_TRUE(pending_add(&T, 2147483648LL, "A", 0, NULL));
    U = cJSON_Parse("{\"message\":{\"message_id\":2147483649,\"chat\":{\"id\":-1001234567890},"
                    "\"text\":\"\xD0\x96 \xF0\x9F\x98\x80\","
                    "\"reply_to_message\":{\"message_id\":2147483648}}}");
    TEST_ASSERT_EQUAL(BRIDGE_REPLY, bridge_route(U, -1001234567890LL, BOT, &T, &R));
    TEST_ASSERT_EQUAL_INT64(2147483648LL, R.to);
    TEST_ASSERT_EQUAL_INT64(2147483649LL, R.msg);
    TEST_ASSERT_EQUAL_STRING("\xD0\x96 \xF0\x9F\x98\x80", R.text);
}

/* ------------------------------------------------------------ bridge_check_text */

static void test_check_text_bytes(void)
{
    static char buf[4200];
    memset(buf, 'x', 4092);
    buf[4092] = '\0';
    /* "[A] " + 4092 = 4096 units: the largest that fits with a 1-char actor */
    TEST_ASSERT_EQUAL(BRIDGE_TEXT_OK, bridge_check_text("A", buf, 4096));
    buf[4092] = 'x';
    buf[4093] = '\0';
    TEST_ASSERT_EQUAL(BRIDGE_TEXT_TOO_LONG, bridge_check_text("A", buf, 4096));
    memset(buf, 'x', 4097);
    buf[4097] = '\0';
    TEST_ASSERT_EQUAL(BRIDGE_TEXT_TOO_LONG, bridge_check_text("A", buf, 4096));
    /* 16-char actor: 4077 ASCII chars fit (README 2.1) */
    memset(buf, 'x', 4077);
    buf[4077] = '\0';
    TEST_ASSERT_EQUAL(BRIDGE_TEXT_OK, bridge_check_text("abcdefghijklmnop", buf, 4096));
    buf[4077] = 'x';
    buf[4078] = '\0';
    TEST_ASSERT_EQUAL(BRIDGE_TEXT_TOO_LONG, bridge_check_text("abcdefghijklmnop", buf, 4096));
}

static void test_check_text_utf16(void)
{
    static char buf[4200];
    /* 1365 x U+0416 (2 bytes, 1 unit) = 2730 bytes: well within both limits */
    for (int i = 0; i < 1365; i++) {
        memcpy(&buf[2 * i], "\xD0\x96", 2);
    }
    buf[2730] = '\0';
    TEST_ASSERT_EQUAL(BRIDGE_TEXT_OK, bridge_check_text("A", buf, 4096));
    /* 1023 emoji (4 bytes, 2 units) = 4092 bytes, 2046 units: fits */
    for (int i = 0; i < 1023; i++) {
        memcpy(&buf[4 * i], "\xF0\x9F\x98\x80", 4);
    }
    buf[4092] = '\0';
    TEST_ASSERT_EQUAL(BRIDGE_TEXT_OK, bridge_check_text("A", buf, 4096));
    /* 1024 emoji = 4096 bytes (byte limit exactly) */
    memcpy(&buf[4092], "\xF0\x9F\x98\x80", 5);
    TEST_ASSERT_EQUAL(BRIDGE_TEXT_OK, bridge_check_text("A", buf, 4096));
    /* the byte limit is the caller's */
    TEST_ASSERT_EQUAL(BRIDGE_TEXT_TOO_LONG, bridge_check_text("A", buf, 4095));
}

static void test_check_text_bad_utf8_and_empty(void)
{
    TEST_ASSERT_EQUAL(BRIDGE_TEXT_BAD_UTF8, bridge_check_text("A", "\xC0\xAF", 4096));
    TEST_ASSERT_EQUAL(BRIDGE_TEXT_BAD_UTF8, bridge_check_text("A", "ok \xED\xA0\x80", 4096));
    TEST_ASSERT_EQUAL(BRIDGE_TEXT_BAD_UTF8, bridge_check_text("A", "\xF0\x9F\x98", 4096));
    TEST_ASSERT_EQUAL(BRIDGE_TEXT_OK, bridge_check_text("A", "", 4096));
}

/* ------------------------------------------------------------ bridge_send_err */

static void test_send_err_mapping(void)
{
    TEST_ASSERT_EQUAL_STRING("offline", bridge_send_err(TG_ERR_NET, 0, ""));
    TEST_ASSERT_EQUAL_STRING("offline", bridge_send_err(TG_ERR_SERVER, 502, "Bad Gateway"));
    TEST_ASSERT_EQUAL_STRING("timeout", bridge_send_err(TG_ERR_TIMEOUT, 0, ""));
    TEST_ASSERT_EQUAL_STRING("auth_error", bridge_send_err(TG_ERR_AUTH, 401, "Unauthorized"));
    TEST_ASSERT_EQUAL_STRING("not_configured", bridge_send_err(TG_ERR_NO_TOKEN, 0, ""));
    TEST_ASSERT_EQUAL_STRING("busy", bridge_send_err(TG_ERR_RETRY, 429, "Too Many Requests"));
    TEST_ASSERT_EQUAL_STRING("too_long", bridge_send_err(TG_ERR_TOO_LONG, 0, ""));
    TEST_ASSERT_EQUAL_STRING("bad_args", bridge_send_err(TG_ERR_BAD_ARG, 0, ""));
    TEST_ASSERT_EQUAL_STRING("internal", bridge_send_err(TG_ERR_NO_MEM, 0, ""));
    TEST_ASSERT_EQUAL_STRING("internal", bridge_send_err(TG_ERR_TOO_LARGE, 0, ""));
}

static void test_send_err_api(void)
{
    TEST_ASSERT_EQUAL_STRING("not_configured",
                             bridge_send_err(TG_ERR_API, 403,
                                             "Forbidden: bot was blocked by the user"));
    TEST_ASSERT_EQUAL_STRING("not_configured",
                             bridge_send_err(TG_ERR_API, 400, "Bad Request: chat not found"));
    TEST_ASSERT_EQUAL_STRING("bad_args",
                             bridge_send_err(TG_ERR_API, 400, "Bad Request: message is too long"));
    TEST_ASSERT_EQUAL_STRING("bad_args", bridge_send_err(TG_ERR_API, 400, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_reply_from_table);
    RUN_TEST(test_table_wins_over_prefix);
    RUN_TEST(test_answered_entry_still_routes_from_table);
    RUN_TEST(test_reply_from_prefix_after_reboot);
    RUN_TEST(test_prefix_null_table);
    RUN_TEST(test_prefix_needs_bot_author);
    RUN_TEST(test_bot_id_from_token);
    RUN_TEST(test_reply_to_untagged_bot_message_is_hint);
    RUN_TEST(test_no_reply_is_hint);
    RUN_TEST(test_reply_without_message_id_is_hint);
    RUN_TEST(test_other_chat_ignored);
    RUN_TEST(test_no_owner_ignores_everything);
    RUN_TEST(test_non_message_updates_ignored);
    RUN_TEST(test_media_reply_uses_caption_or_empty);
    RUN_TEST(test_large_ids_and_utf8_text);
    RUN_TEST(test_check_text_bytes);
    RUN_TEST(test_check_text_utf16);
    RUN_TEST(test_check_text_bad_utf8_and_empty);
    RUN_TEST(test_send_err_mapping);
    RUN_TEST(test_send_err_api);
    return UNITY_END();
}
