#include <string.h>

#include "pending_core.h"
#include "unity.h"

static pending_table_t T;
static pending_drop_t D;

void setUp(void) { pending_init(&T); memset(&D, 0xAA, sizeof D); }
void tearDown(void) {}

static void fill(int n)   /* msgs 1..n, actor "A", sent at 1000*i ms */
{
    for (int i = 1; i <= n; i++) {
        TEST_ASSERT_TRUE(pending_add(&T, i, "A", 1000u * (uint64_t)i, &D));
        TEST_ASSERT_FALSE(D.dropped);
    }
}

static void test_default_capacity_is_64(void)
{
    TEST_ASSERT_EQUAL_INT(64, EMBPET_PENDING_MAX);
}

static void test_add_and_count(void)
{
    TEST_ASSERT_EQUAL_UINT(0, pending_count(&T));
    TEST_ASSERT_TRUE(pending_add(&T, 501, "A", 0, &D));
    TEST_ASSERT_TRUE(pending_add(&T, 502, "B", 0, &D));
    TEST_ASSERT_EQUAL_UINT(2, pending_count(&T));
    TEST_ASSERT_FALSE(D.dropped);
}

static void test_add_rejects_invalid_actor(void)
{
    TEST_ASSERT_FALSE(pending_add(&T, 1, "", 0, &D));
    TEST_ASSERT_FALSE(pending_add(&T, 1, "a b", 0, &D));
    TEST_ASSERT_FALSE(pending_add(&T, 1, "abcdefghijklmnopq", 0, &D));
    TEST_ASSERT_FALSE(pending_add(&T, 1, NULL, 0, &D));
    TEST_ASSERT_EQUAL_UINT(0, pending_count(&T));
}

static void test_add_accepts_max_length_actor_and_int64_ids(void)
{
    char a[ACTOR_BUF_SIZE];
    TEST_ASSERT_TRUE(pending_add(&T, 5000000000LL, "abcdefghijklmnop", 0, &D));
    TEST_ASSERT_TRUE(pending_lookup(&T, 5000000000LL, a));
    TEST_ASSERT_EQUAL_STRING("abcdefghijklmnop", a);
    TEST_ASSERT_FALSE(pending_lookup(&T, 5LL, a));
    TEST_ASSERT_FALSE(pending_lookup(&T, 705032704LL, a));   /* low 32 bits of 5000000000 */

    TEST_ASSERT_TRUE(pending_add(&T, INT64_MAX - 1, "B", 0, &D));
    TEST_ASSERT_TRUE(pending_add(&T, -42LL, "C", 0, &D));
    TEST_ASSERT_TRUE(pending_lookup(&T, INT64_MAX - 1, a));
    TEST_ASSERT_EQUAL_STRING("B", a);
    TEST_ASSERT_TRUE(pending_lookup(&T, -42LL, a));
    TEST_ASSERT_EQUAL_STRING("C", a);
    TEST_ASSERT_FALSE(pending_lookup(&T, INT64_MAX, a));
    TEST_ASSERT_FALSE(pending_lookup(&T, 42LL, a));
}

static void test_refresh_duplicate_in_full_pending_table_does_not_drop(void)
{
    fill(EMBPET_PENDING_MAX);
    TEST_ASSERT_TRUE(pending_add(&T, 1, "B", 99999, &D));
    TEST_ASSERT_FALSE(D.dropped);
    TEST_ASSERT_EQUAL_UINT(EMBPET_PENDING_MAX, pending_count(&T));
    char a[ACTOR_BUF_SIZE];
    TEST_ASSERT_TRUE(pending_lookup(&T, 1, a));
    TEST_ASSERT_EQUAL_STRING("B", a);
    TEST_ASSERT_TRUE(pending_lookup(&T, 2, NULL));
    /* refreshed entry is now the newest: the next overflow drops msg 2 */
    TEST_ASSERT_TRUE(pending_add(&T, 1001, "C", 99999, &D));
    TEST_ASSERT_TRUE(D.dropped);
    TEST_ASSERT_EQUAL_INT64(2, D.msg_id);
}

