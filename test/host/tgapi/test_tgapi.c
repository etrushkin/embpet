#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "fixtures.h"
#include "tgapi_core.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static size_t u16(const char *s)
{
    return tg_utf16_len(s, strlen(s));
}

static tg_status_t parse(int http, const char *body, tg_resp_t *r)
{
    return tg_parse_response(http, body, strlen(body), TG_RESP_MAX, r);
}

/* ------------------------------------------------------------ UTF-16 */

static void test_utf16_len_counts_code_units(void)
{
    TEST_ASSERT_EQUAL_size_t(0, u16(""));
    TEST_ASSERT_EQUAL_size_t(3, u16("[A]"));
    TEST_ASSERT_EQUAL_size_t(6, u16("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"));  /* Привет */
    TEST_ASSERT_EQUAL_size_t(1, u16("\xE4\xB8\xAD"));                 /* 中, BMP 3 bytes */
    TEST_ASSERT_EQUAL_size_t(1, u16("\xE2\x82\xAC"));                 /* € */
    TEST_ASSERT_EQUAL_size_t(2, u16("\xF0\x9F\x98\x80"));             /* 😀 U+1F600 */
    TEST_ASSERT_EQUAL_size_t(2, u16("\xF0\xA0\x80\x80"));             /* 𠀀 U+20000 (CJK ext B) */
    TEST_ASSERT_EQUAL_size_t(2, u16("\xF4\x8F\xBF\xBF"));             /* U+10FFFF, the last */
    TEST_ASSERT_EQUAL_size_t(1, u16("\xEF\xBF\xBF"));                 /* U+FFFF */
    /* 👍🏽 = thumbs up + skin tone modifier: two astral code points, 4 units */
    TEST_ASSERT_EQUAL_size_t(4, u16("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD"));
    /* "[A] 😀 Привет" = 3 + 1 + 2 + 1 + 6 */
    TEST_ASSERT_EQUAL_size_t(13, u16("[A] \xF0\x9F\x98\x80 \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"));
}

static void test_utf16_len_rejects_malformed(void)
{
    static const char *const bad[] = {
        "\x80",                 /* stray continuation */
        "a\xBF\x62",            /* "a", stray continuation, "b" */
        "\xC0\x80",             /* overlong NUL */
        "\xC1\xBF",             /* overlong */
        "\xE0\x80\x80",         /* overlong 3-byte */
        "\xE0\x9F\xBF",         /* overlong 3-byte, upper edge */
        "\xF0\x80\x80\x80",     /* overlong 4-byte */
        "\xF0\x8F\xBF\xBF",     /* overlong 4-byte, upper edge */
        "\xED\xA0\x80",         /* U+D800 high surrogate */
        "\xED\xBF\xBF",         /* U+DFFF low surrogate */
        "\xF4\x90\x80\x80",     /* U+110000 */
        "\xF5\x80\x80\x80",
        "\xFF",
        "\xC3\x28",             /* bad continuation */
        "\xF0\x9F\x98",         /* truncated emoji */
        "ok\xD0",               /* truncated at the end */
        "\xE2\x82",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_size_t_MESSAGE(TG_UTF16_INVALID, u16(bad[i]), bad[i]);
    }
    TEST_ASSERT_EQUAL_size_t(TG_UTF16_INVALID, tg_utf16_len(NULL, 1));
    TEST_ASSERT_EQUAL_size_t(0, tg_utf16_len(NULL, 0));
    /* the length bounds the scan: a valid prefix of a longer buffer */
    TEST_ASSERT_EQUAL_size_t(2, tg_utf16_len("ab\xFF", 2));
}

static void test_utf16_span_for_tag(void)
{
    size_t off = 99;
    size_t len = 99;
    const char *t = "[A] \xF0\x9F\x98\x80";
    TEST_ASSERT_TRUE(tg_utf16_span(t, strlen(t), 0, 3, &off, &len));
    TEST_ASSERT_EQUAL_size_t(0, off);
    TEST_ASSERT_EQUAL_size_t(3, len);

    /* a tag after astral and Cyrillic text: offsets count UTF-16 units, not bytes */
    const char *u = "\xF0\x9F\x98\x80\xD0\x96 [bot_1] x";          /* "😀Ж [bot_1] x" */
    TEST_ASSERT_TRUE(tg_utf16_span(u, strlen(u), 7, 7, &off, &len));
    TEST_ASSERT_EQUAL_size_t(4, off);                               /* 2 + 1 + 1 */
    TEST_ASSERT_EQUAL_size_t(7, len);

    /* a span covering astral characters */
    TEST_ASSERT_TRUE(tg_utf16_span(u, strlen(u), 0, 6, &off, &len));
    TEST_ASSERT_EQUAL_size_t(0, off);
    TEST_ASSERT_EQUAL_size_t(3, len);

    /* whole string and empty span at the end */
    TEST_ASSERT_TRUE(tg_utf16_span(u, strlen(u), 0, strlen(u), &off, &len));
    TEST_ASSERT_EQUAL_size_t(u16(u), len);
    TEST_ASSERT_TRUE(tg_utf16_span(u, strlen(u), strlen(u), 0, &off, &len));
    TEST_ASSERT_EQUAL_size_t(u16(u), off);
    TEST_ASSERT_EQUAL_size_t(0, len);
}

static void test_utf16_span_rejects_bad_ranges(void)
{
    size_t off;
    size_t len;
    const char *u = "\xF0\x9F\x98\x80\xD0\x96x";
    TEST_ASSERT_FALSE(tg_utf16_span(u, strlen(u), 1, 2, &off, &len));    /* starts mid-emoji */
    TEST_ASSERT_FALSE(tg_utf16_span(u, strlen(u), 0, 5, &off, &len));    /* ends mid-Ж */
    TEST_ASSERT_FALSE(tg_utf16_span(u, strlen(u), 0, 8, &off, &len));    /* past the end */
    TEST_ASSERT_FALSE(tg_utf16_span(u, strlen(u), 8, 0, &off, &len));
    TEST_ASSERT_FALSE(tg_utf16_span(u, strlen(u), SIZE_MAX, 2, &off, &len));
    const char *bad = "[A] \xED\xA0\x80";
    TEST_ASSERT_FALSE(tg_utf16_span(bad, strlen(bad), 0, 3, &off, &len));  /* malformed later */
}

/* ------------------------------------------------------------ sendMessage body */

static cJSON *build_send(int64_t chat, const char *actor, const char *text, int64_t reply)
{
    char *json = NULL;
    TEST_ASSERT_EQUAL(TG_OK, tg_build_send_message(chat, actor, text, reply, &json));
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_NULL(strstr(json, "parse_mode"));
    cJSON *root = cJSON_Parse(json);
    cJSON_free(json);
    TEST_ASSERT_NOT_NULL(root);
    return root;
}

static void assert_bold(const cJSON *root, double offset, double length)
{
    const cJSON *ents = cJSON_GetObjectItemCaseSensitive(root, "entities");
    TEST_ASSERT_TRUE(cJSON_IsArray(ents));
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(ents));
    const cJSON *e = cJSON_GetArrayItem(ents, 0);
    TEST_ASSERT_EQUAL_STRING("bold", cJSON_GetObjectItemCaseSensitive(e, "type")->valuestring);
    TEST_ASSERT_EQUAL_DOUBLE(offset, cJSON_GetObjectItemCaseSensitive(e, "offset")->valuedouble);
    TEST_ASSERT_EQUAL_DOUBLE(length, cJSON_GetObjectItemCaseSensitive(e, "length")->valuedouble);
}

