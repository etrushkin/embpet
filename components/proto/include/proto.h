/* proto: "@@" frame writer, per-boot sequence and boot id, event ring and replay. */
#ifndef EMBPET_PROTO_H
#define EMBPET_PROTO_H

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "proto_core.h"

/* Create the writer lock, pick the boot id (esp_random, 32-bit hex) and reset seq to 0.
 * Call once from app_main after NVS/cfg, before any other proto call. */
void proto_init(void);

/* Boot id as 8 lowercase hex chars (valid after proto_init). */
const char *proto_boot_id(void);

/* Emit the `boot` event {fw, idf, reset_reason} (README 3.3). Call once after proto_init. */
uint32_t proto_emit_boot(void);

/* All emitters are callable from any task context, including the esp_timer task. Each
 * assigns the next seq (shared by res and evt, starting at 1), renders the frame and
 * writes "\n@@{json}\n" with a single fwrite under a mutex, which also holds off ESP_LOG
 * output (see proto.c). If the USB host is not connected the write is skipped and events
 * stay in the ring. Blocking: the caller may wait for the writer lock (held for one
 * render + write) and, when the host is connected but stalled, for about 50 ms once per
 * stall (the USB-Serial-JTAG TX timeout, after which bytes are dropped fast). It never
 * blocks indefinitely.
 *
 * `fields` / `payload` are consumed (freed) on every path and may be NULL. Keys that
 * collide with a reserved key are dropped; reserved for res: t, seq, boot, cmd, rid,
 * actor, ok (and err, msg for proto_res_err); for evt: t, seq, boot, ev.
 * Return the frame's seq, or 0 if the frame could not be built (out of memory), in which
 * case nothing was recorded and no seq was used. A non-zero return means the frame was
 * recorded (events: in the ring) and written if the host was connected. */

/* res ok:true with the payload fields merged in. */
uint32_t proto_res(const proto_ctx_t *ctx, cJSON *payload);

/* res ok:false, err = code, msg = human text (may be NULL). */
uint32_t proto_res_err(const proto_ctx_t *ctx, const char *code, const char *msg);

/* evt {ev, ...fields}; also stored in the replay ring. */
uint32_t proto_evt(const char *ev, cJSON *fields);

/* `evt since <seq>`: re-emit, verbatim and with their original seq, every ring event with
 * seq > since. Returns how many were replayed (or would have been, if the host is not
 * connected). *truncated is set if matching events had already left the ring; the console
 * command then answers with proto_res(ctx, {"truncated":true}) when set. The console
 * command itself lives in the console bead. */
size_t proto_evt_replay(uint32_t since, bool *truncated);

#endif /* EMBPET_PROTO_H */
