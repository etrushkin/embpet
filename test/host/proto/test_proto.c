#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto_core.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define N EMBPET_EVT_RING

typedef struct {
    uint32_t seq[N + 8];
    size_t n;
} seen_t;

static void visit(uint32_t seq, const char *line, size_t len, void *ctx)
{
    (void)line;
    (void)len;
    seen_t *s = ctx;
    s->seq[s->n++] = seq;
}

static void push_seq(proto_ring_t *r, uint32_t seq)
{
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "\n@@{\"seq\":%u}\n", (unsigned)seq);
    char *line = malloc((size_t)n + 1);
    memcpy(line, buf, (size_t)n + 1);
    proto_ring_push(r, seq, line, (size_t)n);
}

static void test_empty_ring(void)
{
    proto_ring_t r;
    proto_ring_init(&r);
    seen_t s = {0};
    bool trunc = true;
    TEST_ASSERT_EQUAL_UINT(0, proto_ring_since(&r, 0, visit, &s, &trunc));
    TEST_ASSERT_FALSE(trunc);
}

static void test_since_filters_and_orders(void)
{
    proto_ring_t r;
    proto_ring_init(&r);
    for (uint32_t i = 1; i <= 5; i++) {
        push_seq(&r, i * 2);                     /* gaps: res frames share the counter */
    }
    seen_t s = {0};
    bool trunc = true;
    TEST_ASSERT_EQUAL_UINT(3, proto_ring_since(&r, 4, visit, &s, &trunc));
    TEST_ASSERT_FALSE(trunc);
    TEST_ASSERT_EQUAL_UINT32(6, s.seq[0]);
    TEST_ASSERT_EQUAL_UINT32(8, s.seq[1]);
    TEST_ASSERT_EQUAL_UINT32(10, s.seq[2]);
    s.n = 0;
    TEST_ASSERT_EQUAL_UINT(5, proto_ring_since(&r, 0, visit, &s, NULL));
    s.n = 0;
    TEST_ASSERT_EQUAL_UINT(0, proto_ring_since(&r, 10, visit, &s, &trunc));
    proto_ring_clear(&r);
}

static void test_exactly_full_not_truncated(void)
{
    proto_ring_t r;
    proto_ring_init(&r);
    for (uint32_t i = 1; i <= N; i++) {
        push_seq(&r, i);
    }
    seen_t s = {0};
    bool trunc = true;
    TEST_ASSERT_EQUAL_UINT(N, proto_ring_since(&r, 0, visit, &s, &trunc));
    TEST_ASSERT_FALSE(trunc);
    proto_ring_clear(&r);
}

static void test_wraparound_keeps_last_n(void)
{
    proto_ring_t r;
    proto_ring_init(&r);
    const uint32_t total = N * 2 + 5;
    for (uint32_t i = 1; i <= total; i++) {
        push_seq(&r, i);
    }
    seen_t s = {0};
    bool trunc = false;
    TEST_ASSERT_EQUAL_UINT(N, proto_ring_since(&r, 0, visit, &s, &trunc));
    TEST_ASSERT_TRUE(trunc);
    for (size_t i = 0; i < N; i++) {
        TEST_ASSERT_EQUAL_UINT32(total - N + 1 + (uint32_t)i, s.seq[i]);
    }
    proto_ring_clear(&r);
}

static void test_since_older_than_ring_is_truncated(void)
{
    proto_ring_t r;
    proto_ring_init(&r);
    for (uint32_t i = 1; i <= N + 3; i++) {      /* seqs 1..3 evicted */
        push_seq(&r, i);
    }
    seen_t s = {0};
    bool trunc = false;
    /* host has seen up to seq 1: events 2 and 3 are lost */
    TEST_ASSERT_EQUAL_UINT(N, proto_ring_since(&r, 1, visit, &s, &trunc));
    TEST_ASSERT_TRUE(trunc);
    TEST_ASSERT_EQUAL_UINT32(4, s.seq[0]);
    /* host has seen everything evicted: nothing lost */
    s.n = 0;
    TEST_ASSERT_EQUAL_UINT(N, proto_ring_since(&r, 3, visit, &s, &trunc));
    TEST_ASSERT_FALSE(trunc);
    proto_ring_clear(&r);
}