static void test_send_message_body_emoji_cyrillic(void)
{
    const char *text = "\xF0\x9F\x98\x80 \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xF0\xA0\x80\x80";
    cJSON *root = build_send(111222333, "A", text, 0);
    int64_t chat = 0;
    TEST_ASSERT_TRUE(tg_json_i64(root, "chat_id", &chat));
    TEST_ASSERT_EQUAL_INT64(111222333, chat);
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "text");
    char want[96];
    strcpy(want, "[A] ");
    strcat(want, text);
    TEST_ASSERT_EQUAL_STRING(want, t->valuestring);         /* UTF-8 passes through */
    assert_bold(root, 0, 3);
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(root, "reply_parameters"));
    cJSON_Delete(root);
}

static void test_send_message_body_long_actor_and_reply(void)
{
    cJSON *root = build_send(-1001234567890123LL, "abcdefghijklmnop", "x", 501);
    int64_t chat = 0;
    TEST_ASSERT_TRUE(tg_json_i64(root, "chat_id", &chat));
    TEST_ASSERT_EQUAL_INT64(-1001234567890123LL, chat);     /* survives the double */
    assert_bold(root, 0, 18);
    const cJSON *rp = cJSON_GetObjectItemCaseSensitive(root, "reply_parameters");
    int64_t mid = 0;
    TEST_ASSERT_TRUE(tg_json_i64(rp, "message_id", &mid));
    TEST_ASSERT_EQUAL_INT64(501, mid);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rp,
                                                                   "allow_sending_without_reply")));
    TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(rp));
    cJSON_Delete(root);
}

static void test_send_message_body_plain_text(void)
{
    const char *text = "Use Reply on an actor's message \xF0\x9F\x98\x80";
    cJSON *root = build_send(42, NULL, text, 0);
    TEST_ASSERT_EQUAL_STRING(text, cJSON_GetObjectItemCaseSensitive(root, "text")->valuestring);
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(root, "entities"));
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(root, "reply_parameters"));
    TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(root));         /* chat_id, text */
    cJSON_Delete(root);

    root = build_send(42, NULL, "[B] not a tag", 77);
    TEST_ASSERT_EQUAL_STRING("[B] not a tag",
                             cJSON_GetObjectItemCaseSensitive(root, "text")->valuestring);
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(root, "entities"));
    int64_t mid = 0;
    TEST_ASSERT_TRUE(tg_json_i64(cJSON_GetObjectItemCaseSensitive(root, "reply_parameters"),
                                 "message_id", &mid));
    TEST_ASSERT_EQUAL_INT64(77, mid);
    cJSON_Delete(root);
}

/* Text of n copies of a UTF-8 unit (malloc'd), optionally followed by a tail. */
static char *repeat(const char *unit, size_t n, const char *tail)
{
    size_t ul = strlen(unit);
    size_t tl = strlen(tail);
    char *s = malloc(ul * n + tl + 1);
    TEST_ASSERT_NOT_NULL(s);
    for (size_t i = 0; i < n; i++) {
        memcpy(s + i * ul, unit, ul);
    }
    memcpy(s + ul * n, tail, tl + 1);
    return s;
}

static void expect_send(tg_status_t want, const char *actor, char *text)
{
    char *json = NULL;
    TEST_ASSERT_EQUAL_STRING(tg_status_name(want),
                             tg_status_name(tg_build_send_message(1, actor, text, 0, &json)));
    TEST_ASSERT_TRUE(want == TG_OK ? json != NULL : json == NULL);
    cJSON_free(json);
    free(text);
}

static void test_send_message_length_limit_in_utf16_units(void)
{
    /* "[A] " is 4 units: 4092 more fit, 4093 do not */
    expect_send(TG_OK, "A", repeat("x", 4092, ""));
    expect_send(TG_ERR_TOO_LONG, "A", repeat("x", 4093, ""));
    /* a 16-char actor takes 19 units, so 4096 bytes of ASCII text are too long */
    expect_send(TG_OK, "abcdefghijklmnop", repeat("x", 4077, ""));
    expect_send(TG_ERR_TOO_LONG, "abcdefghijklmnop", repeat("x", 4096, ""));
    /* astral characters count 2 units each: 2046 emoji + "[A] " = 4096 */
    expect_send(TG_OK, "A", repeat("\xF0\x9F\x98\x80", 2046, ""));
    expect_send(TG_ERR_TOO_LONG, "A", repeat("\xF0\x9F\x98\x80", 2046, "x"));
    /* Cyrillic: 2 bytes but 1 unit each, so 8184 bytes still fit */
    expect_send(TG_OK, "A", repeat("\xD0\x96", 4092, ""));
    expect_send(TG_ERR_TOO_LONG, "A", repeat("\xD0\x96", 4093, ""));
    /* plain text: the whole 4096 units */
    expect_send(TG_OK, NULL, repeat("x", 4096, ""));
    expect_send(TG_ERR_TOO_LONG, NULL, repeat("x", 4097, ""));
    /* malformed UTF-8 is bad_arg even when too long */
    expect_send(TG_ERR_BAD_ARG, "A", repeat("x", 5000, "\xC0\x80"));
}

static void test_send_message_body_escapes_and_empty_text(void)
{
    cJSON *root = build_send(5, "B-2_x", "line1\nquote \" back \\ tab\t", 0);
    TEST_ASSERT_EQUAL_STRING("[B-2_x] line1\nquote \" back \\ tab\t",
                             cJSON_GetObjectItemCaseSensitive(root, "text")->valuestring);
    assert_bold(root, 0, 7);
    cJSON_Delete(root);

    root = build_send(5, "A", "", 0);
    TEST_ASSERT_EQUAL_STRING("[A] ", cJSON_GetObjectItemCaseSensitive(root, "text")->valuestring);
    cJSON_Delete(root);
}

static void test_send_message_body_rejects_bad_args(void)
{
    char *json = (char *)1;
    TEST_ASSERT_EQUAL(TG_ERR_BAD_ARG, tg_build_send_message(1, "", "x", 0, &json));
    TEST_ASSERT_NULL(json);
    TEST_ASSERT_EQUAL(TG_ERR_BAD_ARG, tg_build_send_message(1, "a b", "x", 0, &json));
    TEST_ASSERT_EQUAL(TG_ERR_BAD_ARG, tg_build_send_message(1, "abcdefghijklmnopq", "x", 0, &json));
    TEST_ASSERT_EQUAL(TG_ERR_BAD_ARG, tg_build_send_message(1, NULL, "", 0, &json));
    TEST_ASSERT_EQUAL(TG_ERR_BAD_ARG, tg_build_send_message(1, NULL, NULL, 0, &json));
    TEST_ASSERT_EQUAL(TG_ERR_BAD_ARG, tg_build_send_message(1, "A", NULL, 0, &json));
    TEST_ASSERT_EQUAL(TG_ERR_BAD_ARG, tg_build_send_message(1, NULL, "bad \xED\xA0\x80", 0, &json));
    TEST_ASSERT_EQUAL(TG_ERR_BAD_ARG, tg_build_send_message(1, "A", "bad \xC0\x80", 0, &json));
    TEST_ASSERT_EQUAL(TG_ERR_BAD_ARG, tg_build_send_message(1, "A", "cut \xF0\x9F", 0, &json));
    TEST_ASSERT_EQUAL(TG_ERR_BAD_ARG, tg_build_send_message(0, "A", "x", 0, &json));
    TEST_ASSERT_EQUAL(TG_ERR_BAD_ARG, tg_build_send_message(1, "A", "x", -1, &json));
    TEST_ASSERT_EQUAL(TG_ERR_BAD_ARG,
                      tg_build_send_message(9007199254740992LL, "A", "x", 0, &json));
    TEST_ASSERT_NULL(json);
    TEST_ASSERT_EQUAL(TG_ERR_BAD_ARG, tg_build_send_message(1, "A", "x", 0, NULL));
}

