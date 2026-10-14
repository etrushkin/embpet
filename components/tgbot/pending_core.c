#include "pending_core.h"

#include <string.h>

static int find_index(const pending_table_t *t, int64_t msg_id)
{
    for (size_t i = 0; i < EMBPET_PENDING_MAX; i++) {
        if (t->e[i].used && t->e[i].msg_id == msg_id) {
            return (int)i;
        }
    }
    return -1;
}

static pending_entry_t *find(pending_table_t *t, int64_t msg_id)
{
    int i = find_index(t, msg_id);
    return i < 0 ? NULL : &t->e[i];
}

/* Oldest used entry (by order) whose answered flag equals `answered`, or NULL. */
static pending_entry_t *oldest(pending_table_t *t, bool answered)
{
    pending_entry_t *best = NULL;
    for (size_t i = 0; i < EMBPET_PENDING_MAX; i++) {
        pending_entry_t *e = &t->e[i];
        if (e->used && e->answered == answered && (best == NULL || e->order < best->order)) {
            best = e;
        }
    }
    return best;
}

static uint64_t age_ms(const pending_entry_t *e, uint64_t now_ms)
{
    return now_ms >= e->sent_ms ? now_ms - e->sent_ms : 0;
}

void pending_init(pending_table_t *t)
{
    memset(t, 0, sizeof *t);
}

size_t pending_count(const pending_table_t *t)
{
    size_t n = 0;
    for (size_t i = 0; i < EMBPET_PENDING_MAX; i++) {
        if (t->e[i].used && !t->e[i].answered) {
            n++;
        }
    }
    return n;
}

bool pending_add(pending_table_t *t, int64_t msg_id, const char *actor, uint64_t now_ms,
                 pending_drop_t *drop)
{
    if (drop != NULL) {
        drop->dropped = false;
    }
    if (actor == NULL) {
        return false;
    }
    size_t n = 0;
    while (n <= ACTOR_ID_MAX && actor[n] != '\0') {
        n++;
    }
    if (!actor_id_valid(actor, n)) {
        return false;
    }

    pending_entry_t *slot = find(t, msg_id);
    if (slot == NULL) {
        for (size_t i = 0; i < EMBPET_PENDING_MAX; i++) {
            if (!t->e[i].used) {
                slot = &t->e[i];
                break;
            }
        }
    }
    if (slot == NULL) {
        slot = oldest(t, true);
    }
    if (slot == NULL) {
        slot = oldest(t, false);
        if (drop != NULL) {
            drop->dropped = true;
            drop->msg_id = slot->msg_id;
            memcpy(drop->actor, slot->actor, sizeof drop->actor);
        }
    }

    slot->used = true;
    slot->answered = false;
    slot->msg_id = msg_id;
    memcpy(slot->actor, actor, n);
    slot->actor[n] = '\0';
    slot->sent_ms = now_ms;
    slot->order = t->next_order++;
    return true;
}

bool pending_lookup(const pending_table_t *t, int64_t msg_id, char actor_out[ACTOR_BUF_SIZE])
{
    int i = find_index(t, msg_id);
    if (i < 0) {
        return false;
    }
    if (actor_out != NULL) {
        memcpy(actor_out, t->e[i].actor, ACTOR_BUF_SIZE);
    }
    return true;
}

bool pending_answer(pending_table_t *t, int64_t msg_id, char actor_out[ACTOR_BUF_SIZE])
{
    pending_entry_t *e = find(t, msg_id);
    if (e == NULL) {
        return false;
    }
    e->answered = true;
    if (actor_out != NULL) {
        memcpy(actor_out, e->actor, ACTOR_BUF_SIZE);
    }
    return true;
}

size_t pending_ack_msg(pending_table_t *t, int64_t msg_id)
{
    pending_entry_t *e = find(t, msg_id);
    if (e == NULL || e->answered) {
        return 0;
    }
    e->answered = true;
    return 1;
}

size_t pending_ack_older(pending_table_t *t, uint64_t now_ms, uint32_t older_s)
{
    size_t n = 0;
    uint64_t limit = (uint64_t)older_s * 1000u;
    for (size_t i = 0; i < EMBPET_PENDING_MAX; i++) {
        pending_entry_t *e = &t->e[i];
        if (e->used && !e->answered && age_ms(e, now_ms) >= limit) {
            e->answered = true;
            n++;
        }
    }
    return n;
}

size_t pending_list(const pending_table_t *t, const char *actor_filter, uint64_t now_ms,
                    pending_item_t *out, size_t max)
{
    size_t total = 0;
    uint64_t last = 0;
    bool have_last = false;
    for (;;) {
        const pending_entry_t *best = NULL;
        for (size_t i = 0; i < EMBPET_PENDING_MAX; i++) {
            const pending_entry_t *e = &t->e[i];
            if (!e->used || e->answered) {
                continue;
            }
            if (actor_filter != NULL && strcmp(e->actor, actor_filter) != 0) {
                continue;
            }
            if (have_last && e->order <= last) {
                continue;
            }
            if (best == NULL || e->order < best->order) {
                best = e;
            }
        }
        if (best == NULL) {
            break;
        }
        if (total < max && out != NULL) {
            out[total].msg_id = best->msg_id;
            memcpy(out[total].actor, best->actor, ACTOR_BUF_SIZE);
            out[total].age_s = (uint32_t)(age_ms(best, now_ms) / 1000u);
        }
        total++;
        last = best->order;
        have_last = true;
    }
    return total;
}
