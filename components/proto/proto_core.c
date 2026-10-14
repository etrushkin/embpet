/* Pure logic, see proto_core.h. No ESP-IDF includes. */
#include "proto_core.h"

#include <stdlib.h>
#include <string.h>

/* Move every member of src into dst (skipping keys dst already has), then free src. */
static void merge_consume(cJSON *dst, cJSON *src)
{
    if (src == NULL) {
        return;
    }
    cJSON *c;
    while ((c = src->child) != NULL) {
        cJSON_DetachItemViaPointer(src, c);
        if (c->string == NULL || cJSON_GetObjectItemCaseSensitive(dst, c->string) != NULL ||
            !cJSON_AddItemToObject(dst, c->string, c)) {
            cJSON_Delete(c);
        }
    }
    cJSON_Delete(src);
}

static cJSON *begin_frame(const char *type, uint32_t seq, const char *boot)
{
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return NULL;
    }
    if (cJSON_AddStringToObject(o, "t", type) == NULL ||
        cJSON_AddNumberToObject(o, "seq", (double)seq) == NULL ||
        cJSON_AddStringToObject(o, "boot", boot) == NULL) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}

/* Print `o` and wrap it as "\n@@" json "\n". Frees `o`. */
static char *finish_frame(cJSON *o, size_t *len)
{
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (json == NULL) {
        return NULL;
    }
    size_t n = strlen(json);
    char *line = malloc(n + 5);          /* "\n@@" + json + "\n" + NUL */
    if (line != NULL) {
        line[0] = '\n';
        line[1] = '@';
        line[2] = '@';
        memcpy(line + 3, json, n);
        line[3 + n] = '\n';
        line[4 + n] = '\0';
        if (len != NULL) {
            *len = n + 4;
        }
    }
    cJSON_free(json);
    return line;
}

char *proto_core_render_evt(uint32_t seq, const char *boot, const char *ev, cJSON *fields,
                            size_t *len)
{
    cJSON *o = begin_frame("evt", seq, boot);
    if (o == NULL || cJSON_AddStringToObject(o, "ev", ev) == NULL) {
        cJSON_Delete(o);
        cJSON_Delete(fields);
        return NULL;
    }
    merge_consume(o, fields);
    return finish_frame(o, len);
}

char *proto_core_render_res(uint32_t seq, const char *boot, const proto_ctx_t *ctx,
                            const char *err, const char *msg, cJSON *payload, size_t *len)
{
    cJSON *o = begin_frame("res", seq, boot);
    bool ok = (o != NULL);
    if (ok && ctx != NULL) {
        ok = (ctx->cmd == NULL || cJSON_AddStringToObject(o, "cmd", ctx->cmd) != NULL) &&
             (ctx->rid == NULL || cJSON_AddStringToObject(o, "rid", ctx->rid) != NULL) &&
             (ctx->actor == NULL || cJSON_AddStringToObject(o, "actor", ctx->actor) != NULL);
    }
    if (ok) {
        if (err == NULL) {
            ok = cJSON_AddTrueToObject(o, "ok") != NULL;
        } else {
            ok = cJSON_AddFalseToObject(o, "ok") != NULL &&
                 cJSON_AddStringToObject(o, "err", err) != NULL &&
                 (msg == NULL || cJSON_AddStringToObject(o, "msg", msg) != NULL);
        }
    }
    if (!ok) {
        cJSON_Delete(o);
        cJSON_Delete(payload);
        return NULL;
    }
    if (err == NULL) {
        merge_consume(o, payload);
    } else {
        cJSON_Delete(payload);
    }
    return finish_frame(o, len);
}

/* ---- ring ---- */

void proto_ring_init(proto_ring_t *r)
{
    memset(r, 0, sizeof(*r));
}

void proto_ring_clear(proto_ring_t *r)
{
    for (size_t i = 0; i < r->count; i++) {
        free(r->s[(r->head + i) % EMBPET_EVT_RING].line);
    }
    proto_ring_init(r);
}

void proto_ring_push(proto_ring_t *r, uint32_t seq, char *line, size_t len)
{
    if (len > EMBPET_EVT_RING_BYTES) {
        free(line);
        if (seq > r->evicted_seq) {
            r->evicted_seq = seq;
        }
        return;
    }
    while (r->count == EMBPET_EVT_RING || r->bytes + len > EMBPET_EVT_RING_BYTES) {
        proto_ring_slot_t *old = &r->s[r->head];
        if (old->seq > r->evicted_seq) {
            r->evicted_seq = old->seq;
        }
        r->bytes -= old->len;
        free(old->line);
        r->head = (r->head + 1) % EMBPET_EVT_RING;
        r->count--;
    }
    size_t idx = (r->head + r->count) % EMBPET_EVT_RING;
    r->count++;
    r->bytes += len;
    r->s[idx].line = line;
    r->s[idx].len = len;
    r->s[idx].seq = seq;
}

size_t proto_ring_since(const proto_ring_t *r, uint32_t since, proto_ring_visit_fn fn,
                        void *ctx, bool *truncated)
{
    size_t n = 0;
    if (truncated != NULL) {
        *truncated = r->evicted_seq > since;
    }
    for (size_t i = 0; i < r->count; i++) {
        const proto_ring_slot_t *e = &r->s[(r->head + i) % EMBPET_EVT_RING];
        if (e->seq > since) {
            if (fn != NULL) {
                fn(e->seq, e->line, e->len, ctx);
            }
            n++;
        }
    }
    return n;
}