/* ------------------------------------------------------------ getUpdates body */

static void test_get_updates_body(void)
{
    char *s = tg_build_get_updates(861234569, TG_UPDATES_LIMIT, 50);
    TEST_ASSERT_EQUAL_STRING(
        "{\"offset\":861234569,\"limit\":5,\"timeout\":50,\"allowed_updates\":[\"message\"]}", s);
    cJSON_free(s);

    s = tg_build_get_updates(0, 5, 50);
    TEST_ASSERT_EQUAL_STRING("{\"limit\":5,\"timeout\":50,\"allowed_updates\":[\"message\"]}", s);
    cJSON_free(s);

    s = tg_build_get_updates(4503599627370497LL, 1, 0);            /* 2^52 + 1, exact */
    TEST_ASSERT_EQUAL_STRING(
        "{\"offset\":4503599627370497,\"limit\":1,\"timeout\":0,\"allowed_updates\":[\"message\"]}",
        s);
    cJSON_free(s);

    TEST_ASSERT_NULL(tg_build_get_updates(9007199254740992LL, 5, 50));
}

/* ------------------------------------------------------------ responses */

static void test_parse_get_me(void)
{
    tg_resp_t r;
    TEST_ASSERT_EQUAL(TG_OK, parse(200, FX_GETME, &r));
    TEST_ASSERT_EQUAL_INT(200, r.http_status);
    TEST_ASSERT_EQUAL_INT(0, r.error_code);
    char bot[33];
    TEST_ASSERT_TRUE(tg_parse_bot_username(r.result, bot, sizeof(bot)));
    TEST_ASSERT_EQUAL_STRING("embpet_test_bot", bot);
    char tiny[15];
    TEST_ASSERT_FALSE(tg_parse_bot_username(r.result, tiny, sizeof(tiny)));    /* 15 + NUL */
    tg_resp_free(&r);
    TEST_ASSERT_NULL(r.root);
    tg_resp_free(&r);                                                         /* idempotent */

    TEST_ASSERT_EQUAL(TG_OK, parse(200, FX_DELETE_WEBHOOK, &r));
    TEST_ASSERT_TRUE(cJSON_IsTrue(r.result));
    tg_resp_free(&r);
}

static void test_parse_send_message(void)
{
    tg_resp_t r;
    TEST_ASSERT_EQUAL(TG_OK, parse(200, FX_SEND_MESSAGE, &r));
    int64_t mid = 0;
    TEST_ASSERT_TRUE(tg_parse_message_id(r.result, &mid));
    TEST_ASSERT_EQUAL_INT64(1234, mid);
    /* \u escapes and the surrogate pair decode to UTF-8 */
    TEST_ASSERT_EQUAL_STRING("[A] \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xF0\x9F\x98\x80",
                             cJSON_GetObjectItemCaseSensitive(r.result, "text")->valuestring);
    tg_resp_free(&r);
}

static void test_parse_get_updates(void)
{
    tg_resp_t r;
    TEST_ASSERT_EQUAL(TG_OK, parse(200, FX_GET_UPDATES, &r));
    TEST_ASSERT_TRUE(cJSON_IsArray(r.result));
    TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(r.result));
    const cJSON *u0 = cJSON_GetArrayItem(r.result, 0);
    int64_t id = 0;
    TEST_ASSERT_TRUE(tg_json_i64(u0, "update_id", &id));
    TEST_ASSERT_EQUAL_INT64(861234567, id);
    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(u0, "message");
    TEST_ASSERT_EQUAL_STRING("\xD0\xB4\xD0\xB0 \xF0\x9F\x91\x8D",            /* "да 👍" */
                             cJSON_GetObjectItemCaseSensitive(msg, "text")->valuestring);
    const cJSON *rt = cJSON_GetObjectItemCaseSensitive(msg, "reply_to_message");
    TEST_ASSERT_TRUE(tg_parse_message_id(rt, &id));
    TEST_ASSERT_EQUAL_INT64(502, id);
    tg_resp_free(&r);

    TEST_ASSERT_EQUAL(TG_OK, parse(200, FX_EMPTY_UPDATES, &r));
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(r.result));
    tg_resp_free(&r);
}

static void test_parse_auth_errors(void)
{
    tg_resp_t r;
    TEST_ASSERT_EQUAL(TG_ERR_AUTH, parse(401, FX_401, &r));
    TEST_ASSERT_EQUAL_INT(401, r.error_code);
    TEST_ASSERT_EQUAL_STRING("Unauthorized", r.description);
    TEST_ASSERT_NULL(r.root);
    TEST_ASSERT_NULL(r.result);
    TEST_ASSERT_EQUAL_UINT32(0, r.retry_after_s);

    TEST_ASSERT_EQUAL(TG_ERR_AUTH, parse(404, FX_404, &r));
    TEST_ASSERT_EQUAL_INT(404, r.error_code);
    TEST_ASSERT_EQUAL_STRING("Not Found", r.description);

    /* a proxy answering 401 without a Bot API body is still an auth error */
    TEST_ASSERT_EQUAL(TG_ERR_AUTH, parse(401, "Unauthorized", &r));
    TEST_ASSERT_EQUAL_INT(401, r.error_code);
}

static void test_parse_rate_limit(void)
{
    tg_resp_t r;
    TEST_ASSERT_EQUAL(TG_ERR_RETRY, parse(429, FX_429, &r));
    TEST_ASSERT_EQUAL_INT(429, r.error_code);
    TEST_ASSERT_EQUAL_UINT32(17, r.retry_after_s);
    TEST_ASSERT_EQUAL_STRING("Too Many Requests: retry after 17", r.description);

    TEST_ASSERT_EQUAL(TG_ERR_RETRY,
                      parse(429, "{\"ok\":false,\"error_code\":429,\"description\":\"x\"}", &r));
    TEST_ASSERT_EQUAL_UINT32(TG_RETRY_DEFAULT_S, r.retry_after_s);

    TEST_ASSERT_EQUAL(TG_ERR_RETRY, parse(429, "<html>slow down</html>", &r));
    TEST_ASSERT_EQUAL_UINT32(TG_RETRY_DEFAULT_S, r.retry_after_s);

    TEST_ASSERT_EQUAL(TG_ERR_RETRY,
                      parse(429, "{\"ok\":false,\"error_code\":429,\"parameters\":"
                                 "{\"retry_after\":9999999}}", &r));
    TEST_ASSERT_EQUAL_UINT32(86400, r.retry_after_s);

    /* retry_after is only reported for 429 */
    TEST_ASSERT_EQUAL(TG_ERR_API,
                      parse(400, "{\"ok\":false,\"error_code\":400,\"parameters\":"
                                 "{\"retry_after\":3}}", &r));
    TEST_ASSERT_EQUAL_UINT32(0, r.retry_after_s);
}

static void test_parse_api_errors(void)
{
    tg_resp_t r;
    TEST_ASSERT_EQUAL(TG_ERR_API, parse(400, FX_400_CHAT, &r));
    TEST_ASSERT_EQUAL_INT(400, r.error_code);
    TEST_ASSERT_EQUAL_STRING("Bad Request: chat not found", r.description);
    TEST_ASSERT_EQUAL(TG_ERR_API, parse(403, FX_403_BLOCKED, &r));
    TEST_ASSERT_EQUAL_INT(403, r.error_code);
    TEST_ASSERT_EQUAL(TG_ERR_API, parse(409, FX_409_WEBHOOK, &r));
    TEST_ASSERT_EQUAL_INT(409, r.error_code);
    /* error_code missing: the HTTP status stands in */
    TEST_ASSERT_EQUAL(TG_ERR_AUTH, parse(401, "{\"ok\":false}", &r));
    TEST_ASSERT_EQUAL(TG_ERR_SERVER, parse(500, "{\"ok\":false,\"description\":\"x\"}", &r));
    TEST_ASSERT_EQUAL_INT(500, r.error_code);
}

