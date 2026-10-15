# embpet — Telegram bridge on ESP32-C3

> **Status: design draft, under review.** No firmware yet. This README defines the board, firmware behaviour, serial protocol and requirements. Implementation tasks will be tracked in beads (`bd ready`).

embpet is a USB-attached ESP32-C3 that lets programs on a host computer ("actors") talk to one person on Telegram, and gives the host some GPIO.

```
 host computer                          ESP32-C3 Super Mini                Telegram
┌──────────────────────┐   USB CDC   ┌──────────────────────────┐  Wi-Fi  ┌──────────┐
│ actor A ─┐           │  (serial    │ console CLI ─ tg bridge ─┼─HTTPS──▶│ bot API  │──▶ owner
│ actor B ─┼─ broker* ─┼─ CLI/JSON)─▶│      │          │        │         └──────────┘   (you)
│ actor C ─┘           │             │   GPIO 0-7 out, 10/20/21 in, LED 8 │
└──────────────────────┘             └──────────────────────────┘
  * host broker is out of scope for now (see Non-goals)
```

Example from the owner's side: actors **A** and **B** each send you a message. In Telegram you use **Reply** on A's message and the answer goes to A. You use **Reply** on B's message and it goes to B. While any message is still unanswered, the board LED shows it.

---

## 1. Hardware

### 1.1 Board

| Item | Value |
|---|---|
| Module | "ESP32-C3 Super Mini" (generic clone). Dev unit verified: ESP32-C3 QFN32 rev v0.4, 4 MB embedded flash (XMC), 40 MHz crystal |
| CPU / RAM | RISC-V single core 160 MHz, 400 KB SRAM |
| Radio | Wi-Fi 4, 2.4 GHz only; BLE 5 (unused) |
| USB | Native USB-Serial/JTAG controller (VID `0x303A`, PID `0x1001`), USB-C. Flashing, console and debugging all go over this one port. No USB-UART chip. |
| Buttons | BOOT = GPIO9, RESET = EN |
| Logic level | **3.3 V only. GPIOs are not 5 V tolerant.** |
| Dev unit | Attached to the dev Mac as `/dev/cu.usbmodem11101` (USB serial `E8:3D:C1:83:83:54`). The port name may change between USB sockets. |

### 1.2 Pin assignment

| GPIO | Function | Notes |
|---|---|---|
| 0 | OUT0 | Alt function XTAL_32K_P (no 32 kHz crystal on this board) |
| 1 | OUT1 | Alt function XTAL_32K_N |
| 2 | OUT2 | **Strapping pin**, must read high at reset or the chip may not boot. Wire its opto in the sink configuration (§1.3). |
| 3 | OUT3 | |
| 4–7 | OUT4–OUT7 | JTAG pins (MTMS/MTDI/MTCK/MTDO); free because JTAG runs over USB |
| 8 | Status LED | Onboard blue LED, **active-low** (verified on the unit: GPIO8 high = dark, low = lit; `CONFIG_EMBPET_LED_ACTIVE_LOW=y`). **Strapping pin**, must be high at reset; leave nothing else on it. |
| 9 | BOOT button | Strapping; reserved |
| 10 | IN10 | |
| 20 | IN20 | U0RXD. Sits next to the antenna, so wires here can detune Wi-Fi. |
| 21 | IN21 | U0TXD. **The ROM prints its boot log on this pin at reset**, so an energised input opto briefly contends with it (limited by the 1 kΩ series resistor in §1.3). Also next to the antenna. |
| 18 / 19 | USB D− / D+ | Reserved |

Output numbers equal GPIO numbers (`OUT3` is GPIO3); inputs are addressed by GPIO number (10, 20, 21).

### 1.3 Optocoupler interface (PC817C)

All inputs and outputs are isolated with PC817C optocouplers (CTR 200–400 % at I<sub>F</sub> = 5 mA, V<sub>F</sub> ≈ 1.2 V).

**Outputs: sink configuration, mandatory for all of OUT0–7.**
```
3V3 ── 390 Ω ── opto LED anode ─|>|─ cathode ── GPIOn        (GPIO low = opto ON)
```
- I<sub>F</sub> ≈ (3.3 − 1.2) / 390 ≈ 5.4 mA per channel, which is well inside the C3's drive capability. All 8 channels on draw about 43 mA from 3V3.
- **Requirement: every output is off from power-up/reset until the firmware sets its boot level.** Sink wiring meets this with no extra components:
  - In reset, the ROM and the bootloader leave GPIO0–7 high-impedance or weakly pulled up. With the LED between 3V3 and the pin there is then no current path, so the opto is **off**.
  - GPIO2 sits at about 3V3 through the LED path, so the strapping pin reads high and the board boots normally. **GPIO2 therefore stays usable.**
  - Source wiring (GPIO → R → LED → GND) fails on both counts. A weak internal pull-up pushes about 50 µA through the LED, so the opto is partly on during boot. On GPIO2 the LED also clamps the pin to about 1 V, so the strapping pin reads low and the board may not boot.
- **Firmware rule:** set the pin's latch to the "off" level (high) *before* enabling it as an output, so there is no glitch at init. The per-pin boot level (§2.4) is applied right after that, early in `app_main`.
- The firmware works in **logical** levels (`on` = opto conducting = pin low).
- **Bench check, once per board:** hold RESET, then release it, and confirm no output conducts during reset or boot. This rules out a pin with a reset-time pull-down, which isn't documented for GPIO0–7 but isn't confirmed either.