static void test_lines_are_verbatim(void)
{
    proto_ring_t r;
    proto_ring_init(&r);
    size_t len = 0;
    char *line = proto_core_render_evt(7, "deadbeef", "pending", cJSON_Parse("{\"count\":2}"), &len);
    TEST_ASSERT_NOT_NULL(line);
    char *copy = malloc(len + 1);
    memcpy(copy, line, len + 1);
    proto_ring_push(&r, 7, line, len);
    TEST_ASSERT_EQUAL_STRING(copy, r.s[r.head].line);
    TEST_ASSERT_EQUAL_UINT(len, r.s[r.head].len);
    free(copy);
    proto_ring_clear(&r);
}

static void test_render_evt(void)
{
    size_t len = 0;
    cJSON *f = cJSON_Parse("{\"state\":\"up\",\"t\":\"hack\",\"rssi\":-40}");
    char *line = proto_core_render_evt(3, "9f3a11c2", "wifi", f, &len);
    TEST_ASSERT_NOT_NULL(line);
    TEST_ASSERT_EQUAL_STRING("\n@@{\"t\":\"evt\",\"seq\":3,\"boot\":\"9f3a11c2\",\"ev\":\"wifi\","
                             "\"state\":\"up\",\"rssi\":-40}\n", line);
    TEST_ASSERT_EQUAL_UINT(strlen(line), len);
    free(line);
}

static void test_render_evt_null_fields(void)
{
    size_t len = 0;
    char *line = proto_core_render_evt(1, "00000001", "boot", NULL, &len);
    TEST_ASSERT_EQUAL_STRING("\n@@{\"t\":\"evt\",\"seq\":1,\"boot\":\"00000001\",\"ev\":\"boot\"}\n", line);
    free(line);
}

static void test_render_res_ok(void)
{
    proto_ctx_t ctx = {.cmd = "tg send", .rid = "7", .actor = "A"};
    size_t len = 0;
    char *line = proto_core_render_res(15, "9f3a11c2", &ctx, NULL, NULL,
                                       cJSON_Parse("{\"msg\":1234}"), &len);
    TEST_ASSERT_EQUAL_STRING("\n@@{\"t\":\"res\",\"seq\":15,\"boot\":\"9f3a11c2\",\"cmd\":\"tg send\","
                             "\"rid\":\"7\",\"actor\":\"A\",\"ok\":true,\"msg\":1234}\n", line);
    free(line);
}

static void test_render_res_err_no_actor(void)
{
    proto_ctx_t ctx = {.cmd = "io set", .rid = "8", .actor = NULL};
    size_t len = 0;
    char *line = proto_core_render_res(16, "9f3a11c2", &ctx, "bad_pin", "pin 9 is not an output",
                                       NULL, &len);
    TEST_ASSERT_EQUAL_STRING("\n@@{\"t\":\"res\",\"seq\":16,\"boot\":\"9f3a11c2\",\"cmd\":\"io set\","
                             "\"rid\":\"8\",\"ok\":false,\"err\":\"bad_pin\","
                             "\"msg\":\"pin 9 is not an output\"}\n", line);
    free(line);
}

static void test_render_escapes_text(void)
{
    cJSON *f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "text", "a\"b\\c\nd \xc3\xa9");
    size_t len = 0;
    char *line = proto_core_render_evt(2, "00000001", "reply", f, &len);
    /* one physical line between the framing newlines, valid JSON */
    TEST_ASSERT_EQUAL_PTR(line + len - 1, strchr(line + 1, '\n'));
    cJSON *back = cJSON_Parse(line + 3);
    TEST_ASSERT_NOT_NULL(back);
    TEST_ASSERT_EQUAL_STRING("a\"b\\c\nd \xc3\xa9", cJSON_GetObjectItem(back, "text")->valuestring);
    cJSON_Delete(back);
    free(line);
}

static void push_len(proto_ring_t *r, uint32_t seq, size_t len)
{
    char *line = malloc(len + 1);
    memset(line, 'x', len);
    line[len] = '\0';
    proto_ring_push(r, seq, line, len);
}

