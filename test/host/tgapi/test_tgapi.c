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
    RUN_TEST(test_retry_delay);
    RUN_TEST(test_status_names);
    return UNITY_END();
}