static void test_parse_description_truncated_on_char_boundary(void)
{
    char body[512];
    strcpy(body, "{\"ok\":false,\"error_code\":400,\"description\":\"");
    for (int i = 0; i < 60; i++) {
        strcat(body, "\xD0\x96");                           /* Ж: 120 bytes */
    }
    strcat(body, "\"}");
    tg_resp_t r;
    TEST_ASSERT_EQUAL(TG_ERR_API, parse(400, body, &r));
    size_t n = strlen(r.description);
    TEST_ASSERT_TRUE(n < TG_DESC_SIZE);
    TEST_ASSERT_EQUAL_size_t(94, n);                        /* 47 whole characters */
    TEST_ASSERT_NOT_EQUAL(TG_UTF16_INVALID, tg_utf16_len(r.description, n));
}

static void test_parse_server_and_garbage(void)
{
    tg_resp_t r;
    TEST_ASSERT_EQUAL(TG_ERR_SERVER, parse(502, FX_502_HTML, &r));
    TEST_ASSERT_EQUAL_INT(502, r.error_code);
    TEST_ASSERT_NULL(r.root);
    TEST_ASSERT_EQUAL(TG_ERR_SERVER, parse(200, "not json", &r));
    TEST_ASSERT_EQUAL(TG_ERR_SERVER, parse(200, "", &r));
    TEST_ASSERT_EQUAL(TG_ERR_SERVER, parse(200, "[true]", &r));
    TEST_ASSERT_EQUAL(TG_ERR_SERVER, parse(200, "{\"ok\":\"true\",\"result\":1}", &r));
    TEST_ASSERT_EQUAL(TG_ERR_SERVER, parse(200, "{\"ok\":true}", &r));           /* no result */
    TEST_ASSERT_EQUAL(TG_ERR_SERVER, parse(200, "{\"ok\":true,\"result\":{", &r)); /* cut off */
    TEST_ASSERT_EQUAL(TG_ERR_SERVER, tg_parse_response(0, NULL, 0, TG_RESP_MAX, &r));
}

static void test_parse_size_cap(void)
{
    char *big = malloc(TG_RESP_MAX + 2);
    TEST_ASSERT_NOT_NULL(big);
    static const char head[] = "{\"ok\":true,\"result\":true}";
    memset(big, ' ', TG_RESP_MAX + 1);
    memcpy(big, head, sizeof(head) - 1);
    big[TG_RESP_MAX + 1] = '\0';
    tg_resp_t r;
    /* exactly at the cap: accepted */
    TEST_ASSERT_EQUAL(TG_OK, tg_parse_response(200, big, TG_RESP_MAX, TG_RESP_MAX, &r));
    TEST_ASSERT_TRUE(cJSON_IsTrue(r.result));
    TEST_ASSERT_EQUAL_size_t(TG_RESP_MAX, r.body_len);
    tg_resp_free(&r);
    /* one byte over: rejected without parsing, whatever the content */
    TEST_ASSERT_EQUAL(TG_ERR_TOO_LARGE,
                      tg_parse_response(200, big, TG_RESP_MAX + 1, TG_RESP_MAX, &r));
    TEST_ASSERT_NULL(r.root);
    TEST_ASSERT_EQUAL_INT(200, r.error_code);
    TEST_ASSERT_EQUAL_size_t(TG_RESP_MAX + 1, r.body_len);
    TEST_ASSERT_EQUAL(TG_ERR_TOO_LARGE, tg_parse_response(200, NULL, 1000000, TG_RESP_MAX, &r));
    /* the limit-1 retry cap takes the same body */
    TEST_ASSERT_EQUAL(TG_OK, tg_parse_response(200, big, TG_RESP_MAX + 1, TG_RESP_MAX_ONE, &r));
    tg_resp_free(&r);
    TEST_ASSERT_EQUAL(TG_ERR_TOO_LARGE,
                      tg_parse_response(200, NULL, TG_RESP_MAX_ONE + 1, TG_RESP_MAX_ONE, &r));
    free(big);
}

/* ------------------------------------------------------------ helpers */

static void test_json_i64(void)
{
    cJSON *o = cJSON_Parse("{\"a\":-42,\"b\":1.5,\"c\":\"7\",\"d\":9007199254740992,"
                           "\"e\":9007199254740991,\"f\":1e300}");
    int64_t v = 0;
    TEST_ASSERT_TRUE(tg_json_i64(o, "a", &v));
    TEST_ASSERT_EQUAL_INT64(-42, v);
    TEST_ASSERT_FALSE(tg_json_i64(o, "b", &v));
    TEST_ASSERT_FALSE(tg_json_i64(o, "c", &v));
    TEST_ASSERT_FALSE(tg_json_i64(o, "d", &v));
    TEST_ASSERT_TRUE(tg_json_i64(o, "e", &v));
    TEST_ASSERT_EQUAL_INT64(9007199254740991LL, v);
    TEST_ASSERT_FALSE(tg_json_i64(o, "f", &v));
    TEST_ASSERT_FALSE(tg_json_i64(o, "missing", &v));
    TEST_ASSERT_FALSE(tg_json_i64(NULL, "a", &v));
    cJSON_Delete(o);
}

static void test_updates_first_id(void)
{
    int64_t id = 0;
    /* a body cut off at 120 bytes, as when it exceeds the cap */
    TEST_ASSERT_TRUE(tg_updates_first_id(FX_GET_UPDATES, 120, &id));
    TEST_ASSERT_EQUAL_INT64(861234567, id);
    const char *spaced = "{\"result\":[{\"update_id\" : 12 ,";
    TEST_ASSERT_TRUE(tg_updates_first_id(spaced, strlen(spaced), &id));
    TEST_ASSERT_EQUAL_INT64(12, id);
    /* cut inside the number ("{...\"update_id\":861" is 37 bytes): unknown */
    TEST_ASSERT_FALSE(tg_updates_first_id(FX_GET_UPDATES, 37, &id));
    static const char *const bad[] = {
        "{\"ok\":true,\"result\":[{\"upd",
        "{\"update_id\":x}",
        "{\"update_id\"",
        "{\"update_id\":1234567890123456,",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(tg_updates_first_id(bad[i], strlen(bad[i]), &id), bad[i]);
    }
    TEST_ASSERT_FALSE(tg_updates_first_id(FX_EMPTY_UPDATES, strlen(FX_EMPTY_UPDATES), &id));
    TEST_ASSERT_FALSE(tg_updates_first_id(NULL, 10, &id));
}

/* ------------------------------------------------------------ unescape */

/* Unescape in[0..len) into buf (len + 1 bytes) the way tgapi.c reads a body: the input
 * arrives as a `first`-byte piece, then `step`-byte pieces, each appended after the held-back
 * escape. Returns the output length; buf is NUL-terminated. */
static size_t unesc_feed(const char *in, size_t len, size_t first, size_t step, char *buf)
{
    size_t n = 0;
    size_t rest = 0;
    size_t pos = 0;
    while (pos < len) {
        size_t k = pos == 0 ? first : step;
        if (k > len - pos) {
            k = len - pos;
        }
        TEST_ASSERT_TRUE(n + rest <= pos);      /* never more than was fed */
        memcpy(buf + n + rest, in + pos, k);
        pos += k;
        n += tg_json_unescape(buf + n, rest + k, false, &rest);
        TEST_ASSERT_TRUE(rest <= TG_UNESC_HOLD);
        TEST_ASSERT_TRUE(rest == 0 || buf[n] == '\\');
    }
    n += tg_json_unescape(buf + n, rest, true, &rest);
    TEST_ASSERT_EQUAL_size_t(0, rest);
    buf[n] = '\0';
    return n;
}

