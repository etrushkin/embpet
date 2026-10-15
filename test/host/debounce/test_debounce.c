#include "debounce_core.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* Test harness: `raw` is the ISR's edge counter, settle() is the timer expiry. */
typedef struct {
    deb_t d;
    uint32_t raw;
    uint32_t events;
    uint32_t last_extra;
} rig_t;

static void rig_init(rig_t *r, bool active, uint32_t raw0)
{
    r->raw = raw0;
    r->events = 0;
    r->last_extra = 0xFFFFFFFFu;
    deb_init(&r->d, active, r->raw);
}

static void edges(rig_t *r, uint32_t n)
{
    r->raw += n;
}

static bool settle(rig_t *r, bool active)
{
    uint32_t extra = 0;
    bool ev = deb_settle(&r->d, active, r->raw, &extra);
    if (ev) {
        r->events++;
        r->last_extra = extra;
    }
    return ev;
}

static void test_clean_edge_extra_zero(void)
{
    rig_t r;
    rig_init(&r, false, 0);
    edges(&r, 1);
    TEST_ASSERT_TRUE(settle(&r, true));
    TEST_ASSERT_EQUAL_UINT32(0, r.last_extra);
    TEST_ASSERT_TRUE(r.d.active);
    edges(&r, 1);
    TEST_ASSERT_TRUE(settle(&r, false));
    TEST_ASSERT_EQUAL_UINT32(0, r.last_extra);
}

static void test_bounce_five_edges_extra_four(void)
{
    rig_t r;
    rig_init(&r, false, 0);
    edges(&r, 5);                               /* 5 edges, ends in the new level */
    TEST_ASSERT_TRUE(settle(&r, true));
    TEST_ASSERT_EQUAL_UINT32(4, r.last_extra);
    TEST_ASSERT_EQUAL_UINT32(0, deb_pending(&r.d, r.raw));
}

static void test_bounce_even_count_ends_in_new_level(void)
{
    rig_t r;
    rig_init(&r, true, 0);
    edges(&r, 4);                               /* released with contact bounce */
    TEST_ASSERT_TRUE(settle(&r, false));
    TEST_ASSERT_EQUAL_UINT32(3, r.last_extra);
}

static void test_glitch_without_change_accumulates_into_next_event(void)
{
    rig_t r;
    rig_init(&r, false, 0);
    edges(&r, 2);                               /* glitch: away and back */
    TEST_ASSERT_FALSE(settle(&r, false));
    TEST_ASSERT_EQUAL_UINT32(0, r.events);
    TEST_ASSERT_EQUAL_UINT32(2, deb_pending(&r.d, r.raw));
    edges(&r, 1);                               /* the real change */
    TEST_ASSERT_TRUE(settle(&r, true));
    TEST_ASSERT_EQUAL_UINT32(2, r.last_extra);  /* glitch edges, not the changing one */
    TEST_ASSERT_EQUAL_UINT32(0, deb_pending(&r.d, r.raw));
}

/* Pending edges can be odd or large (e.g. edges merged by the GPIO latch undercount):
 * the bookkeeping only sums what the ISR counted. */
static void test_several_glitches_then_bounced_change(void)
{
    rig_t r;
    rig_init(&r, false, 0);
    edges(&r, 2);
    TEST_ASSERT_FALSE(settle(&r, false));
    edges(&r, 4);
    TEST_ASSERT_FALSE(settle(&r, false));
    TEST_ASSERT_EQUAL_UINT32(6, deb_pending(&r.d, r.raw));
    edges(&r, 3);                               /* bouncing change, odd count */
    TEST_ASSERT_TRUE(settle(&r, true));
    TEST_ASSERT_EQUAL_UINT32(8, r.last_extra);  /* 9 edges - 1 */
}

static void test_extra_is_per_event_not_cumulative(void)
{
    rig_t r;
    rig_init(&r, false, 0);
    edges(&r, 3);
    TEST_ASSERT_TRUE(settle(&r, true));
    TEST_ASSERT_EQUAL_UINT32(2, r.last_extra);
    edges(&r, 1);
    TEST_ASSERT_TRUE(settle(&r, false));
    TEST_ASSERT_EQUAL_UINT32(0, r.last_extra);
}

