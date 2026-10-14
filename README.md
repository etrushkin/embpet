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
| 8 | Status LED | Onboard blue LED, **active-low** (verified on the unit: GPIO8 high = dark, low = lit). **Strapping pin**, must be high at reset; leave nothing else on it. |
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
- **Binding the owner:** `tg pair` prints a 6-digit code (valid 5 min) and the owner sends `/start <code>` to the bot. Alternatively set the ID directly with `tg owner <chat_id>`.
- **Outgoing:** `tg send -a A "text"` posts `[A] text` to the owner. The actor tag is rendered bold through Telegram `entities`, with no parse_mode, so message text needs no escaping. Plain text only, up to 4096 bytes UTF-8. Longer text is rejected (`err: too_long`).
- **Threading:** `tg send -a A -r <msg_id> "text"` sends the message as a Telegram reply to an earlier message, e.g. continuing a conversation with the owner.
- **Incoming (routing):**
  1. If the owner uses **Reply** on a bot message, the firmware looks up the replied-to `message_id` in the pending table and emits a `reply` event for that actor.
  2. If the message isn't in the table (after a reboot or eviction), the actor is recovered from the `[A]` prefix of the replied-to text, which Telegram includes in the update. The event then carries `"src":"prefix"`. Routing therefore survives reboots even though the table lives in RAM.
  3. If the owner writes **without Reply**, the bot answers: *"Use Reply on an actor's message so I know who it's for."* No event goes to the host.
- **Pending ("unanswered") table:** RAM only, 64 entries. An entry is created by each `tg send` and becomes *answered* on the first owner reply to it, or through `tg ack`. When the table is full, the oldest answered entry is evicted first. If every entry is still pending, the oldest pending entry is dropped and a `pending_dropped` event is emitted. Ages use uptime (monotonic), so no SNTP is needed.
- **Receiving:** long polling `getUpdates` (timeout 50 s, `allowed_updates=["message"]`) on its own task and HTTPS connection. Sends use a second keep-alive connection. On HTTP 429 the client honours `retry_after`.
- **Delivery semantics:** at-least-once. The getUpdates offset is confirmed only after the event has been emitted. Telegram keeps unconfirmed updates for 24 h, so replies sent while the board was off arrive after boot. A crash between emitting and confirming can produce a duplicate; hosts dedupe by the reply's `msg` id.
- **Online notice:** after each boot, once Telegram is first reachable, the bot posts `embpet online` with firmware version and reset reason. Not sent on later reconnects.
- **TLS:** server verification uses the IDF certificate bundle, with no pinned certificate. mbedTLS dynamic buffers are on to keep idle RAM low.

### 2.2 Status LED (GPIO8)

Priority from top to bottom; the first matching row wins.

| State | LED |
|---|---|
| Booting (app start until init done and Wi-Fi started) | solid on |
| Not configured, or Wi-Fi/Telegram down | blink 1 Hz (500 ms on / 500 ms off) |
| ≥1 unanswered | 1 s burst at 5 Hz (5 × 100 ms on / 100 ms off), then 3 s off; 4 s cycle |
| Online, nothing pending | off |

While offline the unanswered pattern is hidden; the `pending` count is still available from `tg pending`.

### 2.3 Inputs

GPIO 10, 20 and 21, wired through optos (§1.3). Active means the opto is energised and the pin is low.

- **Debounce:** an any-edge GPIO interrupt (re)starts a per-pin debounce timer. A change is accepted once the level has been stable for the debounce time. Default 20 ms, per pin 0–1000 ms, set with `io debounce` and stored in NVS.
- **Events:** each accepted change emits an `input` event (`pin`, `active`, `extra`).
- **`extra`** is the number of raw edges since the previous `input` event for that pin, minus the one edge that produced the change. A clean change gives `0`. Contact bounce gives an even number (2, 4, …). A glitch shorter than the debounce time, which produces no event, is counted into the next event's `extra`. A host detects lost events through gaps in the frame `seq`.
- `io in` returns `active`, raw `level`, and `extra` accumulated so far since the last event, for each input.

### 2.4 Outputs

- `io set <pin> on|off|toggle` sets a level. It cancels any running pulse on that pin.
- `io pulse <pin> [-d ms]` inverts the current level for `ms` (default **100**, range 1–600000), then restores it. Re-pulsing a pin that is already pulsing restarts the timer, and the original level is still the one restored.
- `io get` reports each pin's level, whether it is pulsing, and the remaining pulse time in ms.
- `io boot <pin> on|off` stores the logical level a pin takes at app start (NVS, default `off`). `io boot` with no arguments lists them.
- All output levels are **logical**: `on` = opto conducting, regardless of wiring polarity (§1.3).

### 2.5 Configuration storage (NVS)

Wi-Fi SSID and password, Wi-Fi TX power, bot token, owner `chat_id`, per-pin boot levels, and per-input debounce times. Secrets are never echoed back: `wifi status` shows the SSID only, and `tg status` shows a masked token (`123456:AA…xyz`). NVS encryption is a non-goal for now. The settings carry a schema version; if the stored version differs from the firmware's, including after a downgrade, the `embpet` NVS namespace is erased and all settings return to their defaults.

---

## 3. Serial protocol