/* `in` gives `want` in one piece, split in two at every position, and byte by byte. */
static void check_unesc(const char *in, const char *want)
{
    size_t len = strlen(in);
    char *buf = malloc(len + 1);
    TEST_ASSERT_NOT_NULL(buf);
    unesc_feed(in, len, len, len, buf);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, buf, in);
    for (size_t split = 1; split < len; split++) {
        unesc_feed(in, len, split, len, buf);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(want, buf, in);
    }
    unesc_feed(in, len, 1, 1, buf);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, buf, in);
    free(buf);
}

static void test_unescape_ascii_passthrough(void)
{
    check_unesc("", "");
    check_unesc("{\"ok\":true,\"result\":[]}", "{\"ok\":true,\"result\":[]}");
    check_unesc(FX_GETME, FX_GETME);
    check_unesc("u0041 \\x u00e9", "u0041 \\x u00e9");    /* no escape, or not a \u one */
}

static void test_unescape_bmp(void)
{
    check_unesc("\\u0080", "\xC2\x80");                 /* first 2-byte */
    check_unesc("\\u00e9", "\xC3\xA9");                 /* é */
    check_unesc("\\u00E9", "\xC3\xA9");                 /* upper-case hex */
    check_unesc("\\u07ff", "\xDF\xBF");                 /* last 2-byte */
    check_unesc("\"\\u0434\\u0430\"", "\"\xD0\xB4\xD0\xB0\"");          /* "да" */
    check_unesc("\\u0800", "\xE0\xA0\x80");             /* first 3-byte */
    check_unesc("\\u4e2d\\u6587", "\xE4\xB8\xAD\xE6\x96\x87");          /* 中文 */
    check_unesc("\\u2014", "\xE2\x80\x94");             /* em dash */
    check_unesc("\\ud7ff", "\xED\x9F\xBF");             /* just below the surrogates */
    check_unesc("\\ue000", "\xEE\x80\x80");             /* just above them */
    check_unesc("\\uffff", "\xEF\xBF\xBF");
    check_unesc("a\\u00e9b\\u4e2dc", "a\xC3\xA9" "b\xE4\xB8\xAD" "c");
}

static void test_unescape_surrogate_pairs(void)
{
    check_unesc("\\ud83d\\ude00", "\xF0\x9F\x98\x80");  /* 😀 U+1F600 */
    check_unesc("\\uD83D\\uDE00", "\xF0\x9F\x98\x80");
    check_unesc("\\ud800\\udc00", "\xF0\x90\x80\x80");  /* U+10000, the first */
    check_unesc("\\udbff\\udfff", "\xF4\x8F\xBF\xBF");  /* U+10FFFF, the last */
    /* 👍🏽: two pairs back to back */
    check_unesc("\\ud83d\\udc4d\\ud83c\\udffd", "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD");
    check_unesc("[\\ud83d\\ude00]", "[\xF0\x9F\x98\x80]");
}

static void test_unescape_lone_surrogates_kept(void)
{
    check_unesc("\\ud83d", "\\ud83d");                  /* high at the end */
    check_unesc("\"\\ud83d\"", "\"\\ud83d\"");          /* high, then the string ends */
    check_unesc("\\ud83dx", "\\ud83dx");
    check_unesc("\\ude00", "\\ude00");                  /* low alone */
    check_unesc("\\ude00\\ud83d", "\\ude00\\ud83d");    /* low before high: not a pair */
    check_unesc("\\ud83d\\u00e9", "\\ud83d\xC3\xA9");   /* high, then a non-surrogate */
    check_unesc("\\ud83d\\u0041", "\\ud83d\\u0041");
    check_unesc("\\ud83d\\n", "\\ud83d\\n");            /* high, then another escape */
    check_unesc("\\ud83d\\ud83d\\ude00", "\\ud83d\xF0\x9F\x98\x80");    /* high, then a pair */
    check_unesc("\\udbff\\udbff\\udfff", "\\udbff\xF4\x8F\xBF\xBF");    /* the top high twice */
    check_unesc("\\ud83d\\ue000", "\\ud83d\xEE\x80\x80");               /* just above the lows */
    check_unesc("\\ud83d\\uZZZZ", "\\ud83d\\uZZZZ");    /* high, then a malformed \u */
    check_unesc("\\ud83d\\ude0", "\\ud83d\\ude0");      /* the low cut off by the end */
}

static void test_unescape_invalid_hex_kept(void)
{
    check_unesc("\\u00g9", "\\u00g9");
    check_unesc("\\uzzzz", "\\uzzzz");
    check_unesc("\\u-123", "\\u-123");
    check_unesc("\\u00e", "\\u00e");                    /* cut off by the end */
    check_unesc("\\u", "\\u");
    check_unesc("\\", "\\");
    check_unesc("\\u12\\u00e9", "\\u12\xC3\xA9");       /* the valid escape after it still is */
}

static void test_unescape_ascii_escapes_kept(void)
{
    static const char *const keep[] = {
        "\\\"", "\\\\", "\\/", "\\b", "\\f", "\\n", "\\r", "\\t",
        "\\u0000", "\\u001f", "\\u0022", "\\u002F", "\\u005c", "\\u0041", "\\u007f",
        "{\"t\":\"a\\\"b\\\\c\\/d\\n\\u0000\"}",
    };
    for (size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
        check_unesc(keep[i], keep[i]);
    }
}

static void test_unescape_escaped_backslash_before_u(void)
{
    check_unesc("\\\\u00e9", "\\\\u00e9");              /* text: backslash, "u00e9" */
    check_unesc("\"C:\\\\u4e2d\"", "\"C:\\\\u4e2d\"");
    check_unesc("\\\\\\u00e9", "\\\\\xC3\xA9");         /* escaped backslash, then é */
    check_unesc("\\\\\\\\u00e9", "\\\\\\\\u00e9");      /* two escaped backslashes, then text */
    check_unesc("\\\"\\u00e9", "\\\"\xC3\xA9");
}

static void test_unescape_holds_an_undecided_escape(void)
{
    char buf[32];
    size_t rest = 99;
    strcpy(buf, "ab\\u04");
    TEST_ASSERT_EQUAL_size_t(2, tg_json_unescape(buf, 6, false, &rest));
    TEST_ASSERT_EQUAL_size_t(4, rest);
    TEST_ASSERT_EQUAL_MEMORY("ab\\u04", buf, 6);

    /* a high surrogate waits for a possible low one, up to one hex digit short of it */
    strcpy(buf, "\\u00e9\\ud83d\\ude0");
    TEST_ASSERT_EQUAL_size_t(2, tg_json_unescape(buf, strlen(buf), false, &rest));
    TEST_ASSERT_EQUAL_size_t(TG_UNESC_HOLD, rest);
    TEST_ASSERT_EQUAL_MEMORY("\xC3\xA9\\ud83d\\ude0", buf, 2 + TG_UNESC_HOLD);

    strcpy(buf, "\\ud83d");
    TEST_ASSERT_EQUAL_size_t(0, tg_json_unescape(buf, 6, false, &rest));
    TEST_ASSERT_EQUAL_size_t(6, rest);

    /* decided as soon as the next byte rules a pair out */
    strcpy(buf, "\\ud83d\"");
    TEST_ASSERT_EQUAL_size_t(7, tg_json_unescape(buf, 7, false, &rest));
    TEST_ASSERT_EQUAL_size_t(0, rest);
    strcpy(buf, "x\\");
    TEST_ASSERT_EQUAL_size_t(1, tg_json_unescape(buf, 2, false, &rest));
    TEST_ASSERT_EQUAL_size_t(1, rest);
    TEST_ASSERT_EQUAL_size_t(1, tg_json_unescape(buf + 1, 1, true, &rest));   /* the end */
    TEST_ASSERT_EQUAL_size_t(0, rest);
    TEST_ASSERT_EQUAL_MEMORY("x\\", buf, 2);
}