static void test_add_same_msg_refreshes_in_place(void)
{
    TEST_ASSERT_TRUE(pending_add(&T, 7, "A", 0, &D));
    TEST_ASSERT_TRUE(pending_answer(&T, 7, NULL));
    TEST_ASSERT_TRUE(pending_add(&T, 7, "B", 10, &D));
    TEST_ASSERT_EQUAL_UINT(1, pending_count(&T));
    char a[ACTOR_BUF_SIZE];
    TEST_ASSERT_TRUE(pending_lookup(&T, 7, a));
    TEST_ASSERT_EQUAL_STRING("B", a);
}

static void test_answer_returns_actor_and_decrements(void)
{
    char a[ACTOR_BUF_SIZE] = "";
    fill(1);
    TEST_ASSERT_TRUE(pending_answer(&T, 1, a));
    TEST_ASSERT_EQUAL_STRING("A", a);
    TEST_ASSERT_EQUAL_UINT(0, pending_count(&T));
}

static void test_answer_unknown_is_not_found(void)
{
    char a[ACTOR_BUF_SIZE] = "keep";
    fill(2);
    TEST_ASSERT_FALSE(pending_answer(&T, 99, a));
    TEST_ASSERT_EQUAL_STRING("keep", a);
    TEST_ASSERT_EQUAL_UINT(2, pending_count(&T));
}

static void test_double_reply(void)
{
    char a[ACTOR_BUF_SIZE] = "";
    TEST_ASSERT_TRUE(pending_add(&T, 10, "A", 0, &D));
    TEST_ASSERT_TRUE(pending_add(&T, 11, "B", 0, &D));
    TEST_ASSERT_TRUE(pending_answer(&T, 10, a));
    TEST_ASSERT_EQUAL_UINT(1, pending_count(&T));
    memset(a, 0, sizeof a);
    TEST_ASSERT_TRUE(pending_answer(&T, 10, a));      /* second reply still routes */
    TEST_ASSERT_EQUAL_STRING("A", a);
    TEST_ASSERT_EQUAL_UINT(1, pending_count(&T));      /* count unchanged */
}

static void test_answered_entry_still_routes(void)
{
    char a[ACTOR_BUF_SIZE] = "";
    fill(1);
    TEST_ASSERT_TRUE(pending_answer(&T, 1, NULL));
    TEST_ASSERT_TRUE(pending_lookup(&T, 1, a));
    TEST_ASSERT_EQUAL_STRING("A", a);
    pending_ack_msg(&T, 1);
    TEST_ASSERT_TRUE(pending_lookup(&T, 1, a));
}

static void test_ack_msg(void)
{
    fill(3);
    TEST_ASSERT_EQUAL_UINT(1, pending_ack_msg(&T, 2));
    TEST_ASSERT_EQUAL_UINT(2, pending_count(&T));
    TEST_ASSERT_EQUAL_UINT(0, pending_ack_msg(&T, 2));    /* already answered */
    TEST_ASSERT_EQUAL_UINT(0, pending_ack_msg(&T, 42));   /* unknown */
    TEST_ASSERT_EQUAL_UINT(2, pending_count(&T));
}

static void test_ack_older_zero_acks_all(void)
{
    fill(5);
    TEST_ASSERT_EQUAL_UINT(5, pending_ack_older(&T, 5000, 0));
    TEST_ASSERT_EQUAL_UINT(0, pending_count(&T));
    TEST_ASSERT_EQUAL_UINT(0, pending_ack_older(&T, 5000, 0));   /* nothing left to change */
}

static void test_ack_older_zero_acks_entries_with_zero_age(void)
{
    TEST_ASSERT_TRUE(pending_add(&T, 1, "A", 1000, &D));
    TEST_ASSERT_EQUAL_UINT(1, pending_ack_older(&T, 1000, 0));
}

static void test_ack_older_boundary(void)
{
    /* msg i sent at i*1000 ms; now = 10000 -> ages 9000, 8000, ... */
    fill(3);   /* ages at now=10000: 9000, 8000, 7000 */
    TEST_ASSERT_EQUAL_UINT(0, pending_ack_older(&T, 10000, 10));    /* oldest is 9 s < 10 s */
    TEST_ASSERT_EQUAL_UINT(1, pending_ack_older(&T, 10000, 9));     /* exactly 9 s -> acked */
    TEST_ASSERT_EQUAL_UINT(2, pending_count(&T));
    TEST_ASSERT_EQUAL_UINT(0, pending_ack_older(&T, 10000, 9));
    TEST_ASSERT_EQUAL_UINT(1, pending_ack_older(&T, 10999, 8));     /* msg 2: 8999 ms acked; msg 3: 7999 ms kept */
    TEST_ASSERT_EQUAL_UINT(1, pending_count(&T));
}