**Inputs: opto transistor pulls the pin low.**
```
3V3 ── 10 kΩ ──┬── 1 kΩ ── GPIOn          field side: V_in ── R_F ── opto LED ── GND_field
               └── collector, emitter ── GND
```
- Opto energised means the transistor conducts, the pin reads **low**, and the input is reported as `active: 1`.
- Use an external pull-up of 4.7–10 kΩ. The internal ~45 kΩ pull-up (still enabled) makes PC817 turn-off slow and noise-prone.
- Choose the field-side R<sub>F</sub> for I<sub>F</sub> ≈ 5 mA at the field voltage (e.g. 24 V → 4.7 kΩ, 12 V → 2.2 kΩ, 5 V → 680 Ω).
- The 1 kΩ series resistor protects GPIO21 (ROM UART TX at boot) and GPIO20.
- PC817 switching takes tens of µs, which is negligible next to the 20 ms default debounce.

**Other notes**
- An energised input opto on **GPIO20/21 at reset** only disturbs the ROM log; it doesn't affect boot. Don't put inputs on GPIO2, 8 or 9.
- Keep opto wiring away from the antenna end of the board (GPIO20/21 side).
- Known Super Mini issue: many units drop Wi-Fi at the default 19.5 dBm TX power because of a weak regulator or antenna. The firmware defaults to **8.5 dBm**, adjustable with `wifi txpower`.

---

## 2. Firmware

ESP-IDF **v5.5.x**, plain C, no Arduino. The Telegram client is the project's own component (`esp_http_client` + `esp_crt_bundle` + cJSON).

### 2.1 Telegram bridge

- **One owner chat.** The bot serves exactly one Telegram `chat_id`. Updates from any other chat are ignored and not answered.
- **Binding the owner:** `tg pair` (needs a stored bot token) prints a random 6-digit code, valid 5 min. The owner sends `/start <code>` to the bot from the chat that should receive the messages (`tools/tg_pair.py` runs the whole flow). The poller accepts that message from any chat while pairing is active: it stores the chat as owner, emits `paired {chat_id}`, and the bot confirms there in plain text (best effort, one try; the `paired` event is authoritative). Pairing then ends. A wrong or expired code is ignored without an answer, and `/start@<other_bot>` commands are not attempts. 5 wrong codes cancel pairing, and setting a new token, setting or clearing the owner, or a new `tg pair` voids the earlier code. Running `tg pair` again re-binds the owner. Known limitation: anyone who knows the bot can send wrong codes and cancel the pairing; the fallback is a new `tg pair` or `tg owner <chat_id>`. Alternatively set the ID directly with `tg owner <chat_id>`.
- **Setting the token:** `tg token <token>` asks Telegram (`getMe`) whether it accepts the token and stores it only if so, so Wi-Fi must be up; a rejected token gives `auth_error` and the old one stays. The bridge then restarts the poller (`deleteWebhook` included). The response and the logs show the bot name and the masked token only; the host must send the token without it appearing in `argv` (`tools/tg_from_env.py`).
- **Outgoing:** `tg send -a A "text"` posts `[A] text` to the owner. The actor tag is rendered bold through Telegram `entities`, with no parse_mode, so message text needs no escaping. Plain text only, up to 4096 bytes UTF-8, and the whole message `[A] text` must also fit Telegram's limit of 4096 characters counted in UTF-16 code units (an emoji outside the BMP counts 2; the tag and its space count 3 + the actor length). Text over either limit is rejected (`err: too_long`), so with a 16-character actor at most 4077 ASCII characters fit.
- **Threading:** `tg send -a A -r <msg_id> "text"` sends the message as a Telegram reply to an earlier message, e.g. continuing a conversation with the owner.
- **Incoming (routing):**
  1. If the owner uses **Reply** on a bot message, the firmware looks up the replied-to `message_id` in the pending table and emits a `reply` event for that actor.
  2. If the message isn't in the table (after a reboot or eviction), the actor is recovered from the `[A]` prefix of the replied-to text, which Telegram includes in the update. The event then carries `"src":"prefix"`. Routing therefore survives reboots even though the table lives in RAM. Only a replied-to message written by this bot counts (its `from.id` equals the bot's user id, the digits of the token before `:`), so a Reply to the owner's own `[A] …` text, or to another bot's, is not routed.
  3. If the owner writes **without Reply**, or replies to a message that carries no actor (e.g. the hint itself), the bot answers, as a Telegram reply to that message: *"Use Reply on an actor's message so I know who it's for."* No event goes to the host.
  - A reply that is a photo or another non-text message routes the same way; its `text` is the caption, or empty.