/* Parse body before and after unescaping: the same tree, every string identical. */
static void check_same_json(const char *body, const char *must_contain)
{
    size_t len = strlen(body);
    char *buf = malloc(len + 1);
    TEST_ASSERT_NOT_NULL(buf);
    size_t n = unesc_feed(body, len, len, len, buf);
    TEST_ASSERT_TRUE(n <= len && 3 * n >= len);
    cJSON *a = cJSON_ParseWithLength(body, len);
    cJSON *b = cJSON_ParseWithLength(buf, n);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_TRUE(cJSON_Compare(a, b, true));
    if (must_contain != NULL) {
        TEST_ASSERT_NOT_NULL(strstr(buf, must_contain));
    }
    for (size_t split = 1; split < len; split += 7) {
        char *again = malloc(len + 1);
        TEST_ASSERT_NOT_NULL(again);
        TEST_ASSERT_EQUAL_size_t(n, unesc_feed(body, len, split, 512, again));
        TEST_ASSERT_EQUAL_STRING(buf, again);
        free(again);
    }
    cJSON_Delete(a);
    cJSON_Delete(b);
    free(buf);
}

static void test_unescape_get_updates_same_strings(void)
{
    check_same_json(FX_GET_UPDATES, "\xD0\xB4\xD0\xB0 \xF0\x9F\x91\x8D");     /* да 👍 */
    check_same_json(FX_SEND_MESSAGE, "[A] \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 ");
    check_same_json(FX_GET_UPDATES_INTL, "C:\\\\u00e9");    /* the literal stays escaped */
    check_same_json(FX_429, NULL);

    size_t len = strlen(FX_GET_UPDATES_INTL);
    char *buf = malloc(len + 1);
    TEST_ASSERT_NOT_NULL(buf);
    size_t n = unesc_feed(FX_GET_UPDATES_INTL, len, len, len, buf);
    tg_resp_t r;
    TEST_ASSERT_EQUAL(TG_OK, tg_parse_response(200, buf, n, TG_RESP_MAX, &r));
    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(r.result, 0),
                                                        "message");
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(msg, "text");
    TEST_ASSERT_TRUE(cJSON_IsString(text));
    /* "да, почисти — 好的 👍" */
    TEST_ASSERT_EQUAL_STRING("\xD0\xB4\xD0\xB0, \xD0\xBF\xD0\xBE\xD1\x87\xD0\xB8\xD1\x81\xD1\x82"
                             "\xD0\xB8 \xE2\x80\x94 \xE5\xA5\xBD\xE7\x9A\x84 \xF0\x9F\x91\x8D",
                             text->valuestring);
    const cJSON *quoted = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(msg, "reply_to_message"), "text");
    TEST_ASSERT_TRUE(cJSON_IsString(quoted));
    TEST_ASSERT_EQUAL_STRING("[C] \xE4\xB8\xAD\xE6\x96\x87 \"q\" C:\\u00e9 a/b\n"
                             "\xF0\x9F\x98\x80\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD caf\xC3\xA9",
                             quoted->valuestring);
    tg_resp_free(&r);
    free(buf);
}

/* Copy s to p; returns the end of the copy (not NUL-terminated). */
static char *put(char *p, const char *s)
{
    size_t n = strlen(s);
    memcpy(p, s, n);
    return p + n;
}

/* The limit-1 worst case (docs/PLAN.md 1.2): an owner reply of 4096 CJK units quoting a bot
 * message of 4096 CJK units is ~50 KB escaped, ~25 KB unescaped, within TG_RESP_MAX_ONE. */
static void test_unescape_worst_case_fits_cap_one(void)
{
    static const char head[] =
        "{\"ok\":true,\"result\":[{\"update_id\":861234570,\n\"message\":{\"message_id\":508,"
        "\"from\":{\"id\":111222333,\"is_bot\":false,\"first_name\":\"Owner\"},"
        "\"chat\":{\"id\":111222333,\"first_name\":\"Owner\",\"type\":\"private\"},"
        "\"date\":1760450160,\"reply_to_message\":{\"message_id\":507,\"from\":{\"id\":"
        "7123456789,\"is_bot\":true,\"first_name\":\"embpet\",\"username\":"
        "\"embpet_test_bot\"},\"chat\":{\"id\":111222333,\"first_name\":\"Owner\","
        "\"type\":\"private\"},\"date\":1760450100,\"text\":\"[A] ";
    static const char mid[] =
        "\",\"entities\":[{\"offset\":0,\"length\":3,\"type\":\"bold\"}]},\"text\":\"";
    static const char tail[] = "\"}}]}";
    const size_t units = TG_TEXT_MAX_UTF16;
    size_t len = strlen(head) + (units - 4) * 6 + strlen(mid) + units * 6 + strlen(tail);
    char *body = malloc(len + 1);
    TEST_ASSERT_NOT_NULL(body);
    char *p = put(body, head);
    for (size_t i = 0; i < units - 4; i++) {
        p = put(p, "\\u4e2d");
    }
    p = put(p, mid);
    for (size_t i = 0; i < units; i++) {
        p = put(p, "\\u6587");
    }
    p = put(p, tail);
    *p = '\0';
    TEST_ASSERT_EQUAL_size_t(len, (size_t)(p - body));
    TEST_ASSERT_TRUE(len > 3 * TG_RESP_MAX_ONE / 2);

    char *buf = malloc(len + 1);
    TEST_ASSERT_NOT_NULL(buf);
    size_t n = unesc_feed(body, len, 4096, 4096, buf);
    TEST_ASSERT_TRUE(n < 27000 && n <= TG_RESP_MAX_ONE);
    tg_resp_t r;
    TEST_ASSERT_EQUAL(TG_OK, tg_parse_response(200, buf, n, TG_RESP_MAX_ONE, &r));
    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(r.result, 0),
                                                        "message");
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(msg, "text");
    TEST_ASSERT_TRUE(cJSON_IsString(text));
    TEST_ASSERT_EQUAL_size_t(units, tg_utf16_len(text->valuestring, strlen(text->valuestring)));
    tg_resp_free(&r);
    free(buf);
    free(body);
}

/* ------------------------------------------------------------ reading a body */

/* A fake transport: data[0..len) arrives in pieces of at most `step` bytes; once `fail_at`
 * bytes have been read, a read fails. */
typedef struct {
    const char *data;
    size_t len;
    size_t step;
    size_t fail_at;
    size_t pos;
    unsigned calls;
    unsigned after_end;         /* reads after one returned 0 or an error */
    bool ended;
} fake_body_t;

static int fake_read(void *ctx, char *buf, size_t cap)
{
    fake_body_t *f = ctx;
    TEST_ASSERT_TRUE(cap >= 1);
    f->calls++;
    if (f->ended) {
        f->after_end++;
    }
    if (f->pos >= f->fail_at) {
        f->ended = true;
        return -1;
    }
    size_t k = f->len - f->pos;
    k = k < f->step ? k : f->step;
    k = k < cap ? k : cap;
    k = k < f->fail_at - f->pos ? k : f->fail_at - f->pos;
    if (k == 0) {
        f->ended = true;
        return 0;
    }
    memcpy(buf, f->data + f->pos, k);
    f->pos += k;
    return (int)k;
}

