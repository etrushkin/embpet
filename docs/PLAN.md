# embpet implementation plan

**Audience:** the orchestrating agent and the implementing agents.

This file holds the **architecture** and the **verification strategy** only. It deliberately contains no task list, so it can't drift out of sync with the work:

| What | Where |
|---|---|
| Behaviour (spec) and review decisions | `README.md` (decisions in §8) |
| Work items, status, dependencies, critical path, risks | beads (see §3) |
| Workflow rules: hardware access, definition of done, agent routing, secrets, and git (agents commit and push each verified task) | `AGENTS.md` → *Project workflow* |

If this file disagrees with the README, the README wins. Fix this file in the same change.

---

## 1. Architecture

### 1.1 Components

```
main/            app_main: init order, command registration
components/
  proto/         @@ frame writer, event ring, console helpers (-a/-i, escapes), shared state
  netcfg/        NVS config store (cfg), Wi-Fi station
  tgbot/         Bot API client, pending table, bridge (send/poll/route), pairing
  io/            outputs + pulses, inputs + debounce, status LED
test/host/       native Unity tests for *_core.c (no ESP-IDF headers)
test/hil/        pytest over serial against the real board
tools/embpet/    python package (serial, frames), embpet_cli.py wrapper
scripts/         bootstrap.sh, export.sh, test_host.sh, dev-venv.sh
```

**Pure-logic rule:** state machines live in `*_core.c` files. They have no ESP-IDF includes, take time as `now_ms`, and are unit-tested on the host:

| Core file | What it holds |
|---|---|
| `escape_core` | Decoding of escapes in text arguments |
| `proto_core` | Event ring |
| `pending_core` | Pending ("unanswered") table |
| `prefix_core` | Parser for the `[A]` actor prefix |
| `pulse_core` | Output pulse state machine |
| `debounce_core` | Input debounce and `extra` counting |
| `led_core` | LED pattern engine |
| JSON / UTF-16 helpers | Response parsing and entity offsets |

### 1.2 Runtime model

| Context | Owner of | Stack (start) | Notes |
|---|---|---|---|
| console REPL task | command parsing, res frames | 6 KB | Never does TLS. `tg send` waits on a semaphore for the send task. |
| `tg_send` task | send HTTPS handle, request queue | 8 KB | sendMessage, hint replies, online notice |
| `tg_poll` task | poll HTTPS handle, getUpdates offset | 8 KB | Routes replies and emits `reply` events. Advances the offset only after the frame is written. |
| esp_timer task | pulse ends, debounce expiry, LED tick (50 ms) | default | Callbacks must be short. They may call `proto_evt`. |
| GPIO ISR | raw edge counters, debounce re-arm | ISR | No logging, no frames |
| default event loop | Wi-Fi/IP events, reconnect backoff | default | Updates `state` |

- **Shared state:** `proto/state.c`, mutex-protected. It holds wifi {state, ip, rssi}, tg {state, bot}, pending count, booting flag and configured flag. Subscribers get change callbacks. The LED and the `wifi`/`tg`/`pending` events are driven from here.
- **Frames:** `proto_res` / `proto_evt` are callable from any task context. Each frame is one `fwrite` under a mutex. A frame is skipped (but kept in the ring) if the USB host isn't connected, so the caller never blocks.
- **Init order** (`app_main`):
  1. Outputs: latch high (off), set as output, then apply boot levels.
  2. NVS and cfg.
  3. proto and state, then the `boot` event.
  4. Inputs and LED (booting = solid on).
  5. Console REPL.
  6. Wi-Fi.
  7. tgbot tasks.
  8. Clear the booting flag, then set the runtime log level to WARN.
- **Memory budget:** about 320 KB heap. Wi-Fi takes about 55 KB. Each TLS session takes 35–45 KB with dynamic buffers, ×2. The getUpdates response buffer is capped at 16 KB. Target: min free heap above 60 KB after a 24 h soak.

### 1.3 Key data flows

- **Actor sends:**
  1. The host runs `tg send -a A -i 7 "text"`.
  2. The console decodes the text, validates it, and posts it to `tg_send`.
  3. The `sendMessage` request goes to the owner with a bold `[A]` entity (UTF-16 offsets).
  4. On success the message is added to pending, `res {msg}` is returned, and a `pending` event is emitted.
- **Owner replies:**
  1. `tg_poll` receives an update and checks that it comes from the owner chat.
  2. It looks up `reply_to_message.message_id` in the pending table, falling back to the `[A]` prefix in the replied-to text.
  3. It emits `evt reply`, marks the message answered (which emits `pending`), and advances the offset.
  4. If the message is not a reply, or can't be routed, `tg_send` posts the hint text.

---

## 2. Verification

| Level | What | Where |
|---|---|---|
| Unit (host) | All `*_core.c` modules, escape decoder, JSON and UTF-16 helpers | `test/host`, run with `scripts/test_host.sh` |
| Device smoke | Build, flash, `sys info`, the command under test | `tools/embpet_cli.py` |
| HIL | Protocol, io, wifi and tg suites over serial | `test/hil` (pytest, project `.venv`) |
| Robustness | Network, host and reboot disturbances; 24 h soak | HIL plus manual disturbances |
| Bench | Opto wiring, boot with GPIO2 loaded, outputs off during reset | Human, on real wiring |

---

## 3. Work tracking: entry point

All planning state lives in beads. Start every session with:

```bash
bd prime                      # workflow context + project memories
bd ready                      # unblocked work, highest priority first
bd show <id>                  # the brief: Context / Scope / Acceptance Criteria / Refs (+ risk notes)
```

| Question | Command |
|---|---|
| Which epics exist? | `bd list --type epic` |
| What's on the critical path? | `bd list -l critical-path` (prefer these when several tasks are ready) |
| Which tasks need the board? | `bd list -l hw` (one at a time, see AGENTS.md) |
| What's waiting on the user? | `bd human list` |
| Which tasks should use a stronger model? | `bd list -l model-opus` |
| What blocks an issue, and what does it unblock? | `bd dep tree <id>` / `bd show <id>` |
| What is blocked right now? | `bd blocked` |

Parallel work comes from the dependency graph: every task in `bd ready` can run concurrently, except that hardware tasks take turns on the board.