- **Pending ("unanswered") table:** RAM only, 64 entries. An entry is created by each `tg send` and becomes *answered* on the first owner reply to it, or through `tg ack`. When the table is full, the oldest answered entry is evicted first. If every entry is still pending, the oldest pending entry is dropped and a `pending_dropped` event is emitted. Ages use uptime (monotonic), so no SNTP is needed.
- **Receiving:** long polling `getUpdates` (timeout 50 s, `allowed_updates=["message"]`) on its own task and HTTPS connection. Sends use a second keep-alive connection. On HTTP 429 the client honours `retry_after`. Polling starts once `getMe` and `deleteWebhook` have succeeded for the stored token. After a failed poll or a Wi-Fi drop the next `getUpdates` uses timeout 0, and after a Wi-Fi reconnect the send task probes once with `getMe`, so the bridge is `online` again within seconds rather than after the next long poll. Failed polls back off 1, 2, 4, … 30 s.
- **Hint rate:** at most one hint reply per 10 s; further non-routable owner messages in that time get none. `tg send` requests go ahead of a queued hint.
- **Delivery semantics:** at-least-once. An update's offset advances only after its `reply` event has been emitted (written to the host if one is connected, and stored in the event ring), and Telegram drops the update only when the next getUpdates confirms that offset. Telegram keeps unconfirmed updates for 24 h, so replies sent while the board was off arrive after boot. A reboot or crash between emitting and confirming delivers the reply again after boot (then with `"src":"prefix"`, since the table is gone); hosts dedupe by the reply's `msg` id.
- **Online notice:** after each boot, when Telegram is first reachable and an owner is set, the bot posts `embpet online · fw <version> · reset <reason>` (plain text; transient failures are retried, up to 3 attempts in all). Not sent on later reconnects, and not at all in a boot where no owner was set at that moment (pairing later in the boot gets the confirmation only).
- **TLS:** server verification uses the IDF certificate bundle, with no pinned certificate. The default common-CA bundle (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN`) validates `api.telegram.org` (verified on the device). mbedTLS dynamic buffers are on to keep idle RAM low.
- **Responses** larger than 16 KB are rejected; a getUpdates batch that is too large is fetched again one update at a time with a larger cap (docs/PLAN.md 1.2). HTTP 401/404 from the Bot API mean the token was rejected (`auth_error`); on 429 the client waits `retry_after`; network errors back off 1, 2, 4, 8, 16 s, then every 30 s.

### 2.2 Status LED (GPIO8)

Priority from top to bottom; the first matching row wins.

| State | LED |
|---|---|
| Booting (from LED init during startup until Wi-Fi and the Telegram bridge are started) | solid on |
| Not configured, or Wi-Fi/Telegram down (including no token) | blink 1 Hz (500 ms on / 500 ms off) |
| ≥1 unanswered | 1 s burst at 5 Hz (5 × 100 ms on / 100 ms off), then 3 s off; 4 s cycle |
| Online, nothing pending | off |

While offline the unanswered pattern is hidden; the `pending` count is still available from `tg pending`.

### 2.3 Inputs

GPIO 10, 20 and 21, wired through optos (§1.3). Active means the opto is energised and the pin is low.

- **Debounce:** an any-edge GPIO interrupt (re)starts a per-pin debounce timer. A change is accepted once the level has been stable for the debounce time. Default 20 ms, per pin 0–1000 ms, set with `io debounce` and stored in NVS.
- **Debounce 0:** the level is sampled about 1 ms after the first edge (one storm window), so bounces inside that millisecond are folded into `extra`. An input reports at most 1000 events per second.
- **Edge storms:** the ISR always counts the edge but re-arms the timer at most once per millisecond, so a storm cannot keep the CPU busy; the event follows once the level has been stable for the debounce time.
- **Events:** each accepted change emits an `input` event (`pin`, `active` as `1`/`0`, `extra`). There is no event at boot for an input that is already active.
- **`extra`** is the number of raw edges since the previous `input` event for that pin, minus the one edge that produced the change. A clean change gives `0`. Contact bounce gives an even number (2, 4, …). A glitch shorter than the debounce time, which produces no event, is counted into the next event's `extra`. A host detects lost events through gaps in the frame `seq`.
- `io in` returns `active` (`1`/`0`, the last reported state), raw `level` and `extra` accumulated so far since the last event, for each input. `io debounce <pin> <ms>` takes pin 10, 20 or 21 (else `err:"bad_pin"`) and 0–1000 ms (else `err:"bad_args"`); with no arguments it lists the three times.

### 2.4 Outputs

- `io set <pin> on|off|toggle` sets a level. It cancels any running pulse on that pin.
- `io pulse <pin> [-d ms]` inverts the current level for `ms` (default **100**, range 1–600000), then restores it. Re-pulsing a pin that is already pulsing restarts the timer, and the original level is still the one restored.
- `io get` reports each pin's level, whether it is pulsing, and the remaining pulse time in ms. `io set` and `io pulse` answer with the pin's resulting level (`pulse`: the level during the pulse, plus `remaining_ms`).
- A pin that is not an output (not 0–7) gives `err:"bad_pin"` (a negative pin looks like an option and gets `bad_args`); `io pulse -d` outside 1–600000 gives `err:"bad_args"`.
- `io boot <pin> on|off` stores the logical level a pin takes at app start (NVS, default `off`). `io boot` with no arguments lists them.
- All output levels are **logical**: `on` = opto conducting, regardless of wiring polarity (§1.3).

### 2.5 Configuration storage (NVS)

Wi-Fi SSID and password, Wi-Fi TX power, bot token, owner `chat_id`, per-pin boot levels, and per-input debounce times. Secrets are never echoed back: `wifi status` shows the SSID only, and `tg status` shows a masked token (`123456:AA…xyz`). NVS encryption is a non-goal for now. The settings carry a schema version; if the stored version differs from the firmware's, including after a downgrade, the `embpet` NVS namespace is erased and all settings return to their defaults.

---

## 3. Serial protocol

### 3.1 Transport
- USB-Serial/JTAG CDC port; the baud rate is irrelevant. The firmware runs its own small REPL with prompt `embpet> `, and the maximum command line is **4400 bytes** (a longer line is answered with `err:"too_long"`). It works like linenoise's "dumb" mode: no escape sequences, the input is echoed, BS/DEL erases one character, Ctrl-U (0x15) discards the whole line (echoed as `^U` and a new prompt on a fresh line; inside quotes too, as quotes are only looked at when the line ends), and other control bytes except TAB are ignored. A line ends with CR, LF or CRLF; blank lines get no response. `esp_console`'s REPL is not used because in IDF 5.5.2 linenoise strips every byte that isn't `isprint()` (all UTF-8 bytes), `esp_console_split_argv` drops unknown escapes such as `\n` together with their character, and it answers unknown commands with plain text rather than a frame. Options are still parsed with argtable3. Input is flow-controlled, so nothing the host sends is lost while a command runs (§3.2).
- **Port-open reset (verified on macOS, pyserial 3.x; run counts per case below):** opening the port can reset the C3 or hold it in reset, depending on the DTR/RTS order.
  - Default pyserial open (DTR and RTS asserted): the chip stays **silent for as long as the port is open**, presumably because RTS holds EN low. 5/5 runs.
  - `dtr = rts = False` set *before* `open()` still **resets the chip on every open** (`rst:0x15 USB_UART_CHIP_RESET`, uptime restarts at 0): 5/5 runs, and 4 further opens in the closed-port runs. macOS asserts both lines on `open()`, and pyserial then clears DTR first and RTS second, so the chip sees (DTR,RTS) = (1,1), (0,1), (0,0); the likely cause is the (0,1) step.
  - **No reset:** clear **RTS first**, then DTR (skip pyserial's DTR update in `open()`, then clear RTS, then DTR). In about 15 such opens (including 10 s closed periods) the chip kept the same boot, with continuous uptime.
  - The first open after any chip reset (flash, reset) delivered its first byte only after about 2.5 s; later opens deliver within 0.1 s.
  - A host program must therefore use the RTS-first order, or accept one reset per command. `tools/embpet_cli.py` does: it opens with `dsrdtr=True`, so pyserial's `open()` clears only RTS, and then it clears DTR. That gives (1,1), (1,0), (0,0). In 10 consecutive CLI runs the chip kept the same boot and its uptime kept rising, and `--reset` still resets it.
- **Log sink: USB-Serial/JTAG is the primary and only console**, for both the 2nd-stage bootloader and the app (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`, `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`). UART0 is never initialised as a console, so GPIO20/21 are free for inputs. The 1st-stage ROM boot log still goes to UART0 (GPIO21) as well as USB. That's left as is: no eFuse changes.
- **Port closed (verified, 10 s closed, 2 cycles, both console modes):** nothing blocks for long and nothing is replayed.
  - Output written while no program has the port open is **dropped**; after reopening, the stream continues at the current line (no backlog).
  - The first write after the host stops reading stalls the writer for the TX timeout, **40–50 ms** (IDF `TX_FLUSH_TIMEOUT_US` = 50 ms), once per close. All later writes are fast: about 0.2 ms per 70-byte line without the USB-Serial/JTAG driver, about 0.7 ms with the driver through the IDF VFS, which hands the driver one byte at a time. Worst-case writer stall: **≈50 ms**. A same-priority task kept its 10 ms cadence during the stall (no heartbeat gap above 10 ms). A lower-priority task was not measured; on the single core it could in principle be delayed by up to the stall.
  - `usb_serial_jtag_is_connected()` follows USB start-of-frame packets, not the open state of the port: it stayed **true while the port was closed** (the cable stays enumerated), and was false on only 1 of about 7000 writes (a transient at reconnect). Do not use it to detect a listening host; it only skips writes while the cable is unplugged (the VFS write then returns -1 at once).
  - **The embpet console bypasses the VFS** (embpet-s2j.4): frames, log lines, the echo, the prompt and the `help` text go to the driver in whole chunks of up to 512 bytes (usually one write per line), with LF sent as CRLF exactly as the VFS did. It keeps the VFS rules above: nothing is written while the cable is unplugged, a chunk that does not fit waits up to 50 ms once per stall, and later chunks are then dropped at once until one fits again. A driver write is all or nothing, so dropped output is whole chunks, not single bytes, and the first dropped chunk ends that write. A frame cut by a stall is truncated, never spliced; hosts see an unparsable line. About 15 µs per event frame (from `evt since 0`).
  - Design consequence: writing one frame under the output lock holds the lock for at most about 50 ms per host stall.