static tg_status_t read_fake(fake_body_t *f, const char *data, size_t len, int64_t clen,
                             size_t max_len, size_t step, tg_body_t *b)
{
    memset(f, 0, sizeof(*f));
    f->data = data;
    f->len = len;
    f->step = step;
    f->fail_at = SIZE_MAX;
    return tg_read_body(clen, max_len, fake_read, f, b);
}

/* What tg_json_unescape makes of data[0..len), in a malloc'd buffer. */
static char *unescaped(const char *data, size_t len, size_t *n)
{
    char *want = malloc(len + 1);
    TEST_ASSERT_NOT_NULL(want);
    *n = unesc_feed(data, len, len, len, want);
    return want;
}

/* A whole body that fits: read with and without Content-Length, in pieces of every size in
 * steps[], gives the unescaped body, and no read follows the end. */
static void check_body_fits(const char *data, size_t max_len)
{
    static const size_t steps[] = {1, 2, 5, 7, 11, 12, 13, 64, 512, SIZE_MAX};
    size_t len = strlen(data);
    size_t n;
    char *want = unescaped(data, len, &n);
    TEST_ASSERT_TRUE(n <= max_len);
    for (size_t s = 0; s < sizeof(steps) / sizeof(steps[0]); s++) {
        for (int with_clen = 0; with_clen < 2; with_clen++) {
            fake_body_t f;
            tg_body_t b;
            TEST_ASSERT_EQUAL(TG_OK, read_fake(&f, data, len, with_clen ? (int64_t)len : 0,
                                               max_len, steps[s], &b));
            TEST_ASSERT_EQUAL_size_t(n, b.len);
            TEST_ASSERT_EQUAL_size_t(len, b.wire);
            TEST_ASSERT_EQUAL_size_t(len, f.pos);
            TEST_ASSERT_EQUAL_UINT(0, f.after_end);
            TEST_ASSERT_NOT_NULL(b.buf);
            TEST_ASSERT_TRUE(n == 0 || memcmp(want, b.buf, n) == 0);
            free(b.buf);
        }
    }
    free(want);
}

/* `count` copies of `unit` between quotes. */
static char *repeat_quoted(const char *unit, size_t count)
{
    size_t u = strlen(unit);
    char *s = malloc(u * count + 3);
    TEST_ASSERT_NOT_NULL(s);
    char *p = s;
    *p++ = '"';
    for (size_t i = 0; i < count; i++) {
        memcpy(p, unit, u);
        p += u;
    }
    *p++ = '"';
    *p = '\0';
    return s;
}

static void test_read_body_fits(void)
{
    check_body_fits(FX_GET_UPDATES_INTL, TG_RESP_MAX);
    check_body_fits(FX_GET_UPDATES, TG_RESP_MAX);
    check_body_fits("", TG_RESP_MAX);
    check_body_fits("{}", 2);
}

static void test_read_body_over_cap_on_wire_fits_unescaped(void)
{
    /* 99 x 中: 596 bytes on the wire, 299 unescaped */
    char *s = repeat_quoted("\\u4e2d", 99);
    size_t len = strlen(s);
    check_body_fits(s, 299);                    /* exactly max_len after unescaping */
    check_body_fits(s, 300);

    /* max_len + 1 after unescaping: over the cap, with or without Content-Length */
    size_t n;
    char *want = unescaped(s, len, &n);
    TEST_ASSERT_EQUAL_size_t(299, n);
    for (size_t step = 1; step <= len; step += step < 16 ? 1 : 97) {
        for (int with_clen = 0; with_clen < 2; with_clen++) {
            fake_body_t f;
            tg_body_t b;
            TEST_ASSERT_EQUAL(TG_ERR_TOO_LARGE,
                              read_fake(&f, s, len, with_clen ? (int64_t)len : 0, 298, step, &b));
            TEST_ASSERT_NOT_NULL(b.buf);
            TEST_ASSERT_TRUE(b.len > 298 && b.len <= n);
            TEST_ASSERT_EQUAL_MEMORY(want, b.buf, b.len);
            TEST_ASSERT_EQUAL_size_t(with_clen ? len : f.pos, b.wire);
            free(b.buf);
        }
    }
    free(want);
    free(s);
}

static void test_read_body_escape_held_at_buffer_end(void)
{
    /* max_len 20: a 32-byte buffer. The first 32 bytes end inside the surrogate pair, which
     * is held back while the next read refills the space unescaping freed. */
    check_body_fits("\\u00e9\\u00e9\\u00e9aaaaaaaa\\ud83d\\ude00", 20);     /* 38 -> 18 */
    check_body_fits("\\u00e9\\u00e9\\u00e9aaaaaaaaaa\\ud83d\\ude00", 20);   /* 40 -> 20 */
    check_body_fits("\\u00e9\\u00e9\\u00e9aaaaaaaaa\\u00e9", 20);   /* 33 -> 17, escape at 27 */
}

static void test_read_body_content_length_mismatch(void)
{
    const char *d = FX_GET_UPDATES_INTL;
    size_t len = strlen(d);
    size_t n;
    char *want = unescaped(d, len, &n);
    fake_body_t f;
    tg_body_t b;

    /* larger than the body (the connection ends early): what arrived, the transport tells
     * the body is incomplete */
    TEST_ASSERT_EQUAL(TG_OK, read_fake(&f, d, len, (int64_t)len + 100, TG_RESP_MAX, 64, &b));
    TEST_ASSERT_EQUAL_size_t(n, b.len);
    TEST_ASSERT_EQUAL_MEMORY(want, b.buf, n);
    TEST_ASSERT_EQUAL_size_t(len + 100, b.wire);
    TEST_ASSERT_TRUE(f.ended);
    TEST_ASSERT_EQUAL_UINT(0, f.after_end);
    free(b.buf);
    free(want);

    /* smaller: never more than Content-Length is read, also when it ends inside an escape */
    const char *esc = strstr(d, "\\ud83d\\ude00");
    TEST_ASSERT_NOT_NULL(esc);
    const size_t cuts[] = {len - 3, (size_t)(esc - d) + 3, (size_t)(esc - d) + 8, 1};
    for (size_t i = 0; i < sizeof(cuts) / sizeof(cuts[0]); i++) {
        for (size_t step = 1; step <= 4096; step *= 8) {
            want = unescaped(d, cuts[i], &n);
            TEST_ASSERT_EQUAL(TG_OK,
                              read_fake(&f, d, len, (int64_t)cuts[i], TG_RESP_MAX, step, &b));
            TEST_ASSERT_EQUAL_size_t(cuts[i], f.pos);
            TEST_ASSERT_EQUAL_size_t(n, b.len);
            TEST_ASSERT_EQUAL_MEMORY(want, b.buf, n);
            free(b.buf);
            free(want);
        }
    }
}

static void test_read_body_read_error_or_end(void)
{
    const char *d = FX_GET_UPDATES_INTL;
    size_t len = strlen(d);
    fake_body_t f;
    tg_body_t b;
    /* an error after 50 bytes (inside "В"...): what arrived, no read after the error */
    const char *esc = strstr(d, "\\u0412");
    TEST_ASSERT_NOT_NULL(esc);
    const size_t at[] = {50, (size_t)(esc - d) + 4, 0};
    for (size_t i = 0; i < sizeof(at) / sizeof(at[0]); i++) {
        for (int with_clen = 0; with_clen < 2; with_clen++) {
            size_t n;
            char *want = unescaped(d, at[i], &n);
            memset(&f, 0, sizeof(f));
            f.data = d;
            f.len = len;
            f.step = 7;
            f.fail_at = at[i];
            TEST_ASSERT_EQUAL(TG_OK, tg_read_body(with_clen ? (int64_t)len : 0, TG_RESP_MAX,
                                                  fake_read, &f, &b));
            TEST_ASSERT_EQUAL_size_t(n, b.len);
            TEST_ASSERT_TRUE(n == 0 || memcmp(want, b.buf, n) == 0);
            TEST_ASSERT_EQUAL_size_t(with_clen ? len : at[i], b.wire);
            TEST_ASSERT_EQUAL_UINT(0, f.after_end);
            free(b.buf);
            free(want);
        }
    }
    /* an empty body: one read, which returns 0 */
    TEST_ASSERT_EQUAL(TG_OK, read_fake(&f, "", 0, 0, TG_RESP_MAX, 64, &b));
    TEST_ASSERT_EQUAL_size_t(0, b.len);
    TEST_ASSERT_EQUAL_UINT(1, f.calls);
    free(b.buf);
}