static size_t stored_bytes(const proto_ring_t *r)
{
    size_t sum = 0;
    for (size_t i = 0; i < r->count; i++) {
        sum += r->s[(r->head + i) % EMBPET_EVT_RING].len;
    }
    return sum;
}

static void test_byte_cap_evicts_oldest(void)
{
    proto_ring_t r;
    proto_ring_init(&r);
    const size_t each = EMBPET_EVT_RING_BYTES / 16;      /* 16 lines fill the cap exactly */
    for (uint32_t i = 1; i <= 20; i++) {
        push_len(&r, i, each);
    }
    TEST_ASSERT_EQUAL_UINT(16, r.count);
    TEST_ASSERT_EQUAL_UINT(16 * each, r.bytes);
    TEST_ASSERT_TRUE(r.bytes <= EMBPET_EVT_RING_BYTES);
    TEST_ASSERT_EQUAL_UINT32(4, r.evicted_seq);
    seen_t s = {0};
    bool trunc = false;
    TEST_ASSERT_EQUAL_UINT(16, proto_ring_since(&r, 0, visit, &s, &trunc));
    TEST_ASSERT_TRUE(trunc);
    TEST_ASSERT_EQUAL_UINT32(5, s.seq[0]);
    proto_ring_clear(&r);
}

static void test_big_line_evicts_several(void)
{
    proto_ring_t r;
    proto_ring_init(&r);
    for (uint32_t i = 1; i <= 4; i++) {
        push_len(&r, i, 1000);
    }
    push_len(&r, 5, EMBPET_EVT_RING_BYTES - 1500);       /* must push out seqs 1..3 */
    TEST_ASSERT_EQUAL_UINT(2, r.count);
    TEST_ASSERT_EQUAL_UINT32(3, r.evicted_seq);
    TEST_ASSERT_EQUAL_UINT(stored_bytes(&r), r.bytes);
    proto_ring_clear(&r);
}

static void test_oversized_line_not_stored_and_truncated(void)
{
    proto_ring_t r;
    proto_ring_init(&r);
    push_len(&r, 1, 100);
    push_len(&r, 2, EMBPET_EVT_RING_BYTES + 1);
    TEST_ASSERT_EQUAL_UINT(1, r.count);
    TEST_ASSERT_EQUAL_UINT(100, r.bytes);
    TEST_ASSERT_EQUAL_UINT32(2, r.evicted_seq);
    seen_t s = {0};
    bool trunc = false;
    TEST_ASSERT_EQUAL_UINT(0, proto_ring_since(&r, 1, visit, &s, &trunc));
    TEST_ASSERT_TRUE(trunc);                              /* host missed seq 2 */
    TEST_ASSERT_EQUAL_UINT(0, proto_ring_since(&r, 2, visit, &s, &trunc));
    TEST_ASSERT_FALSE(trunc);
    /* evicting seq 1 later must not move evicted_seq back below 2 */
    for (uint32_t i = 3; i <= 3 + N; i++) {
        push_len(&r, i, 10);
    }
    TEST_ASSERT_TRUE(r.evicted_seq >= 2);
    s.n = 0;
    proto_ring_since(&r, 1, visit, &s, &trunc);
    TEST_ASSERT_TRUE(trunc);
    proto_ring_clear(&r);
}

static void test_bytes_accounting_across_wraparound(void)
{
    proto_ring_t r;
    proto_ring_init(&r);
    uint32_t prev_evicted = 0;
    for (uint32_t i = 1; i <= N * 3 + 1; i++) {
        push_len(&r, i, 50 + (i % 7) * 40);
        TEST_ASSERT_TRUE(r.evicted_seq >= prev_evicted);
        prev_evicted = r.evicted_seq;
        TEST_ASSERT_EQUAL_UINT(stored_bytes(&r), r.bytes);
        TEST_ASSERT_TRUE(r.bytes <= EMBPET_EVT_RING_BYTES);
    }
    proto_ring_clear(&r);
    TEST_ASSERT_EQUAL_UINT(0, r.bytes);
    TEST_ASSERT_EQUAL_UINT(0, r.count);
}