- The console also carries ESP-IDF log lines, the echoed input and the prompt. **Hosts must ignore every line that does not start with `@@`.** The console log level is `WARN` once init is done and can be changed with `sys log`. The HTTP client, transport and TLS log tags never go above `WARN`, whatever `sys log` says, because `esp_http_client` logs the request line, which carries the bot token, at `DEBUG`.
- **Log lines never wait for the host:** the log sink drops whole lines while the cable is unplugged or the driver's 2 KB TX buffer is full (the host stopped reading), or when the line does not fit into it, and counts them (`sys info` → `log_dropped`). Frames are written normally. The output that fills the buffer can still pay the one 40–50 ms stall above. Measured with a soak build (an event and a log line every 100 ms, port closed for 10 min): longest event write 51 ms, longest log write 49 ms, the generator's 100 ms loop never late, and `evt since` worked after reopening. With the batched output (embpet-s2j.4), port closed for 60 s and then 120 s on one boot: longest event write 50.0 ms, longest log write 49 ms (one of the two paid the stall at each close), longest loop period 101 ms, 1841 log lines counted as dropped. The checked maxima count from uptime 15 s: before that there is a boot-time stall of about 0.3 s with either output path, cause not yet isolated (embpet-s2j.5); the soak events report it separately as `boot_max_*`.
- **Command floods leave the idle task a share of the CPU** (embpet-94q). Each command costs the console CPU, and while input is waiting the console never blocks. Console CPU per command, measured with the probe test build (`components/proto/probe.defaults`, `tools/console_cpu.py`: 200 lockstep commands, uptime over 20 s), before → after the batched output of embpet-s2j.4: `sys info` 3.9 → 1.5 ms, `help` 29.4 → 5.3 ms, `evt since 0` (33 frames) 6.0 → 1.0 ms. A command with an empty `res` costs about 0.57 ms (line editing, argtable, frame rendering); the rest of `sys info` is its handler and JSON.
  - After each command and each input chunk, once the idle task has not run for 100 ms, the console sleeps tick by tick (up to 10 ms each) until the idle task has run, for at most 100 ms.
  - This leaves lower-priority tasks (TLS handshakes) and the idle task a share of the CPU. A task-watchdog report then points at a task that hogs the CPU itself.
  - The cost is latency while lower-priority work is CPU-bound: during the Telegram TLS handshakes after boot, a command can wait up to about 0.2 s, or about 0.5 s when several are queued.
  - Measured with `scripts/hw.sh python tools/wdt_repro.py [--cmd ...] [--depth 8]`, flooding from uptime 2 s to 17 s after `sys reboot`. Every run was clean: no watchdog, no lost or bad frame. Rates before embpet-s2j.4 in brackets.

    | Command | In flight | Runs | Rate | Max latency |
    |---|---|---|---|---|
    | `sys info` | 1 | 5 | about 430/s (215) | 0.20 s |
    | `sys info` | 8 | 3 | about 590/s (215) | 0.51 s |
    | `help` | 1 | 2 | about 170/s (32) | 0.21 s |
    | `help` | 8 | 2 | about 174/s (31) | 0.55 s |
    | `evt since 0` | 1 | 2 | about 645/s (155) | 0.20 s |
    | `evt since 0` | 8 | 2 | about 888/s (150) | 0.51 s |

    A probe build found the longest stretch without the idle task: 0.5–0.6 s during the handshakes (0.47 s with no traffic at all). Outside them, a lockstep host now leaves the idle task about 30% of the CPU (gaps of 1–4 ms, so the 100 ms wait no longer triggers); with 8 commands in flight the console still takes about 90%, and the wait keeps the gaps at about 0.1 s.
  - Before the fix, the idle task got no CPU for 5 s and the watchdog fired. A lockstep host tripped it at about 10 s of uptime, while the TLS handshakes ran. A host with 8 commands in flight tripped it at any uptime.
  - Task-watchdog and panic reports bypass the frame lock: they are written raw from interrupt or exception context. If one appears, it can cut the frame being written. That frame fails to parse, so the host sees a missing `res`, and after a panic also a new boot id.