static void test_ack_older_one_ms_short_is_not_acked(void)
{
    TEST_ASSERT_TRUE(pending_add(&T, 1, "A", 0, &D));
    TEST_ASSERT_EQUAL_UINT(0, pending_ack_older(&T, 4999, 5));
    TEST_ASSERT_EQUAL_UINT(1, pending_ack_older(&T, 5000, 5));
}

static void test_ack_older_clock_behind_counts_as_age_zero(void)
{
    TEST_ASSERT_TRUE(pending_add(&T, 1, "A", 5000, &D));
    TEST_ASSERT_EQUAL_UINT(0, pending_ack_older(&T, 1000, 1));
    TEST_ASSERT_EQUAL_UINT(1, pending_ack_older(&T, 1000, 0));
}

static void test_ack_older_large_seconds_no_overflow(void)
{
    TEST_ASSERT_TRUE(pending_add(&T, 1, "A", 0, &D));
    TEST_ASSERT_EQUAL_UINT(0, pending_ack_older(&T, 1000000, 0xFFFFFFFFu));
}

static void test_fill_to_capacity_no_drop(void)
{
    fill(EMBPET_PENDING_MAX);
    TEST_ASSERT_EQUAL_UINT(EMBPET_PENDING_MAX, pending_count(&T));
}

static void test_eviction_prefers_oldest_answered(void)
{
    fill(EMBPET_PENDING_MAX);
    /* answer 5 first, then 3: eviction goes by age (insertion), not answer time */
    TEST_ASSERT_TRUE(pending_answer(&T, 5, NULL));
    TEST_ASSERT_TRUE(pending_answer(&T, 3, NULL));
    TEST_ASSERT_EQUAL_UINT(EMBPET_PENDING_MAX - 2, pending_count(&T));

    TEST_ASSERT_TRUE(pending_add(&T, 1001, "B", 99999, &D));
    TEST_ASSERT_FALSE(D.dropped);
    TEST_ASSERT_FALSE(pending_lookup(&T, 3, NULL));      /* oldest answered gone */
    TEST_ASSERT_TRUE(pending_lookup(&T, 5, NULL));
    TEST_ASSERT_TRUE(pending_lookup(&T, 1, NULL));       /* older but still pending: kept */
    TEST_ASSERT_EQUAL_UINT(EMBPET_PENDING_MAX - 1, pending_count(&T));

    TEST_ASSERT_TRUE(pending_add(&T, 1002, "B", 99999, &D));
    TEST_ASSERT_FALSE(D.dropped);
    TEST_ASSERT_FALSE(pending_lookup(&T, 5, NULL));
    TEST_ASSERT_TRUE(pending_lookup(&T, 1, NULL));
    TEST_ASSERT_EQUAL_UINT(EMBPET_PENDING_MAX, pending_count(&T));
}

static void test_all_pending_overflow_drops_oldest_and_reports(void)
{
    fill(EMBPET_PENDING_MAX);
    TEST_ASSERT_TRUE(pending_add(&T, 1001, "B", 99999, &D));
    TEST_ASSERT_TRUE(D.dropped);
    TEST_ASSERT_EQUAL_INT64(1, D.msg_id);
    TEST_ASSERT_EQUAL_STRING("A", D.actor);
    TEST_ASSERT_FALSE(pending_lookup(&T, 1, NULL));
    TEST_ASSERT_TRUE(pending_lookup(&T, 2, NULL));
    TEST_ASSERT_TRUE(pending_lookup(&T, 1001, NULL));
    TEST_ASSERT_EQUAL_UINT(EMBPET_PENDING_MAX, pending_count(&T));

    TEST_ASSERT_TRUE(pending_add(&T, 1002, "C", 99999, &D));
    TEST_ASSERT_TRUE(D.dropped);
    TEST_ASSERT_EQUAL_INT64(2, D.msg_id);
}

static void test_overflow_with_null_drop_is_safe(void)
{
    fill(EMBPET_PENDING_MAX);
    TEST_ASSERT_TRUE(pending_add(&T, 1001, "B", 1, NULL));
    TEST_ASSERT_FALSE(pending_lookup(&T, 1, NULL));
}

