#include <string.h>

#include "prefix_core.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static bool parse(const char *s, char *out)
{
    return prefix_parse_actor(s, strlen(s), out);
}

static void test_valid_prefix_with_text(void)
{
    char a[ACTOR_BUF_SIZE] = "";
    TEST_ASSERT_TRUE(parse("[A] hello", a));
    TEST_ASSERT_EQUAL_STRING("A", a);
}

static void test_prefix_only_no_text(void)
{
    char a[ACTOR_BUF_SIZE] = "";
    TEST_ASSERT_TRUE(parse("[bot_1]", a));
    TEST_ASSERT_EQUAL_STRING("bot_1", a);
}

static void test_no_prefix(void)
{
    char a[ACTOR_BUF_SIZE] = "keep";
    TEST_ASSERT_FALSE(parse("hello [A] x", a));
    TEST_ASSERT_FALSE(parse("A] hello", a));
    TEST_ASSERT_FALSE(parse(" [A] hello", a));
    TEST_ASSERT_FALSE(parse("", a));
    TEST_ASSERT_FALSE(parse("[", a));
    TEST_ASSERT_FALSE(parse("[A", a));
    TEST_ASSERT_EQUAL_STRING("keep", a);
}

static void test_empty_actor(void)
{
    char a[ACTOR_BUF_SIZE];
    TEST_ASSERT_FALSE(parse("[] hi", a));
}

static void test_bad_chars(void)
{
    char a[ACTOR_BUF_SIZE];
    TEST_ASSERT_FALSE(parse("[a b] hi", a));
    TEST_ASSERT_FALSE(parse("[a.b] hi", a));
    TEST_ASSERT_FALSE(parse("[a[b] hi", a));
    TEST_ASSERT_FALSE(parse("[\xD0\x96] hi", a));      /* UTF-8 inside the tag */
    TEST_ASSERT_FALSE(parse("[a\n] hi", a));
}

static void test_length_boundaries(void)
{
    char a[ACTOR_BUF_SIZE] = "";
    TEST_ASSERT_TRUE(parse("[abcdefghijklmnop] x", a));        /* 16 */
    TEST_ASSERT_EQUAL_STRING("abcdefghijklmnop", a);
    TEST_ASSERT_FALSE(parse("[abcdefghijklmnopq] x", a));      /* 17 */
    TEST_ASSERT_FALSE(parse("[abcdefghijklmnopqrstuvwxyz0123456789] x", a));
    TEST_ASSERT_TRUE(parse("[Z] x", a));                       /* 1 */
}

static void test_charset_edges(void)
{
    char a[ACTOR_BUF_SIZE] = "";
    TEST_ASSERT_TRUE(parse("[Az09_-] x", a));
    TEST_ASSERT_EQUAL_STRING("Az09_-", a);
    TEST_ASSERT_FALSE(parse("[@] x", a));      /* just before 'A' */
    TEST_ASSERT_FALSE(parse("[{] x", a));      /* just after 'z' */
    TEST_ASSERT_FALSE(parse("[:] x", a));      /* just after '9' */
}

static void test_utf8_after_tag(void)
{
    char a[ACTOR_BUF_SIZE] = "";
    TEST_ASSERT_TRUE(parse("[A] \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xF0\x9F\x99\x82", a));
    TEST_ASSERT_EQUAL_STRING("A", a);
    TEST_ASSERT_TRUE(parse("[B]\xD0\x96", a));
    TEST_ASSERT_EQUAL_STRING("B", a);
}

static void test_length_is_respected(void)
{
    char a[ACTOR_BUF_SIZE] = "";
    TEST_ASSERT_FALSE(prefix_parse_actor("[A] x", 2, a));      /* truncated before ']' */
    TEST_ASSERT_TRUE(prefix_parse_actor("[A] x", 3, a));
    TEST_ASSERT_FALSE(prefix_parse_actor(NULL, 0, a));
}

static void test_actor_id_valid(void)
{
    TEST_ASSERT_TRUE(actor_id_valid("a", 1));
    TEST_ASSERT_FALSE(actor_id_valid("", 0));
    TEST_ASSERT_FALSE(actor_id_valid(NULL, 1));
    TEST_ASSERT_TRUE(actor_id_valid("0123456789abcdef", 16));
    TEST_ASSERT_FALSE(actor_id_valid("0123456789abcdefg", 17));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_valid_prefix_with_text);
    RUN_TEST(test_prefix_only_no_text);
    RUN_TEST(test_no_prefix);
    RUN_TEST(test_empty_actor);
    RUN_TEST(test_bad_chars);
    RUN_TEST(test_length_boundaries);
    RUN_TEST(test_charset_edges);
    RUN_TEST(test_utf8_after_tag);
    RUN_TEST(test_length_is_respected);
    RUN_TEST(test_actor_id_valid);
    return UNITY_END();
}