### 3.2 Host → FW: plain commands

Every command is a normal console line. Two options are accepted on **every** command and echoed back in its response:

| Option | Meaning |
|---|---|
| `-a <actor>` | Actor ID, `[A-Za-z0-9_-]{1,16}`. **Required** for `tg send`; optional elsewhere. |
| `-i <rid>` | Request ID chosen by the host, `[A-Za-z0-9_-]{1,32}`, for matching responses. |

Text arguments are double-quoted. Inside quotes, `\"`, `\\` and `\n` are escapes, and UTF-8 passes through as-is. Any other escape, a missing closing quote, a backslash at the end of the line, or text glued to a closing quote is rejected with `err:"bad_args"`. Outside quotes, backslashes and quotes are ordinary characters. Arguments are separated by spaces or tabs; inside quotes a tab is ordinary text. A text argument that starts with `-` must come after `--`, or it is taken as an option. `tools/embpet_cli.py` does this for everything after its own `--`: `embpet_cli.py wifi set -- "my ssid" "-pass"` sends `wifi set -i <rid> -- "my ssid" "-pass"`.

`-a` and `-i` are echoed back even when the command fails, as long as their values are valid. That covers an unknown command, a bad option or a bad escape. A response to an invalid `-i` value has no `rid`. When quoting fails or the line is too long, only the options **before** the faulty spot are seen, so put `-a` and `-i` right after the command words.

Host rules:
- Match each response to its command by `rid`, not by position. Echoed input, log lines and event frames can be interleaved with responses.
- At session start send Ctrl-U (byte `0x15`) then a newline (`b"\x15\n"`; `tools/embpet` does this when a `Board` opens). A partial line left by a previous session would otherwise be joined to the new session's first command, and a bare newline would run it as a command whose `res` has no `rid` from this session. Ctrl-U discards that partial line (it clears the line buffer, and bytes still in the receive buffer are processed in order, so it also kills a leftover that was queued); the newline then ends the empty line, which gets no response. The firmware echoes `^U` and a new prompt; hosts ignore non-frame lines anyway. Ctrl-U cannot be sent inside a command argument (`tools/embpet` rejects control characters).
- Commands may be pipelined; input is flow-controlled and never dropped (embpet-s2j.3). Commands run one at a time, in order. While one runs (e.g. `tg send`, up to 10 s), input waits in a receive buffer of 4400 + 256 bytes. When that is nearly full, the firmware stops accepting USB packets: the host's USB stack holds them and retries, so the host's write blocks (or its OS buffers the data) until the console reads again. A host that writes with a timeout must allow for the longest command ahead of it. **A pipelining host must keep reading while it writes** (a separate reader thread, or non-blocking I/O): the echo is as long as the input, and output the host does not read is dropped after the one 50 ms stall (§3.1), so `res` frames would be lost, and they cannot be replayed. If the RX flow control ever loses its re-enable, the console restores it within 1 s (its read timeout). Measured with `tools/console_test.py pipeline` (probe build): 10 KB, 21 KB (500 short lines) and 160 KB (40 lines of 4 KB) written behind a 10 s blocking command all arrived whole and were answered in order; 200 KB with no blocking command took 0.7 s.

Plain commands for everything, including `tg send`, mean the firmware needs only one input parser (argtable3), and every command can still be typed by hand in a terminal.

### 3.3 FW → host: JSON frames

Every machine-readable output is one line: `@@` + a compact JSON object. Each frame is written as `\n@@{…}\n` in one piece while holding the stdout lock (§3.1). The leading `\n` ends any prompt or partial line before it, so a frame always starts a line (regex `^@@(\{.*\})\r?$`), and no log line can split it.