static void test_order_survives_slot_reuse(void)
{
    /* Eviction reuses slot 0 for newest; the next "oldest" must still be msg 2. */
    fill(EMBPET_PENDING_MAX);
    TEST_ASSERT_TRUE(pending_add(&T, 1001, "B", 1, &D));   /* drops 1, reuses its slot */
    TEST_ASSERT_TRUE(pending_add(&T, 1002, "B", 1, &D));
    TEST_ASSERT_TRUE(D.dropped);
    TEST_ASSERT_EQUAL_INT64(2, D.msg_id);
}

static void test_list_filters_orders_and_ages(void)
{
    pending_item_t it[EMBPET_PENDING_MAX];
    TEST_ASSERT_TRUE(pending_add(&T, 1, "A", 1000, &D));
    TEST_ASSERT_TRUE(pending_add(&T, 2, "B", 2000, &D));
    TEST_ASSERT_TRUE(pending_add(&T, 3, "A", 3500, &D));
    TEST_ASSERT_TRUE(pending_add(&T, 4, "A", 4000, &D));
    TEST_ASSERT_TRUE(pending_answer(&T, 4, NULL));

    TEST_ASSERT_EQUAL_UINT(3, pending_list(&T, NULL, 6000, it, EMBPET_PENDING_MAX));
    TEST_ASSERT_EQUAL_INT64(1, it[0].msg_id);
    TEST_ASSERT_EQUAL_UINT32(5, it[0].age_s);
    TEST_ASSERT_EQUAL_INT64(2, it[1].msg_id);
    TEST_ASSERT_EQUAL_STRING("B", it[1].actor);
    TEST_ASSERT_EQUAL_INT64(3, it[2].msg_id);
    TEST_ASSERT_EQUAL_UINT32(2, it[2].age_s);      /* 2500 ms -> 2 s */

    TEST_ASSERT_EQUAL_UINT(2, pending_list(&T, "A", 6000, it, EMBPET_PENDING_MAX));
    TEST_ASSERT_EQUAL_INT64(1, it[0].msg_id);
    TEST_ASSERT_EQUAL_INT64(3, it[1].msg_id);
    TEST_ASSERT_EQUAL_UINT(0, pending_list(&T, "Z", 6000, it, EMBPET_PENDING_MAX));
}

static void test_list_truncates_but_reports_total(void)
{
    pending_item_t it[2];
    fill(5);
    TEST_ASSERT_EQUAL_UINT(5, pending_list(&T, NULL, 9000, it, 2));
    TEST_ASSERT_EQUAL_INT64(1, it[0].msg_id);
    TEST_ASSERT_EQUAL_INT64(2, it[1].msg_id);
    TEST_ASSERT_EQUAL_UINT(5, pending_list(&T, NULL, 9000, NULL, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_default_capacity_is_64);
    RUN_TEST(test_add_and_count);
    RUN_TEST(test_add_rejects_invalid_actor);
    RUN_TEST(test_add_accepts_max_length_actor_and_int64_ids);
    RUN_TEST(test_add_same_msg_refreshes_in_place);
    RUN_TEST(test_refresh_duplicate_in_full_pending_table_does_not_drop);
    RUN_TEST(test_answer_returns_actor_and_decrements);
    RUN_TEST(test_answer_unknown_is_not_found);
    RUN_TEST(test_double_reply);
    RUN_TEST(test_answered_entry_still_routes);
    RUN_TEST(test_ack_msg);
    RUN_TEST(test_ack_older_zero_acks_all);
    RUN_TEST(test_ack_older_zero_acks_entries_with_zero_age);
    RUN_TEST(test_ack_older_boundary);
    RUN_TEST(test_ack_older_one_ms_short_is_not_acked);
    RUN_TEST(test_ack_older_clock_behind_counts_as_age_zero);
    RUN_TEST(test_ack_older_large_seconds_no_overflow);
    RUN_TEST(test_fill_to_capacity_no_drop);
    RUN_TEST(test_eviction_prefers_oldest_answered);
    RUN_TEST(test_all_pending_overflow_drops_oldest_and_reports);
    RUN_TEST(test_overflow_with_null_drop_is_safe);
    RUN_TEST(test_order_survives_slot_reuse);
    RUN_TEST(test_list_filters_orders_and_ages);
    RUN_TEST(test_list_truncates_but_reports_total);
    return UNITY_END();
}