static void test_since_newer_than_newest_after_eviction(void)
{
    proto_ring_t r;
    proto_ring_init(&r);
    for (uint32_t i = 1; i <= N + 5; i++) {
        push_seq(&r, i);
    }
    seen_t s = {0};
    bool trunc = true;
    TEST_ASSERT_EQUAL_UINT(0, proto_ring_since(&r, N + 5, visit, &s, &trunc));
    TEST_ASSERT_FALSE(trunc);
    TEST_ASSERT_EQUAL_UINT(0, proto_ring_since(&r, N + 100, visit, &s, &trunc));
    TEST_ASSERT_FALSE(trunc);
    proto_ring_clear(&r);
}

static void test_evt_keeps_actor_and_msg(void)
{
    size_t len = 0;
    char *line = proto_core_render_evt(22, "9f3a11c2", "reply",
                                       cJSON_Parse("{\"actor\":\"B\",\"to\":502,\"msg\":503}"), &len);
    TEST_ASSERT_EQUAL_STRING("\n@@{\"t\":\"evt\",\"seq\":22,\"boot\":\"9f3a11c2\",\"ev\":\"reply\","
                             "\"actor\":\"B\",\"to\":502,\"msg\":503}\n", line);
    free(line);
}

static void test_res_collisions_dropped(void)
{
    proto_ctx_t ctx = {.cmd = "tg send", .rid = "7", .actor = NULL};
    size_t len = 0;
    char *line = proto_core_render_res(4, "00000001", &ctx, NULL, NULL,
                                       cJSON_Parse("{\"ok\":false,\"cmd\":\"x\",\"msg\":1}"), &len);
    TEST_ASSERT_EQUAL_STRING("\n@@{\"t\":\"res\",\"seq\":4,\"boot\":\"00000001\",\"cmd\":\"tg send\","
                             "\"rid\":\"7\",\"ok\":true,\"msg\":1}\n", line);
    free(line);
}

static void test_res_null_ctx(void)
{
    size_t len = 0;
    char *line = proto_core_render_res(5, "00000001", NULL, NULL, NULL, NULL, &len);
    TEST_ASSERT_EQUAL_STRING("\n@@{\"t\":\"res\",\"seq\":5,\"boot\":\"00000001\",\"ok\":true}\n", line);
    free(line);
}

static void test_res_err_null_msg(void)
{
    proto_ctx_t ctx = {.cmd = "tg send", .rid = NULL, .actor = "A"};
    size_t len = 0;
    char *line = proto_core_render_res(6, "00000001", &ctx, "offline", NULL,
                                       cJSON_Parse("{\"ignored\":1}"), &len);
    TEST_ASSERT_EQUAL_STRING("\n@@{\"t\":\"res\",\"seq\":6,\"boot\":\"00000001\",\"cmd\":\"tg send\","
                             "\"actor\":\"A\",\"ok\":false,\"err\":\"offline\"}\n", line);
    free(line);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_byte_cap_evicts_oldest);
    RUN_TEST(test_big_line_evicts_several);
    RUN_TEST(test_oversized_line_not_stored_and_truncated);
    RUN_TEST(test_bytes_accounting_across_wraparound);
    RUN_TEST(test_since_newer_than_newest_after_eviction);
    RUN_TEST(test_evt_keeps_actor_and_msg);
    RUN_TEST(test_res_collisions_dropped);
    RUN_TEST(test_res_null_ctx);
    RUN_TEST(test_res_err_null_msg);
    RUN_TEST(test_empty_ring);
    RUN_TEST(test_since_filters_and_orders);
    RUN_TEST(test_exactly_full_not_truncated);
    RUN_TEST(test_wraparound_keeps_last_n);
    RUN_TEST(test_since_older_than_ring_is_truncated);
    RUN_TEST(test_lines_are_verbatim);
    RUN_TEST(test_render_evt);
    RUN_TEST(test_render_evt_null_fields);
    RUN_TEST(test_render_res_ok);
    RUN_TEST(test_render_res_err_no_actor);
    RUN_TEST(test_render_escapes_text);
    return UNITY_END();
}