Common fields: `t` (`res` | `evt`), `seq` (per-boot frame counter, starting at 1), `boot` (random 32-bit hex boot ID, so a host can detect resets).

**Responses** (`t:"res"`). Exactly one per command, emitted after the command finishes:
```
@@{"t":"res","seq":15,"boot":"9f3a11c2","cmd":"tg send","rid":"7","actor":"A","ok":true,"msg":1234}
@@{"t":"res","seq":16,"boot":"9f3a11c2","cmd":"io set","rid":"8","ok":false,"err":"bad_pin","msg":"pin 9 is not an output"}
```
`tg send` blocks until Telegram answers (timeout 10 s). It fails immediately, without contacting Telegram, in this order: `bad_args` / `too_long` for the text (invalid UTF-8, over the limits of §2.1); `not_configured` if the Wi-Fi credentials, the bot token or the owner chat are missing; `offline` if Wi-Fi or Telegram is down; `auth_error` if Telegram rejected the token. Telegram's own answers map as: network or 5xx → `offline`, no answer within 10 s → `timeout`, 429 → `busy` (later sends also get `busy` until `retry_after` has passed), 403 (bot blocked) or 400 "chat not found" → `not_configured`, other 400s → `bad_args`. After a `timeout` the message may still be delivered; it is then added to the pending table like any sent message (a `pending` event shows it), and a reply to it routes normally.

Error codes (`err`), with a human-readable `msg` alongside:

| `err` | Meaning |
|---|---|
| `bad_args` | Unknown command, unknown or missing option or argument, invalid `-a`/`-i` value, bad quoting or escape, value out of range |
| `bad_pin` | Pin number isn't valid for this operation (e.g. not an output) |
| `too_long` | Command line over 4400 bytes, or text over its limit (`tg send`: 4096 bytes, and 4096 UTF-16 code units for the whole `[A] text`) |
| `offline` | Wi-Fi or Telegram is down |
| `not_configured` | Required setting missing (Wi-Fi credentials, bot token, owner) |
| `auth_error` | Telegram rejected the bot token |
| `timeout` | No answer in time (e.g. Telegram within 10 s) |
| `busy` | The operation can't run right now (e.g. another one is in progress) |
| `internal` | Firmware error (out of memory, or a handler that sent no response) |

The `cmd` of an unknown command echoes the typed command words (e.g. `"sys nope"`) when they are plain ASCII words; otherwise `cmd` is omitted.

**Events** (`t:"evt"`). Asynchronous:

| `ev` | Fields | When |
|---|---|---|
| `boot` | `fw`, `idf`, `reset_reason` | Once after start |
| `wifi` | `state` (`up`/`down`), `ip`, `rssi` (`ip` and `rssi` only when `up`) | Wi-Fi goes up or down, or the IP changes; not on every reconnect attempt |
| `tg` | `state` (`online`/`offline`/`unconfigured`/`auth_error`), `detail` | Telegram reachability changes (state, or the bot behind the token). `detail` is a short reason for humans, e.g. `@<bot>`, `wifi down`, `token changed`, `timeout`, `network: <error>`, `server error 502`, `401 Unauthorized`, `no token` |
| `reply` | `actor`, `to` (msg_id replied to), `msg` (owner's msg_id), `text`, `date` (unix), `src` (`table`/`prefix`) | Owner replied to an actor's message |
| `paired` | `chat_id` | Owner bound with `/start <code>` |
| `pending` | `count` | Unanswered count changed |
| `pending_dropped` | `actor`, `msg` | Table overflow |
| `input` | `pin`, `active`, `extra` | Debounced input change |

The last 32 events are kept in RAM. After a host reconnects it can replay them with `evt since <seq>`.

### 3.4 Command reference

| Command | Purpose | Response payload |
|---|---|---|
| `tg send -a A [-r MSG] "<text>"` | Send to the owner as actor A | `msg` (Telegram message_id) |
| `tg pending [-a A]` | List unanswered | `items:[{msg,actor,age_s}]` |
| `tg ack --older <s>` / `tg ack -m <msg>` | Mark answered: all older than *s* seconds (`0` = all), or one message | `acked` (count) |
| `tg token <token>` | Check the bot token with Telegram, store it and restart the poller (errors: `bad_args` malformed, `offline` Wi-Fi/Telegram down, `auth_error` rejected, `timeout`, `busy` send queue full, `internal`) | `bot` (username, from `getMe`), `token` (masked) |
| `tg pair` | Start owner pairing (`not_configured` without a token) | `code`, `expires_s` |
| `tg owner [<chat_id>\|--clear]` | Show, set or clear the owner chat (a negative id goes after `--`) | `chat_id` (`null` if unset) |
| `tg status` | Bridge status | `state`, `detail` (as in the `tg` event), `bot`, `owner` (`null` if unset), `token` (masked, `""` if unset), `pending` |
| `wifi set "<ssid>" ["<pass>"]` | Store credentials; reconnects at once (the res does not wait for it; omit the password for an open network) | — (then `wifi` event) |
| `wifi status` | Connection info | `state` (`up`/`down`/`unconfigured`), `ssid` (empty if unset), `txpower` (dBm); `ip` and `rssi` only when `up` |
| `wifi txpower <dBm>` | TX power, 2–20 dBm in 0.25 steps (default 8.5), stored and applied at once | `txpower` |
| `io in` | Read inputs | `in:[{pin,active,level,extra}]` |
| `io debounce [<pin> <ms>]` | Show or set debounce | `debounce:{"10":20,...}` |
| `io set <pin> on\|off\|toggle` | Set an output | `pin`, `level` |
| `io pulse <pin> [-d ms]` | Pulse-toggle an output (default 100 ms) | `pin`, `level`, `remaining_ms` |
| `io get` | Output states | `out:[{pin,level,pulsing,remaining_ms}]` |
| `io boot [<pin> on\|off]` | Per-pin boot level | `levels:{"0":"off",...}` (not `boot`: that key is the frame's boot id) |
| `evt since <seq>` | Replay buffered events with seq > `<seq>` (`0` = all), verbatim with their original seq | events re-emitted, then `res` with `count` and `truncated` (true if some newer events had already left the buffer) |
| `sys info` | Versions, uptime, heap, MAC | `fw`, `idf`, `uptime_s`, `heap_free`, `heap_min`, `mac`, `reset_reason`, `log` (level), `log_dropped` |
| `sys log <none\|error\|warn\|info\|debug>` | Console log level (not stored; `warn` after each boot) | `level` |
| `sys reboot` | Restart | `res`, then reboot after about 100 ms |
| `help` | Print every command's syntax (plain text, for humans) | `cmds` (names) |

Wi-Fi behaviour: the station reconnects with exponential backoff (1, 2, 4, 8, 16 s, then every 30 s), reset by a successful connection or by `wifi set`. With no SSID stored it makes no connection attempts (`unconfigured`). The driver keeps credentials in RAM only (`WIFI_STORAGE_RAM`); the NVS config store is the single place they are saved.

### 3.5 Example: two actors

```
> tg send -a A -i 1 "Build #42 failed on step 'test'. Retry?"
@@{"t":"evt","seq":20,"boot":"9f3a11c2","ev":"pending","count":1}
@@{"t":"res","seq":21,"boot":"9f3a11c2","cmd":"tg send","rid":"1","actor":"A","ok":true,"msg":501}
> tg send -a B -i 2 "Disk at 91% on nas01. Clean /tmp?"
@@{"t":"evt","seq":22,"boot":"9f3a11c2","ev":"pending","count":2}
@@{"t":"res","seq":23,"boot":"9f3a11c2","cmd":"tg send","rid":"2","actor":"B","ok":true,"msg":502}
                                     ... LED: 5 Hz bursts (2 pending)
   owner: Reply on [B] → "yes"
@@{"t":"evt","seq":24,"boot":"9f3a11c2","ev":"reply","actor":"B","to":502,"msg":503,"text":"yes","date":1760450100,"src":"table"}
@@{"t":"evt","seq":25,"boot":"9f3a11c2","ev":"pending","count":1}
   owner: Reply on [A] → "no, check flaky test first"
@@{"t":"evt","seq":26,"boot":"9f3a11c2","ev":"reply","actor":"A","to":501,"msg":504,"text":"no, check flaky test first","date":1760450130,"src":"table"}
@@{"t":"evt","seq":27,"boot":"9f3a11c2","ev":"pending","count":0}
                                     ... LED off
```

The `pending` event of a send comes before its `res` (the send task records the message as soon as Telegram confirms it); a `reply` event comes before the `pending` event of the message it answers.

---

## 4. Requirements

### Build host (macOS, Apple Silicon)
- Homebrew packages: `cmake ninja dfu-util`, plus git and python3 ≥ 3.9 (the Xcode CLT versions are fine).
- ESP-IDF **v5.5.2**, installed with `scripts/bootstrap.sh` as a self-contained directory: checkout, toolchains, Python venv and caches all live under one root, and nothing goes to `~/.espressif` or user caches. Use one root per version so several versions can coexist:
  ```bash
  scripts/bootstrap.sh ~/code/esp-idf_5.5.2 -v v5.5.2 -t esp32c3   # once; safe to re-run
  ```
  Removing the version is `rm -rf ~/code/esp-idf_5.5.2`.
- Python tooling (pyserial) comes from the ESP-IDF venv. `tools/embpet_cli.py` runs with that interpreter.

### Network and accounts
- 2.4 GHz Wi-Fi with WPA2- or WPA3-Personal. No enterprise auth and no captive portal.
- Outbound TCP 443 and DNS to `api.telegram.org`.
- A Telegram bot token from **@BotFather**. The bot must not have a webhook set, because long polling fails while a webhook exists. The firmware calls `deleteWebhook` once at start.

### Secrets for development
`.env.local` in the repo root is git-ignored and holds `WIFI_SSID`, `WIFI_PASS` and `TG_TOKEN`. Keys are listed in `.env.example`. Skills and tools read from it, and nothing secret is committed.

---

## 5. Build and flash

```bash
. scripts/export.sh                  # default root ~/code/esp-idf_5.5.2, or pass another
idf.py set-target esp32c3            # once
idf.py build
idf.py -p /dev/cu.usbmodem11101 flash monitor
```

`sdkconfig` is generated and git-ignored; `sdkconfig.defaults` is the source of truth
(delete `sdkconfig` and rebuild to pick up changes to the defaults). The firmware version
is `git describe --always --tags --dirty`, so an untagged repo yields a bare commit hash.

Warnings: all project components build with `-Wall -Wextra -Werror` plus `-Wsign-compare`
and errors for unused variables and functions. They are set by `embpet_component_warnings()`
in `cmake/embpet_warnings.cmake`, and the host tests use the same set. One difference: unused
parameters are only flagged on the host, because IDF's own inline headers trip
`-Wunused-parameter` on target.

Provisioning is done through the CLI; see the `wifi-connect` skill in `skills/`.

### Testing

Pure logic (escape decoding, pending table, prefix parsing, pulse/debounce, LED pattern,
JSON frame building) lives in `components/<comp>/*_core.c/.h` with no ESP-IDF includes;
time is passed in as `uint64_t now_ms`. These are unit-tested natively on the Mac:

```bash
scripts/test_host.sh     # configure + build in build-host/, run via CTest; non-zero on failure
```

The harness is a standalone CMake project in `test/host/` using Unity and cJSON from
`$IDF_PATH/components`. To add a test, create `test/host/<name>/test_<name>.c` (Unity, own
`main()`) and register it in `test/host/CMakeLists.txt` with `embpet_add_host_test(NAME ...
SOURCES ... INCLUDES ... [WITH_CJSON])`. Host tests are built with `-Wall -Wextra -Werror`.
They default to a Debug build with AddressSanitizer and UBSan (`-fsanitize=address,undefined`, UB is fatal); LeakSanitizer is unavailable on macOS ARM. On toolchains without the sanitizer runtimes, configure with `-DEMBPET_HOST_SANITIZE=OFF`.

**Hardware-in-the-loop** (production firmware on the attached board; `scripts/dev-venv.sh` first, details in `test/hil/README.md`):

```bash
scripts/hw.sh python tools/hil.py -m "not owner and not loopback"   # default selection, about 2 min
scripts/hw.sh python tools/hil.py -m owner      # real sends to the paired owner; asks the human to reply in Telegram
scripts/hw.sh python tools/hil.py -m loopback   # input events; needs OUTn -> opto -> IN wiring (EMBPET_LOOPBACK, default 0:10,1:20,2:21)
```

The suites are `protocol`, `io`, `wifi` and `tg` (the Wi-Fi-down paths run in one shared outage). Tests marked `owner` or `loopback` are skipped unless selected with `-m` (or `EMBPET_HIL_OWNER=1` / `EMBPET_HIL_LOOPBACK=1`). Settings the tests change (boot levels, debounce, outputs, log level, TX power, owner, Wi-Fi and token from `.env.local`) are restored afterwards, and the NVS is never erased; hard kills (SIGKILL, a dropped port) cannot be undone by fixtures, and a board with no Wi-Fi SSID ends up on the `.env.local` network (see `test/hil/README.md`). Do not pass `-l`, `--showlocals` or `--tb=long`: they print fixture locals, which include the `.env.local` secrets. Not covered by the default run: `evt since` with `truncated: true` (loopback only), reply routing, the online notice and a valid `tg token` (owner suite and `tools/pair_test.py` only). Telethon automation of the owner replies is not implemented.

---

## 6. Planned repo layout

```
main/                 app_main, console command registration
components/tgbot/     Telegram client: poller, sender, JSON, pending table
components/proto/     @@ frame writer, event ring, common -a/-i options
components/io/        outputs (pulse timers), inputs, boot levels, status LED
components/netcfg/    Wi-Fi station + NVS config
main/Kconfig.projbuild  "embpet" menu (limits and defaults)
partitions.csv        nvs, phy_init, factory (3 MB, no OTA)
sdkconfig.defaults    build configuration (sdkconfig itself is generated)
cmake/                embpet_warnings.cmake, the shared warning policy
tools/embpet/         Python library: reset-free serial open, @@ frame parser, quoting, Board.cmd() / Board.events()
tools/embpet_cli.py   send one command, print the matching @@ response (`CMD [ARG ...] [-- TEXT ...]`: words after `--` may start with `-`); --listen SEC prints evt frames; --monitor SEC [--reset] dumps raw console output
tools/console_test.py device tests of the console: functional, soak (port closed), reopen (no reset on open)
tools/console_cpu.py  console CPU per command (test build, components/proto/probe.defaults)
tools/hil.py          run test/hil (pytest, project .venv) -- use `scripts/hw.sh python tools/hil.py -k smoke`
tools/tg_from_env.py  store TG_TOKEN from .env.local on the board in-process (never in argv, never printed)
tools/tg_test.py      device tests of the Bot API client (test build, components/tgbot/tgtest.defaults, bridge off): api, soak
tools/bridge_test.py  device tests of the bridge (test build, components/tgbot/bridgetest.defaults): auto (no user, nothing sent), human (README 3.5 with the owner replying in Telegram)
test/hil/             hardware-in-the-loop pytest suite; skips when no board is attached
scripts/dev-venv.sh   create the git-ignored .venv (ESP-IDF interpreter + pyserial + pytest)
scripts/bootstrap.sh  install a self-contained ESP-IDF into a given root
scripts/export.sh     activate that ESP-IDF in the current shell (source it)
skills/               agent skills (wifi-connect, …); symlinked into .claude/skills and .agents/skills (both git-ignored)
```

---

## 7. Non-goals (for now)

- **Host broker/daemon.** It will exist later. The protocol (actor IDs, request IDs, `seq`, `boot`, event replay) is designed for it.
- GPIO control or status from Telegram.
- More than one Telegram chat, group chats, media/files, inline keyboards.
- A pending table that persists across reboots (routing still survives via the `[A]` prefix).
- OTA updates, webhooks, NVS encryption, deep sleep and battery operation.

---

## 8. Review decisions

Resolved in review 1:
- LED patterns
- input events with debounce
- 100 ms pulse default
- both owner-binding methods
- 64-entry pending table
- `[A]` format plus online notice
- rejecting text over 4096 bytes

Resolved in review 2:
- `extra` edge count instead of a running counter
- answered state is per message; the host withdraws a message with `tg ack -m`
- all outputs use sink wiring (off during boot, GPIO2 kept)

Resolved in review 3:
- ROM boot log on UART0 left as is (no eFuse)
- USB-Serial/JTAG is the only log sink (bootloader and app)

No open questions.