static void test_debounce_zero_each_edge_settles_at_once(void)
{
    /* Debounce 0 with edges more than 1 ms apart: each edge is sampled alone, so every
     * edge is a change. */
    rig_t r;
    rig_init(&r, false, 0);
    bool level = false;
    for (int k = 0; k < 5; k++) {
        level = !level;
        edges(&r, 1);
        TEST_ASSERT_TRUE(settle(&r, level));
        TEST_ASSERT_EQUAL_UINT32(0, r.last_extra);
    }
    TEST_ASSERT_EQUAL_UINT32(5, r.events);
}

static void test_debounce_zero_bounce_folds_into_extra(void)
{
    /* Debounce 0, 5-edge bounce inside the 1 ms window: one event, extra 4. */
    rig_t r;
    rig_init(&r, false, 0);
    edges(&r, 5);
    TEST_ASSERT_TRUE(settle(&r, true));
    TEST_ASSERT_EQUAL_UINT32(4, r.last_extra);
    TEST_ASSERT_EQUAL_UINT32(1, r.events);
}

static void test_debounce_zero_coalesced_edges_show_as_extra(void)
{
    /* Three edges land inside the window and it samples the changed level: extra 2.
     * Two edges ending in the old level: no event, counted into the next one (extra 2). */
    rig_t r;
    rig_init(&r, false, 0);
    edges(&r, 3);
    TEST_ASSERT_TRUE(settle(&r, true));
    TEST_ASSERT_EQUAL_UINT32(2, r.last_extra);
    edges(&r, 2);
    TEST_ASSERT_FALSE(settle(&r, true));
    TEST_ASSERT_EQUAL_UINT32(2, deb_pending(&r.d, r.raw));
    edges(&r, 1);
    TEST_ASSERT_TRUE(settle(&r, false));
    TEST_ASSERT_EQUAL_UINT32(2, r.last_extra);
}

static void test_edges_are_conserved(void)
{
    /* Over any sequence, sum(extra + 1) over events + pending == raw edges since init. */
    rig_t r;
    rig_init(&r, false, 100);
    uint32_t sum = 0;
    bool level = false;
    const uint32_t burst[] = {1, 2, 5, 1, 4, 7, 2, 3};
    for (unsigned k = 0; k < sizeof(burst) / sizeof(burst[0]); k++) {
        edges(&r, burst[k]);
        if (burst[k] % 2u == 1u) {
            level = !level;
        }
        if (settle(&r, level)) {
            sum += r.last_extra + 1u;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(r.raw - 100u, sum + deb_pending(&r.d, r.raw));
}

static void test_counter_wraps(void)
{
    rig_t r;
    rig_init(&r, false, 0xFFFFFFFEu);
    edges(&r, 3);                               /* wraps to 1 */
    TEST_ASSERT_EQUAL_UINT32(1u, r.raw);
    TEST_ASSERT_EQUAL_UINT32(3, deb_pending(&r.d, r.raw));
    TEST_ASSERT_TRUE(settle(&r, true));
    TEST_ASSERT_EQUAL_UINT32(2, r.last_extra);
}

static void test_changed_level_without_counted_edge(void)
{
    /* A change whose edge the ISR never counted (e.g. the level was already different at
     * init) reports extra 0, not a huge wrapped value. */
    rig_t r;
    rig_init(&r, false, 7);
    TEST_ASSERT_TRUE(settle(&r, true));
    TEST_ASSERT_EQUAL_UINT32(0, r.last_extra);
}

static void test_no_change_no_edges(void)
{
    rig_t r;
    rig_init(&r, true, 0);
    TEST_ASSERT_FALSE(settle(&r, true));
    TEST_ASSERT_EQUAL_UINT32(0, deb_pending(&r.d, r.raw));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_clean_edge_extra_zero);
    RUN_TEST(test_bounce_five_edges_extra_four);
    RUN_TEST(test_bounce_even_count_ends_in_new_level);
    RUN_TEST(test_glitch_without_change_accumulates_into_next_event);
    RUN_TEST(test_several_glitches_then_bounced_change);
    RUN_TEST(test_extra_is_per_event_not_cumulative);
    RUN_TEST(test_debounce_zero_each_edge_settles_at_once);
    RUN_TEST(test_debounce_zero_bounce_folds_into_extra);
    RUN_TEST(test_debounce_zero_coalesced_edges_show_as_extra);
    RUN_TEST(test_edges_are_conserved);
    RUN_TEST(test_counter_wraps);
    RUN_TEST(test_changed_level_without_counted_edge);
    RUN_TEST(test_no_change_no_edges);
    return UNITY_END();
}
