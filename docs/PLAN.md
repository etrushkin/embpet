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
| `tgapi_core` | Bot API request bodies, response classification (16 KB cap, 401/404/429), UTF-16 entity offsets, retry delay |

### 1.2 Runtime model

| Context | Owner of | Stack (start) | Notes |
|---|---|---|---|
| console REPL task | command parsing, res frames | 6 KB | Priority 3. Never does TLS. After each command and input chunk, once the idle task has not run for 100 ms, it sleeps tick by tick (up to 10 ms each, at most 100 ms) until the idle task has run. That leaves lower-priority tasks and the idle task a share of the CPU under a command flood; a task-watchdog report then points at a task that hogs the CPU itself (embpet-94q). Console output bypasses the per-byte IDF VFS and goes to the driver in whole chunks (embpet-s2j.4): about 1.5 ms of CPU per `sys info`, 5.3 ms per `help` (README 3.1). Input is flow-controlled (embpet-s2j.3): when the 4.6 KB RX ring is nearly full, the driver's RX packet interrupt is turned off, so the hardware NAKs further packets until the REPL reads again; no RAM added, nothing dropped while a command blocks. The REPL reads with a 1 s timeout and re-enables from the hardware bit, so a lost re-enable heals within 1 s. `tg send` waits on its request's semaphore for the send task, with its own 10 s deadline. Measured (embpet-64s): 2952 B free after 4 KB `tg send` lines, injected replies and every tg command. |
| `tg_send` task | send HTTPS handle, request queue | 8 KB | sendMessage, hint replies, online notice; getMe + deleteWebhook per token, one getMe probe after each Wi-Fi reconnect; hints at most 1 per 10 s, `tg send` queued ahead of them. Priority 2, below the console. Measured: 4988 B free. |
| `tg_poll` task | poll HTTPS handle, getUpdates offset | 8 KB | Routes replies and emits `reply` events. Advances the offset only after the frame is written. Priority 2. Measured: 5336 B free, with long polls but no real owner reply yet (the injected replies of `tools/bridge_test.py auto` run on the console task, which they then also measure). |
| esp_timer task | LED tick (50 ms), `input` events (drains the debounce decision queue) | default | Callbacks must be short. They may call `proto_evt`. All task-dispatched esp_timer callbacks share one 3584-byte stack; measured at least 2144 B free with `proto_evt` and input events (2212 B with the queued input events of embpet-jk5.1, `tools/io_test.py inputs`), so a new callback must stay under about 1.5 KB. Priority 22, below the Wi-Fi driver task: callbacks run up to about 0.33 s late during each WPA3-SAE step (next rows), which is why the pulse end and the debounce expiry are not here. |
| esp_timer ISR (`ESP_TIMER_ISR` dispatch) | pulse ends, debounce expiry | ISR | embpet-jk5.1, `CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD`. Runs on time while the Wi-Fi driver task computes SAE. ISR-safe only: spinlocks (`portMUX`) instead of mutexes, no `proto_evt`, no logging. A debounce decision goes to a 32-entry FreeRTOS queue and a task-dispatched timer kicks the esp_timer task, which emits the `input` events in order; with the queue full the decision is retried after the drain. The esp_timer interrupt is IRAM (`CONFIG_ESP_TIMER_IN_IRAM`, forced on for the C3) and runs with the flash cache off, so the callbacks and their callees are IRAM (`IRAM_ATTR`, `components/io/linker.lf` for `pulse_expire`/`deb_settle`, `CONFIG_GPIO_CTRL_FUNC_IN_IRAM`). Single core: CPU0 affinity, the only choice. |
| Wi-Fi driver task (IDF) | association, WPA handshakes, PHY | IDF | Priority 23, above every embpet task and the esp_timer task. On a WPA3 network it computes each SAE association in software (no ECC accelerator on the C3): about 85 ms, then about 300 ms and 310 ms in one piece each, at uptime 1.8–4.4 s on boot and, by the IDF code, after `wifi set` and after a drop without a cached PMK. Everything below it stalls meanwhile: input events and the LED come late, frames wait; interrupts stay on, so pulse ends and debounce decisions stay on time (before embpet-jk5.1 a 100 ms pulse measured up to 402 ms). On a WPA2-only network the PBKDF2 PMK derivation stalls 110 ms once per credential set. Fixed priority, not avoidable without dropping WPA3 (README 3.1, embpet-s2j.5). |
| GPIO ISR | raw edge counters, debounce re-arm | ISR | No logging, no frames. The ISR service is installed with `ESP_INTR_FLAG_IRAM`, so it keeps counting and re-arming during flash writes; every handler on it (also the test builds' edge recorders) must be `IRAM_ATTR`. |
| default event loop | Wi-Fi/IP events, reconnect backoff | default | Updates `state` |

- **Shared state:** `proto/state.c`, mutex-protected. It holds wifi {state, ip, rssi}, tg {state, bot}, pending count, booting flag and configured flag. Subscribers get change callbacks. The LED and the `wifi`/`tg`/`pending` events are driven from here.
- **Frames:** `proto_res` / `proto_evt` are callable from any task context. Each frame is written in one piece under a mutex and the stdout lock (`console_out`). A frame is skipped (but kept in the ring) if the USB host isn't connected, so the caller never blocks.
- **Init order** (`app_main`):
  1. Outputs: latch high (off), then set as output. All eight stay off.
  2. NVS and cfg, then apply the `out_boot` levels (they live in cfg, so they cannot be applied in step 1).
  3. proto and state, then the `boot` event.
  4. Inputs and LED (booting = solid on).
  5. Console REPL.
  6. Wi-Fi.
  7. tgbot tasks.
  8. Clear the booting flag, then set the runtime log level to WARN.
- **Memory budget:** about 320 KB heap. Wi-Fi takes about 55 KB. Each TLS session takes 35–45 KB with dynamic buffers, ×2. The getUpdates response buffer is capped at 16 KB. Target: min free heap above 60 KB after a 24 h soak.
  - Measured (embpet-q9h, test build, both sessions open, 1 h of long polling, getMe every 15 s, sendMessage every 10 min): min free heap 116,836 B; free heap 132–134 KB with −124 B drift; largest free block 112 KB. 8 KB worker stacks kept about 5.3 KB free.
  - **Response caps.** A response buffer is allocated per call, sized by Content-Length, and freed after parsing, so nothing is held between calls. The default cap is 16 KB (`TG_RESP_MAX`). A getUpdates batch over the cap is fetched again with `limit` 1 under `TG_RESP_MAX_ONE` = 32 KB. One update can hold an owner reply of up to 4096 UTF-16 units that quotes a bot message of up to 4096 units.
    - If Telegram sends non-ASCII as raw UTF-8, the worst case is 3 bytes per unit (BMP CJK): about 2 × 4096 × 3 + 1.5 KB ≈ 26 KB, which fits.
    - If it escapes non-ASCII as `\uXXXX`, the worst case is 6 bytes per unit: about 51 KB, which does not fit. Allocating that alone would take free heap to about 40 KB while it is parsed, below the target, so this case needs another fix (e.g. a streaming filter that drops `reply_to_message.text` past its `[A]` prefix) rather than a bigger buffer.
    - `tools/tg_test.py api` measures the bytes per unit (response size of long Cyrillic, CJK and emoji texts sent to the owner chat). That measurement is pending: no owner chat is configured yet.
    - An update still over the cap is logged at WARN by `tgapi_get_updates_one`, and the poller skips it with `offset = first_update_id + 1` (embpet-64s).
    - Peak heap of the limit-1 path (estimate): the body buffer (up to 32 KB) and the cJSON tree built from it exist together while parsing. cJSON copies every string, so the tree is about the body size plus about 64 B per node. A worst-case 26 KB update therefore takes about 55 KB for a moment, which brings the measured 116.8 KB minimum down to about 60 KB, right at the target. The buffer is freed right after parsing, and the TLS receive buffer has already been released by then (dynamic buffers). Verify on the device together with the escaping measurement.

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
- **Pairing:** `tg pair` arms a 6-digit code (5 min). `tg_poll` judges every update with `pair_check` BEFORE routing (with no owner, routing would ignore everything). A right `/start <code>` from any chat stores the owner (under the same mutex as `tg owner`, `tg pair` and the token store), emits `paired`, and queues a best-effort plain confirmation for `tg_send`.
- **Online notice:** `tg_send` posts it itself the first time Telegram is online in a boot, if an owner is set then.

---

## 2. Verification

| Level | What | Where |
|---|---|---|
| Unit (host) | All `*_core.c` modules, escape decoder, Bot API JSON and UTF-16 helpers (`tgapi_core`) | `test/host`, run with `scripts/test_host.sh` |
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