static void test_read_body_too_large(void)
{
    fake_body_t f;
    tg_body_t b;
    /* Content-Length over 3 x max_len: not read at all */
    TEST_ASSERT_EQUAL(TG_ERR_TOO_LARGE, read_fake(&f, "", 0, 181, 60, 64, &b));
    TEST_ASSERT_NULL(b.buf);
    TEST_ASSERT_EQUAL_size_t(0, b.len);
    TEST_ASSERT_EQUAL_size_t(181, b.wire);
    TEST_ASSERT_EQUAL_UINT(0, f.calls);
    TEST_ASSERT_EQUAL(TG_ERR_TOO_LARGE,
                      read_fake(&f, "", 0, 3 * (int64_t)TG_RESP_MAX_ONE + 1, TG_RESP_MAX_ONE,
                                64, &b));
    TEST_ASSERT_EQUAL_UINT(0, f.calls);

    /* exactly 3 x max_len can still fit: 30 x é is 180 bytes, 60 unescaped */
    char *s = repeat_quoted("\\u00e9", 30);
    size_t len = strlen(s);                     /* 182 with the quotes */
    TEST_ASSERT_EQUAL(TG_OK, read_fake(&f, s + 1, len - 2, (int64_t)len - 2, 60, 512, &b));
    TEST_ASSERT_EQUAL_size_t(60, b.len);
    free(b.buf);
    free(s);

    /* plain ASCII over the cap: stops one byte past it, never reads the whole body */
    char big[200];
    memset(big, 'a', sizeof(big));
    for (int with_clen = 0; with_clen < 2; with_clen++) {
        TEST_ASSERT_EQUAL(TG_ERR_TOO_LARGE, read_fake(&f, big, sizeof(big),
                                                      with_clen ? (int64_t)sizeof(big) : 0,
                                                      100, 512, &b));
        TEST_ASSERT_TRUE(b.len > 100 && b.len <= 100 + 1 + TG_UNESC_HOLD);
        TEST_ASSERT_TRUE(f.pos <= 100 + 1 + TG_UNESC_HOLD);
        TEST_ASSERT_EQUAL_UINT(0, f.after_end);
        free(b.buf);
    }
}

static void test_retry_delay(void)
{
    TEST_ASSERT_EQUAL_UINT32(17000, tg_retry_delay_ms(TG_ERR_RETRY, 17, 0));
    TEST_ASSERT_EQUAL_UINT32(1000, tg_retry_delay_ms(TG_ERR_RETRY, 0, 5));
    TEST_ASSERT_EQUAL_UINT32(86400000u, tg_retry_delay_ms(TG_ERR_RETRY, 4000000000u, 0));
    static const uint32_t want[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000};
    for (unsigned a = 0; a < sizeof(want) / sizeof(want[0]); a++) {
        TEST_ASSERT_EQUAL_UINT32(want[a], tg_retry_delay_ms(TG_ERR_NET, 0, a));
        TEST_ASSERT_EQUAL_UINT32(want[a], tg_retry_delay_ms(TG_ERR_TIMEOUT, 0, a));
        TEST_ASSERT_EQUAL_UINT32(want[a], tg_retry_delay_ms(TG_ERR_SERVER, 0, a));
    }
    TEST_ASSERT_EQUAL_UINT32(30000, tg_retry_delay_ms(TG_ERR_NET, 0, 4000000000u));
    TEST_ASSERT_EQUAL_UINT32(TG_AUTH_RETRY_MS, tg_retry_delay_ms(TG_ERR_AUTH, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(0, tg_retry_delay_ms(TG_OK, 5, 3));
    TEST_ASSERT_EQUAL_UINT32(0, tg_retry_delay_ms(TG_ERR_API, 5, 3));
    TEST_ASSERT_EQUAL_UINT32(0, tg_retry_delay_ms(TG_ERR_TOO_LARGE, 0, 0));
}

static void test_status_names(void)
{
    TEST_ASSERT_EQUAL_STRING("ok", tg_status_name(TG_OK));
    TEST_ASSERT_EQUAL_STRING("auth", tg_status_name(TG_ERR_AUTH));
    TEST_ASSERT_EQUAL_STRING("too_large", tg_status_name(TG_ERR_TOO_LARGE));
    TEST_ASSERT_EQUAL_STRING("no_mem", tg_status_name(TG_ERR_NO_MEM));
    TEST_ASSERT_EQUAL_STRING("too_long", tg_status_name(TG_ERR_TOO_LONG));
    TEST_ASSERT_EQUAL_STRING("?", tg_status_name((tg_status_t)99));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_utf16_len_counts_code_units);
    RUN_TEST(test_utf16_len_rejects_malformed);
    RUN_TEST(test_utf16_span_for_tag);
    RUN_TEST(test_utf16_span_rejects_bad_ranges);
    RUN_TEST(test_send_message_body_emoji_cyrillic);
    RUN_TEST(test_send_message_body_long_actor_and_reply);
    RUN_TEST(test_send_message_body_escapes_and_empty_text);
    RUN_TEST(test_send_message_body_plain_text);
    RUN_TEST(test_send_message_length_limit_in_utf16_units);
    RUN_TEST(test_send_message_body_rejects_bad_args);
    RUN_TEST(test_get_updates_body);
    RUN_TEST(test_parse_get_me);
    RUN_TEST(test_parse_send_message);
    RUN_TEST(test_parse_get_updates);
    RUN_TEST(test_parse_auth_errors);
    RUN_TEST(test_parse_rate_limit);
    RUN_TEST(test_parse_api_errors);
    RUN_TEST(test_parse_description_truncated_on_char_boundary);
    RUN_TEST(test_parse_server_and_garbage);
    RUN_TEST(test_parse_size_cap);
    RUN_TEST(test_json_i64);
    RUN_TEST(test_updates_first_id);
    RUN_TEST(test_unescape_ascii_passthrough);
    RUN_TEST(test_unescape_bmp);
    RUN_TEST(test_unescape_surrogate_pairs);
    RUN_TEST(test_unescape_lone_surrogates_kept);
    RUN_TEST(test_unescape_invalid_hex_kept);
    RUN_TEST(test_unescape_ascii_escapes_kept);
    RUN_TEST(test_unescape_escaped_backslash_before_u);
    RUN_TEST(test_unescape_holds_an_undecided_escape);
    RUN_TEST(test_unescape_get_updates_same_strings);
    RUN_TEST(test_unescape_worst_case_fits_cap_one);
    RUN_TEST(test_read_body_fits);
    RUN_TEST(test_read_body_over_cap_on_wire_fits_unescaped);
    RUN_TEST(test_read_body_escape_held_at_buffer_end);
    RUN_TEST(test_read_body_content_length_mismatch);
    RUN_TEST(test_read_body_read_error_or_end);
    RUN_TEST(test_read_body_too_large);
    RUN_TEST(test_retry_delay);
    RUN_TEST(test_status_names);
    return UNITY_END();
}