### 3.1 Transport
- USB-Serial/JTAG CDC port; the baud rate is irrelevant. It uses the `esp_console` REPL (linenoise) with prompt `embpet> `, and the maximum command line is **4400 bytes**.
- **Port-open reset:** some tools reset the C3 when they open the port because of the DTR/RTS line states. Host programs should set DTR=RTS=false *before* opening (pyserial: `s = serial.Serial(); s.dtr = s.rts = False; s.port = ...; s.open()`). `tools/embpet_cli.py` does this.
- **Log sink: USB-Serial/JTAG is the primary and only console**, for both the 2nd-stage bootloader and the app (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`, `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`). UART0 is never initialised as a console, so GPIO20/21 are free for inputs. The 1st-stage ROM boot log still goes to UART0 (GPIO21) as well as USB. That's left as is: no eFuse changes.
- If no host is reading, console output is dropped after a short TX timeout instead of blocking the firmware.
- The console also carries ESP-IDF log lines, the echoed input and the prompt. **Hosts must ignore every line that does not start with `@@`.** The console log level defaults to `WARN` and can be changed with `sys log`.

### 3.2 Host → FW: plain commands

Every command is a normal console line. Two options are accepted on **every** command and echoed back in its response:

| Option | Meaning |
|---|---|
| `-a <actor>` | Actor ID, `[A-Za-z0-9_-]{1,16}`. **Required** for `tg send`; optional elsewhere. |
| `-i <rid>` | Request ID chosen by the host, `[A-Za-z0-9_-]{1,32}`, for matching responses. |

Text arguments are double-quoted. Inside quotes, `\"`, `\\` and `\n` are escapes, and UTF-8 passes through as-is.

Plain commands for everything, including `tg send`, mean the firmware needs only one input parser (argtable3), and every command can still be typed by hand in a terminal.

### 3.3 FW → host: JSON frames

Every machine-readable output is one line: `@@` + a compact JSON object. Each frame is written as `\n@@{…}\n` in a single `fwrite` under the stdout lock. The leading `\n` ends any prompt or partial line before it, so a frame always starts a line (regex `^@@(\{.*\})\r?$`), and no log line can split it.

Common fields: `t` (`res` | `evt`), `seq` (per-boot frame counter, starting at 1), `boot` (random 32-bit hex boot ID, so a host can detect resets).

**Responses** (`t:"res"`). Exactly one per command, emitted after the command finishes:
```
@@{"t":"res","seq":15,"boot":"9f3a11c2","cmd":"tg send","rid":"7","actor":"A","ok":true,"msg":1234}
@@{"t":"res","seq":16,"boot":"9f3a11c2","cmd":"io set","rid":"8","ok":false,"err":"bad_pin","msg":"pin 9 is not an output"}
```
`tg send` blocks until Telegram answers (timeout 10 s). It fails immediately with `err:"offline"` if Wi-Fi or Telegram is down.

**Events** (`t:"evt"`). Asynchronous:

| `ev` | Fields | When |
|---|---|---|
| `boot` | `fw`, `idf`, `reset_reason` | Once after start |
| `wifi` | `state` (`up`/`down`), `ip`, `rssi` | Wi-Fi changes |
| `tg` | `state` (`online`/`offline`/`unconfigured`/`auth_error`), `detail` | Telegram reachability changes |
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
| `tg token <token>` | Store the bot token and restart the poller | `bot` (username, from `getMe`) |
| `tg pair` | Start owner pairing | `code`, `expires_s` |
| `tg owner [<chat_id>\|--clear]` | Show, set or clear the owner chat | `chat_id` |
| `tg status` | Bridge status | `state`, `bot`, `owner`, `token` (masked), `pending` |
| `wifi set "<ssid>" ["<pass>"]` | Store credentials and reconnect | — (then `wifi` event) |
| `wifi status` | Connection info | `state`, `ssid`, `ip`, `rssi`, `txpower` |
| `wifi txpower <dBm>` | TX power, 2–20 dBm (default 8.5) | `txpower` |
| `io in` | Read inputs | `in:[{pin,active,level,extra}]` |
| `io debounce [<pin> <ms>]` | Show or set debounce | `debounce:{"10":20,...}` |
| `io set <pin> on\|off\|toggle` | Set an output | `pin`, `level` |
| `io pulse <pin> [-d ms]` | Pulse-toggle an output (default 100 ms) | `pin`, `level`, `remaining_ms` |
| `io get` | Output states | `out:[{pin,level,pulsing,remaining_ms}]` |
| `io boot [<pin> on\|off]` | Per-pin boot level | `boot:{"0":"off",...}` |
| `evt since <seq>` | Replay buffered events | events re-emitted, then `res` |
| `sys info` | Versions, uptime, heap, MAC | … |
| `sys log <level>` | Console log level | — |
| `sys reboot` | Restart | `res`, then reboot |

### 3.5 Example: two actors

```
> tg send -a A -i 1 "Build #42 failed on step 'test'. Retry?"
@@{"t":"res","seq":20,"boot":"9f3a11c2","cmd":"tg send","rid":"1","actor":"A","ok":true,"msg":501}
> tg send -a B -i 2 "Disk at 91% on nas01. Clean /tmp?"
@@{"t":"res","seq":21,"boot":"9f3a11c2","cmd":"tg send","rid":"2","actor":"B","ok":true,"msg":502}
                                     ... LED: 5 Hz bursts (2 pending)
   owner: Reply on [B] → "yes"
@@{"t":"evt","seq":22,"boot":"9f3a11c2","ev":"reply","actor":"B","to":502,"msg":503,"text":"yes","date":1760450100,"src":"table"}
   owner: Reply on [A] → "no, check flaky test first"
@@{"t":"evt","seq":24,"boot":"9f3a11c2","ev":"reply","actor":"A","to":501,"msg":504,"text":"no, check flaky test first","date":1760450130,"src":"table"}
                                     ... LED off
```

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
They default to a Debug build with AddressSanitizer and UBSan (`-fsanitize=address,undefined`, UB is fatal); LeakSanitizer is unavailable on macOS ARM.

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
tools/embpet_cli.py   send one command, print the matching @@ response; --monitor SEC [--reset] dumps raw console output
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

