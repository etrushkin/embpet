# embpet device and protocol specification

This document describes the embpet board as a lab device and specifies its serial protocol
completely enough to drive it from labgrid (or any other host program) without reading the
firmware. It covers the production firmware only; commands that exist only in test builds
(`… t_*`, `probe …`, `trace`) are out of scope.

- Design rationale, measurements and build instructions: `README.md`.
- Architecture and verification: `docs/PLAN.md`.
- Reference host implementation (Python): `tools/embpet/` (`Board`, `build_line`, `parse_frame`, `open_port`).

Normative words: **must**, **must not**, **should**, **may**.

---

## 1. Device overview

embpet is an ESP32-C3 board on USB that gives a host:

| Function | Count | Interface |
|---|---|---|
| Opto-isolated digital outputs | 8 (`OUT0`–`OUT7`) | set on/off/toggle, timed pulse, level applied at boot |
| Opto-isolated digital inputs | 3 (`IN0`–`IN2`) | debounced level, change events, edge counting |
| Telegram bridge | 1 bot, 1 human ("owner") | send messages as named actors, receive the owner's replies as events |
| Status LED | 1 (on board) | shows boot / offline / unanswered messages / idle |

All of it is reached through one USB CDC serial port that carries a line-based text command
interface and machine-readable JSON frames.

Typical lab uses:
- Outputs drive relays or switch inputs of a device under test (DUT): power, reset or boot
  buttons, jumpers.
- Inputs sense DUT signals: power-good, LEDs, GPIOs, or a human button.
- The Telegram bridge asks a human to do or confirm something during a test run (plug a
  cable, look at a display), and returns the answer to the test.

### 1.1 Hardware

| Item | Value |
|---|---|
| Module | ESP32-C3 "Super Mini" (QFN32, rev v0.4, 4 MB embedded flash, 40 MHz crystal) |
| USB | Native USB-Serial/JTAG controller, USB-C. No USB-UART bridge chip. |
| Power | 5 V from USB. The board draws its logic supply from USB, so unplugging USB powers it off. |
| Logic level | 3.3 V. GPIOs are **not** 5 V tolerant. |
| Radio | 2.4 GHz Wi-Fi (WPA2/WPA3-Personal), used only for Telegram |
| Buttons | BOOT (GPIO9), RESET (EN) |

### 1.2 Pin map

Outputs and inputs are addressed by **logical number** everywhere in the protocol. GPIO
numbers matter only for wiring.

| Logical | GPIO | Notes |
|---|---|---|
| `OUT0` | 4 | |
| `OUT1` | 3 | |
| `OUT2` | 2 | Strapping pin; sink wiring keeps it high at reset (required). |
| `OUT3` | 1 | |
| `OUT4` | 0 | |
| `OUT5` | 5 | |
| `OUT6` | 6 | |
| `OUT7` | 7 | |
| `IN0` | 10 | |
| `IN1` | 20 | Next to the antenna. |
| `IN2` | 21 | The ROM prints its boot log on this pin at reset (harmless; limited by the 1 kΩ series resistor). |
| LED | 8 | On-board, active-low. Not available to the host. |

### 1.3 Electrical interface (PC817C optocouplers)

**Outputs** (sink configuration, mandatory):
```
3V3 ── 390 Ω ── opto LED ── GPIO          GPIO low = opto LED on = output "on"
                opto transistor (C–E)     → isolated field side: switches up to the
                                             PC817C ratings (80 V, 50 mA)
```
- Logical `on` means the opto conducts. The firmware hides the inversion.
- LED current is about 5 mA (CTR 200–400 % at 5 mA, so the transistor can sink about 10 mA
  reliably, about 5 mA after ageing margin). Drive a relay or a heavier load through a
  transistor or relay driver on the field side.
- From power-up or reset until the firmware applies the boot levels, every output is off:
  the GPIO is high-impedance and no current can flow through the LED. A continuity check with
  RESET held is still open on the bench (see `bd show embpet-t5x`).

**Inputs** (opto transistor pulls the GPIO low):
```
3V3 ── 10 kΩ ──┬── 1 kΩ ── GPIO           field side: V_in ── R_F ── opto LED ── GND_field
               └── opto C–E ── GND
```
- `active: 1` means the opto LED is energised (the GPIO reads low).
- Choose R_F for I_F ≈ 5 mA (3–10 mA works): 24 V → 4.7 kΩ, 12 V → 2.2 kΩ, 5 V → 680 Ω.
- The PC817 switches in tens of µs; the default debounce (20 ms) dominates.

Verified on the bench (2026-10-21) with each `OUTn` wired to the field side of `INn`: each
output drives exactly one input, no crosstalk, and the loopback test suite passes
(`tools/hil.py -m loopback`).

### 1.4 Status LED

| State (first match wins) | LED |
|---|---|
| Booting (until Wi-Fi and the Telegram bridge are started) | solid on |
| Not configured, or Wi-Fi/Telegram down | blink 1 Hz (500 ms on, 500 ms off) |
| At least one unanswered message | 5 × (100 ms on, 100 ms off), then 3 s off; 4 s cycle |
| Online, nothing pending | off |

---

## 2. Device identification

| Property | Value |
|---|---|
| USB VID:PID | `303A:1001` (Espressif USB-Serial/JTAG) |
| USB product string | `USB JTAG/serial debug unit` |
| USB serial number | The chip's MAC in uppercase with colons, e.g. `E8:3D:C1:8B:8E:D4` (the bench unit used in this document's examples). Unique per board. |
| Port name | Linux `/dev/ttyACM*`; macOS `/dev/cu.usbmodem*` (the name depends on the USB socket) |
| MAC from the firmware | `sys info` → `mac` (lowercase, e.g. `e8:3d:c1:8b:8e:d4`) |

On Linux, udev exposes the serial number as `ID_SERIAL_SHORT`; match on it, not on the
`ttyACM` number, which depends on the order of enumeration. Check with
`udevadm info /dev/ttyACM0 | grep ID_SERIAL`. A stable name, if wanted:
```
# /etc/udev/rules.d/70-embpet.rules
SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", ATTRS{idProduct}=="1001", \
  ENV{ID_SERIAL_SHORT}=="E8:3D:C1:8B:8E:D4", SYMLINK+="embpet-bench1"
```
Every ESP32-C3/S3 with native USB uses the same VID:PID, so always match the serial number.

**The USB device disappears and comes back** on every chip reset (`sys reboot`, RESET button,
watchdog, flashing). The host must close the port and reopen it (see §3.3).

---

## 3. Transport

### 3.1 Link

- USB CDC-ACM. Baud rate, parity and stop bits are ignored; any setting works (the tools use 115200 8N1).
- Encoding: UTF-8 in both directions.
- The firmware console accepts input only from this port. The ESP-IDF log also goes to this
  port (see §4.3). UART0 is not used.

### 3.2 Opening the port without resetting the chip

The USB-Serial/JTAG controller maps the DTR and RTS modem lines to the chip's reset and boot
pins (the same mechanism esptool uses):

| DTR | RTS | Effect |
|---|---|---|
| 0 | 1 | chip held in reset while it stays so; going on to (0,0) releases it: **chip reset** |
| 1 | 1 → 0 | no reset (this is the safe open sequence below) |
| 1 | 0 | selects download mode for the *next* reset; harmless on its own |
| 0 | 0 | normal operation |

Opening a tty asserts both lines (observed on macOS; standard tty behaviour on Linux, not
measured separately here). A plain pyserial `open()` then clears
DTR first and RTS second, so the chip sees (DTR,RTS) = (1,1) → (0,1) → (0,0): **it resets on
every open** (measured on macOS, 5/5 runs). The default pyserial open with DTR/RTS left
asserted keeps the chip **silent while the port is open** (macOS, 5/5 runs).

A host **must** open the port so that the lines go (1,1) → (1,0) → (0,0), i.e. clear RTS
first, then DTR. With pyserial (verified: macOS, about 15 opens with continuous uptime; a
Raspberry Pi with Debian 13 aarch64, pyserial 3.5, `/dev/ttyACM0`, same boot id across
separate runs, 2026-10-16):
```python
s = serial.Serial()
s.port = "/dev/ttyACM0"
s.baudrate = 115200
s.timeout = 0.2
s.dtr = False
s.rts = False
s.dsrdtr = True      # open() then skips its DTR update and clears only RTS
s.open()
s.dtr = False        # now clear DTR: RTS first, then DTR
s.dsrdtr = False
```
To reset the chip on purpose: RTS = 1 for 200 ms, then RTS = 0 (with DTR = 0), and reopen
the port after the USB device comes back.

Notes:
- labgrid's stock `SerialDriver` opens the port the plain pyserial way, so it resets or holds
  the chip. Use a driver that opens the port as above (§9).
- Remote access over ser2net/RFC2217 (labgrid `NetworkSerialPort`) has **not** been
  verified: ser2net opens the tty itself, and the RFC2217 client sets the lines again. If you
  use it, check that `sys info` reports the same `boot` id and a rising `uptime_s` across
  reconnects. Running the embpet driver on the exporter host avoids the problem.
- Closing the port does not reset the chip.

### 3.3 Port lifecycle

- **First data after a chip reset:** on macOS the first open after a reset delivered its
  first byte about 2.5 s late; later opens deliver within 0.1 s. Hosts should allow up to
  about 5 s for the first response after a reset.
- **After a reset** the port disappears (USB re-enumeration). Close it, wait about 3 s, then
  retry opening for up to about 30 s; then poll `sys info` until it answers (up to about 15 s).
  Measured on the bench (2026-10-21, development AP, WPA3): after an RTS reset the first
  `sys info` answered with `uptime_s` 0; Wi-Fi was up about 4–6 s after the reset and
  Telegram online about 1.5 s after Wi-Fi.
- **Port closed:** output written while no program has the port open is dropped (no backlog
  is kept apart from the event ring, §6). Events can be recovered with `evt since` (§7.5).
- **Session start:** after opening, the host **should** send the bytes `0x15 0x0A`
  (Ctrl-U, LF). Ctrl-U discards a partial line a previous session may have left in the
  firmware's line buffer; the LF ends the now empty line, which gets no response.

---

## 4. Console line protocol (host → device)

### 4.1 Lines

- A line ends with CR, LF or CRLF. A blank line (only spaces/tabs) gets no response.
- Maximum line length: **4400 bytes** without the terminator. A longer line is answered with
  `err: "too_long"`; only the options before the cut are seen (§4.3).
- The firmware echoes the input it stores (plus `\b \b` for an erase and a newline at the end
  of the line) and prints the prompt `embpet> `. Hosts ignore both (they are not frames).
- Line editing bytes: BS (0x08) and DEL (0x7F) erase one UTF-8 character; Ctrl-U (0x15) discards the
  whole line (echoed as `^U` plus a new prompt). Every other control byte except TAB is
  ignored. Hosts **must not** send control characters inside arguments (newlines inside text
  go as the escape `\n`, §4.2).

### 4.2 Tokens and quoting

A line is split into tokens on spaces and tabs.

- **Unquoted token:** any run of non-blank bytes. Backslashes and quotes are ordinary there.
- **Quoted token:** `"…"`. Inside, the escapes `\"`, `\\` and `\n` (newline) are recognised;
  a tab is ordinary text; UTF-8 passes through unchanged. Any other escape, a missing closing
  quote, a backslash at the end of the line, or text glued to the closing quote
  (`"abc"def`) is rejected with `err: "bad_args"`.
- A token starting with `-` is taken as an option. A text argument that starts with `-`
  **must** come after a bare `--` token; everything after `--` is positional.
- At most **31 tokens** per line, command words included; more gives `bad_args`
  ("too many arguments").
- Integers: use decimal. (Pin and millisecond arguments also accept argtable's `0x` and
  `0b` prefixes; message ids, chat ids, `--older` and `evt since` accept decimal only.)

Host recommendation: quote every free-text argument; put free text after `--`.

### 4.3 Command line structure

```
<group> <command> [-i <rid>] [-a <actor>] [options] [args] [-- text ...]
help [-i <rid>]
```
- Groups: `tg`, `wifi`, `io`, `evt`, `sys`. The only single-word command is `help`.
- Two options are accepted on **every** command and echoed in its response:

  | Option | Format | Meaning |
  |---|---|---|
  | `-i <rid>` | `[A-Za-z0-9_-]{1,32}` | Request id chosen by the host; echoed as `rid` |
  | `-a <actor>` | `[A-Za-z0-9_-]{1,16}` | Actor id; **required** for `tg send`, optional elsewhere; echoed as `actor` |

- An invalid `-i` or `-a` value gives `bad_args`; the response then has no `rid` (or no
  `actor`).
- `-i` and `-a` are echoed even when the command fails (unknown command, bad option, bad
  escape), provided their values are valid and they appear before the faulty spot. Hosts
  **should** put them right after the command words.

### 4.4 Execution, ordering and flow control

- Commands run **one at a time, in the order received**. Exactly one `res` frame is emitted
  per non-blank line, including lines that fail to parse.
- Commands may be pipelined. Input is flow-controlled and never dropped: while a command runs
  (for example `tg send`, up to 10 s), input waits in a 4656-byte receive buffer; when that is
  nearly full the device stops accepting USB packets and the host's write blocks until the
  console reads again. A host that writes with a timeout must allow for the longest command
  queued ahead.
- A pipelining host **must** keep reading while it writes (reader thread or non-blocking I/O):
  output the host does not read is dropped after one 50 ms stall, and `res` frames are not
  replayable.
- Match responses to requests by `rid`, never by position: echo, log lines and event frames
  are interleaved with responses.

---

## 5. Frames (device → host)

### 5.1 Format

Every machine-readable output is one line:
```
@@<compact JSON object>
```
- A frame always starts at the beginning of a line; the device writes `\n@@{…}\n` atomically,
  so no log line or echo can split it. Recognise frames with the regex `^@@(\{.*\})\r?$`
  (the reference parser accepts a line only if it starts with `@@{`).
- Lines that are not frames (echo, prompt, ESP-IDF log lines, the ROM boot log, `help` text)
  **must** be ignored.
- Field order is not specified. Hosts **must** ignore unknown fields (new fields may be added).
- A frame that does not parse must be dropped. It can be cut by a panic or watchdog report
  written from interrupt context (the host then sees a missing `res` and, after a panic, a
  new `boot` id), or by output dropped while the host was not reading (§4.4).

### 5.2 Common fields

| Field | Type | Meaning |
|---|---|---|
| `t` | `"res"` \| `"evt"` | Frame type |
| `seq` | integer ≥ 1 | Per-boot frame counter shared by `res` and `evt`, increasing by 1 per frame |
| `boot` | string, 8 hex digits | Random id of this boot. A change means the device restarted. |

`(boot, seq)` identifies a frame uniquely. Replayed events (§7.5) keep their original
`boot` and `seq`; hosts dedupe on that pair.

### 5.3 Responses (`t: "res"`)

| Field | Present | Meaning |
|---|---|---|
| `cmd` | normally | Command name, e.g. `"io set"`. For an unknown command, the typed words if they are plain ASCII words; otherwise absent. |
| `rid` | if `-i` was given and valid | Echo of `-i` |
| `actor` | if `-a` was given and valid | Echo of `-a` |
| `ok` | always | `true` on success |
| `err` | if `ok` is false | Error code (§5.4) |
| `msg` | if `ok` is false | Human-readable detail (may be absent). **Note:** for `tg send` success, `msg` is the Telegram message id (integer), not an error text. |
| payload | if `ok` is true | Command-specific fields (§7) |

Examples:
```
@@{"t":"res","seq":15,"boot":"9f3a11c2","cmd":"io set","rid":"7","ok":true,"pin":3,"level":"on"}
@@{"t":"res","seq":16,"boot":"9f3a11c2","cmd":"io set","rid":"8","ok":false,"err":"bad_pin","msg":"pin 9 is not an output"}
```

### 5.4 Error codes

| `err` | Meaning |
|---|---|
| `bad_args` | Unknown command; unknown, missing or malformed option or argument; invalid `-a`/`-i`; bad quoting or escape; value out of range |
| `bad_pin` | Pin number not valid for this operation (not an output 0–7 / not an input 0–2) |
| `too_long` | Line over 4400 bytes, or text over its limit (`tg send`) |
| `offline` | Wi-Fi or Telegram is down |
| `not_configured` | A required setting is missing (Wi-Fi credentials, bot token, owner chat) |
| `auth_error` | Telegram rejected the bot token |
| `timeout` | No answer in time (Telegram within 10 s) |
| `busy` | Cannot run now (Telegram rate limit, send queue full) |
| `internal` | Firmware error (out of memory, storage failure, timer failure, a handler that sent no response) |

A negative pin number looks like an option and gives `bad_args`, not `bad_pin`.

---

## 6. Events (`t: "evt"`)

Events are asynchronous frames with an `ev` field. They are emitted whether or not a host has
the port open, and the last **32** (at most 16 KB of frames) are kept in RAM for replay
(`evt since`, §7.5). The ring is cleared by a reset.

| `ev` | Fields | Emitted when |
|---|---|---|
| `boot` | `fw` (string), `idf` (string), `reset_reason` (string, §7.6) | Once, at start |
| `wifi` | `state` (`"up"`/`"down"`); `ip` (string) and `rssi` (dBm, integer) only when up | Wi-Fi goes up or down, or the IP changes (not on every reconnect attempt) |
| `tg` | `state` (`"online"`/`"offline"`/`"unconfigured"`/`"auth_error"`), `detail` (string) | Telegram reachability or the bot behind the token changes. `detail` is for humans, e.g. `"@mybot"`, `"wifi down"`, `"token changed"`, `"timeout"`, `"no token"`. |
| `input` | `pin` (0–2), `active` (0/1), `extra` (integer ≥ 0) | A debounced input change (§8.2) |
| `pending` | `count` (integer) | The number of unanswered messages changed |
| `reply` | `actor`, `to` (message id replied to), `msg` (the owner's message id), `text`, `date` (unix seconds), `src` (`"table"`/`"prefix"`) | The owner replied to an actor's message (§8.3) |
| `paired` | `chat_id` (integer) | The owner chat was bound with `/start <code>` |
| `pending_dropped` | `actor`, `msg` | The pending table overflowed and dropped its oldest unanswered entry |

Ordering guarantees:
- Frames are written in `seq` order.
- For a `tg send`, the `pending` event comes **before** its `res`.
- A `reply` event comes before the `pending` event of the message it answers.
- `input` events of one pin keep their order; across pins they keep their order unless the
  32-entry decision queue overflowed (§8.2).

---

## 7. Command reference

Payload fields are listed for `ok: true`. All commands also accept `-i` and `-a` (§4.3).
"Stored" means written to flash (NVS) and kept across resets and reflashing (§10).

### 7.1 Outputs

**`io set <pin> on|off|toggle`**
- `pin`: 0–7. Sets the logical level; cancels a running pulse on that pin. Not stored.
- Payload: `pin`, `level` (`"on"`/`"off"`, the resulting level).
- Errors: `bad_pin`, `bad_args` (level not `on`/`off`/`toggle`).

**`io pulse <pin> [-d <ms>]`**
- Inverts the pin's current level for `ms` (default 100, range 1–600000), then restores it.
  Re-pulsing a pulsing pin restarts the timer; the level from before the first pulse is
  restored.
- Payload: `pin`, `level` (the level during the pulse), `remaining_ms`.
- Errors: `bad_pin`, `bad_args` (`-d` out of range), `internal` (timer).
- Timing: the pulse end is driven from the timer interrupt; measured widths are within a few
  µs of the request (1, 20, 100, 250, 1000 ms), also while Wi-Fi associates.

**`io get`**
- Payload: `out`: array of 8 objects `{pin, level, pulsing (bool), remaining_ms}`, pins 0–7
  in order. `remaining_ms` is 0 when not pulsing.

**`io boot [<pin> on|off]`**
- Without arguments: show. With both: store the level the pin takes at application start.
  Default `off` for all pins.
- Payload: `levels`: object `{"0":"off", …, "7":"off"}`.
- Errors: `bad_pin`, `bad_args` (only one argument, or level not `on`/`off`), `internal`.
- The boot level is applied early in start-up, after the outputs have been initialised off.
  Between reset and that moment every output is off (§1.3).

### 7.2 Inputs

**`io in`**
- Payload: `in`: array of 3 objects `{pin (0–2), active (0/1), level (raw GPIO level 0/1),
  extra}`. `active` is the last debounced state (its event may still be waiting to be sent);
  `level` is the raw pin now (`0` = opto energised); `extra` is the number of raw edges since
  the last `input` event of that pin.

**`io debounce [<pin> <ms>]`**
- Without arguments: show. With both: set and store the debounce time of input `pin` (0–2)
  to `ms` (0–1000). Default 20 ms.
- Payload: `debounce`: object `{"0":20, "1":20, "2":20}`.
- Errors: `bad_pin`, `bad_args` (pin without ms, ms out of range), `internal`.

### 7.3 Telegram bridge

**`tg send -a <actor> [-r <msg>] [--] "<text>"`**
- Sends `[actor] text` to the owner chat; the `[actor]` tag is shown bold. Blocks until
  Telegram answers (at most 10 s). On success the message is added to the pending table.
- `-r <msg>`: send as a Telegram reply to an earlier message id (1 … 2^53−1).
- Text: plain UTF-8 (no markup), at most 4096 bytes, and the whole `[actor] text` at most
  4096 UTF-16 code units (an emoji outside the BMP counts 2; the tag and its space count
  3 + actor length).
- Payload: `msg` (integer, Telegram message id).
- Fails without contacting Telegram, in this order: `bad_args` (invalid UTF-8) /
  `too_long` (over the limits),
  `not_configured` (no Wi-Fi credentials, token or owner), `offline`, `auth_error`.
  Telegram's answers map to: network error or 5xx → `offline`; no answer within 10 s →
  `timeout`; HTTP 429 → `busy` (and later sends get `busy` until Telegram's `retry_after`
  has passed); 403 (bot blocked) or 400 "chat not found" → `not_configured`; other 400 →
  `bad_args`.
- After `timeout` the message may still have been delivered; it is then added to the pending
  table (a `pending` event shows it) and replies route normally.

**`tg pending`** (optional `-a <actor>` filters by actor)
- Payload: `items`: array of `{msg, actor, age_s}`, oldest first.

**`tg ack --older <s>`** or **`tg ack -m <msg>`**
- Marks messages answered: all older than `s` seconds (`0` = all), or one message id. Exactly
  one of the two must be given.
- Payload: `acked` (number of entries changed).

**`tg status`**
- Payload: `state` (as in the `tg` event), `detail`, `bot` (username, `""` until known),
  `owner` (chat id integer, or `null`), `token` (masked, e.g. `"123456:AA…xyz"`, `""` if
  unset), `pending` (count).

**`tg token [--] <token>`**
- Checks the token with Telegram (`getMe`), and only if accepted stores it and restarts the
  poller. Wi-Fi must be up.
- Payload: `bot` (username), `token` (masked).
- Errors: `bad_args` (malformed), `offline`, `auth_error` (rejected; the old token stays),
  `timeout`, `busy`, `internal`.
- The token is a secret: never put it in a process argument list or log on the host
  (`tools/tg_from_env.py` sends it in-process).

**`tg pair`**
- Starts owner pairing: returns a random 6-digit code valid for 300 s. The person sends
  `/start <code>` to the bot from the chat that should become the owner; the device then
  stores that chat, emits `paired`, and confirms in the chat. A new `tg pair`, setting the
  owner or a new token voids the code; 5 wrong codes cancel pairing.
- Payload: `code` (string), `expires_s` (integer).
- Errors: `not_configured` (no token).

**`tg owner`**, **`tg owner [--] <chat_id>`**, **`tg owner --clear`**
- Show, set (non-zero integer; a negative id must come after `--`) or clear the owner chat.
  Stored.
- Payload: `chat_id` (integer or `null`).
- Errors: `bad_args`, `internal`.

### 7.4 Wi-Fi

**`wifi set [--] "<ssid>" ["<pass>"]`**
- Stores the credentials and reconnects at once; the response does not wait for the
  connection (watch for the `wifi` event). Omit the password for an open network.
- SSID 1–32 bytes; password empty, 8–63 bytes, or 64 hex digits.
- Payload: none.
- Errors: `bad_args`, `internal`.

**`wifi status`**
- Payload: `state` (`"up"`/`"down"`/`"unconfigured"`), `ssid` (`""` if unset), `txpower`
  (dBm, number); `ip` and `rssi` only when up. The password is never shown.

**`wifi txpower <dBm>`**
- 2–20 dBm in 0.25 steps (default 8.5). Stored and applied at once.
- Payload: `txpower`.
- Errors: `bad_args` (out of range or not a 0.25 step), `internal`.

Reconnection: exponential backoff 1, 2, 4, 8, 16 s, then every 30 s; reset by a successful
connection or by `wifi set`.

### 7.5 Events replay

**`evt since <seq>`**
- Re-emits the buffered events of the **current boot** with `seq` > `<seq>` (`0` = all),
  verbatim with their original `seq` and `boot`, then the response.
- Payload: `count` (events re-emitted), `truncated` (`true` if some events newer than
  `<seq>` had already left the 32-entry ring).
- Errors: `bad_args` (not 0 … 4294967295).

Reconnect procedure: remember the last `(boot, seq)` seen. After reopening, send
`evt since <last seq>` if `sys info` reports the same `boot`, otherwise `evt since 0`; dedupe
on `(boot, seq)`; if `truncated`, resynchronise state with `io in`, `io get`, `tg status`,
`tg pending`, `wifi status`.

### 7.6 System

**`sys info`**
- Payload: `fw` (firmware version, `git describe` output), `idf` (ESP-IDF version),
  `uptime_s`, `heap_free`, `heap_min` (bytes), `mac` (lowercase), `reset_reason`, `log`
  (current log level), `log_dropped` (log lines dropped because the host was not reading).
- `reset_reason` values: `poweron`, `external`, `software`, `panic`, `int_wdt`, `task_wdt`,
  `wdt`, `deepsleep`, `brownout`, `sdio`, `usb`, `jtag`, `efuse`, `pwr_glitch`,
  `cpu_lockup`, `unknown`. Observed on the bench: the RESET button gives `poweron`, an RTS
  reset over USB gives `usb`, `sys reboot` gives `software`.

**`sys log none|error|warn|info|debug`**
- Sets the console log level. Not stored; `warn` after each boot.
- Payload: `level`.

**`sys reboot`**
- Responds, then restarts about 100 ms later. The USB port disappears (§3.3).
- Payload: none.

**`help`**
- Prints every command's syntax as plain text (for humans), then the response.
- Payload: `cmds` (array of command names). Hosts may use it for feature detection.

---

## 8. Behaviour details

### 8.1 Outputs

- Levels are logical (`on` = opto conducting = GPIO low).
- Runtime levels are not stored. On every reset all outputs go **off** until the stored boot
  levels are applied a moment later (well under a second after reset; the exact time was
  not measured). A DUT powered through an output therefore loses power briefly when the
  embpet restarts, even with `io boot <pin> on`.
- `io set` and `io pulse` take effect before their response is written.

### 8.2 Inputs and the `input` event

- Debounce: an any-edge interrupt (re)starts a per-pin timer; a change is accepted once the
  level has been stable for the debounce time (default 20 ms, 0–1000 ms per pin).
- Debounce 0: the level is sampled about 1 ms after the first edge; at most 1000 events per
  second per input.
- `extra` = raw edges since the previous `input` event of that pin, minus the one edge that
  produced the change: 0 for a clean change, an even number for bounce; a glitch shorter than
  the debounce time produces no event and is counted into the next event's `extra`.
- No event is emitted at boot for an input that is already active; read `io in`.
- Latency: debounce time + a few ms; up to about 0.33 s more while Wi-Fi associates with a
  WPA3 network (the decision itself stays on time, only the event is late).
- Up to 32 decisions can wait to be sent; beyond that a pin's next decision waits too, and a
  change undone meanwhile gives no event (its edges count into the next `extra`).

### 8.3 Telegram bridge

- **One owner chat.** Only messages from the owner chat are processed.
- **Pending table** (RAM, 64 entries): each successful `tg send` adds an entry; the first owner
  reply to it, or `tg ack`, marks it answered. When full, the oldest answered entry is evicted
  first; if all are unanswered, the oldest is dropped with a `pending_dropped` event. Cleared
  by a reset.
- **Routing a reply:** if the owner uses Telegram's *Reply* on a bot message, the replied-to
  message id is looked up in the pending table (`src: "table"`). If it is not there (after a
  reset, or evicted), the actor is taken from the `[actor]` prefix of the replied-to text
  (`src: "prefix"`); this only counts when the replied-to message was written by this bot
  (its sender id equals the digits of the token before `:`) and the prefix parses.
- Not delivered to the host, answered with a hint instead (at most one hint per 10 s): a
  message without *Reply*, and a *Reply* to a message that carries no actor or cannot be
  routed.
- Answered entries stay in the pending table until evicted, so a second *Reply* to the same
  message emits another `reply` (`src: "table"`) with no `pending` change.
- **Delivery is at-least-once.** After a device reset between emitting a `reply` and
  confirming it to Telegram, the same reply is delivered again after boot, then with
  `src: "prefix"`. Hosts **must** dedupe `reply` events on `msg` alone.
- Replies sent while the device was off arrive after boot (Telegram keeps them 24 h).
- **Online notice:** after each boot, when Telegram is first reachable and an owner is set,
  the bot posts `embpet online · fw <version> · reset <reason>` to the owner.
- Requires outbound TCP 443 and DNS to `api.telegram.org`.

### 8.4 Timing summary

| Operation | Typical | Worst observed / limit |
|---|---|---|
| Simple command round trip (`sys info`, `io set`) | a few ms | about 0.2 s during TLS handshakes after boot; about 0.3 s per stall while Wi-Fi associates (WPA3) |
| `tg send` | 0.2–0.4 s | 10 s (then `timeout`) |
| `tg token` | about 1 s | network-bound |
| `sys reboot` → commands answered again | about 3 s (the reference host waits 3 s, then reopens) | allow 30 s |
| Wi-Fi up after reset | about 4–6 s (WPA3 AP) | backoff up to 30 s when the AP is away |
| Pulse width | exact to a few µs | — |

---

## 9. labgrid integration

labgrid has no built-in embpet driver. This section specifies one; the code below is a
design sketch, not tested against labgrid. Check the labgrid names used here against your
installed version (in particular `DigitalOutputResetDriver` and its attributes, and whether
a subclass of `Driver` may add a non-keyword attribute after the base class's `name`).

### 9.1 Resource

Use labgrid's `USBSerialPort` (Linux, udev) and match the USB serial number:
```yaml
targets:
  main:
    resources:
      USBSerialPort:
        match:
          ID_SERIAL_SHORT: "E8:3D:C1:8B:8E:D4"
    drivers:
      - EmbpetDriver: {}
      - EmbpetOutputDriver: {name: dut-power, index: 0}
      - EmbpetOutputDriver: {name: dut-reset-btn, index: 1}
      - DigitalOutputPowerDriver:
          name: dut-power-ctl
          bindings: {output: dut-power}
          delay: 2.0
      - DigitalOutputResetDriver:
          bindings: {output: dut-reset-btn}
imports:
  - embpet_labgrid.py
```
Do **not** bind labgrid's `SerialDriver` to this port (§3.2). With several
`EmbpetOutputDriver`s on one target, every lookup by protocol must name the driver
(`target.get_driver("DigitalOutputProtocol", name="dut-power")`), since more than one
driver matches. The sketch uses the resource's `.port` as a local device path, so it works
with `USBSerialPort`/`SerialPort`, not with `NetworkSerialPort`.

### 9.2 Drivers

| Driver | Binds to | Implements | Maps to |
|---|---|---|---|
| `EmbpetDriver` | `USBSerialPort` / `SerialPort` | own API: `cmd()`, `events()`, `info()` | owns the port, opens it reset-free, sends Ctrl-U LF, matches `res` by `rid`, buffers events, dedupes on `(boot, seq)` |
| `EmbpetOutputDriver` (attr `index` 0–7) | `EmbpetDriver` | `DigitalOutputProtocol` (`get()`, `set(status)`) | `io get` → `out[index].level == "on"`; `io set <index> on/off` |
| `EmbpetInputDriver` (attr `index` 0–2) | `EmbpetDriver` | own API: `get()`, `wait_for(active, timeout)` | `io in` → `in[index].active`; `input` events |
| `EmbpetTelegramDriver` (attr `actor`) | `EmbpetDriver` | own API: `ask(text, timeout)`, `notify(text)` | `tg send -a <actor>`; wait for `reply` with `actor` and `to` == sent `msg` |

With `EmbpetOutputDriver` exposing `DigitalOutputProtocol`, labgrid's own
`DigitalOutputPowerDriver` (power on/off/cycle), `DigitalOutputResetDriver` (reset line) and
button drivers can be stacked on top. For exact short pulses (a button press of a precise
length), add `pulse(ms)` → `io pulse <index> -d <ms>`; the device times it, not the host.

### 9.3 Sketch

```python
# embpet_labgrid.py -- design sketch (untested)
import json, re, time, random, string, collections
import attr, serial
from labgrid.factory import target_factory
from labgrid.driver.common import Driver
from labgrid.protocol import DigitalOutputProtocol
from labgrid.step import step

FRAME = re.compile(r"^@@(\{.*\})\r?$")

def _quote(s):
    if any((c < " " and c not in "\n\t") or c == "\x7f" for c in s):
        raise ValueError("control character in argument")
    return '"%s"' % s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")

@target_factory.reg_driver
@attr.s(eq=False)
class EmbpetDriver(Driver):
    bindings = {"port": {"USBSerialPort", "SerialPort"}}

    def on_activate(self):
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = self.port.port, 115200, 0.2
        s.dtr = s.rts = False
        s.dsrdtr = True
        s.open()
        s.dtr, s.dsrdtr = False, False          # RTS first, then DTR: no reset (SPEC 3.2)
        s.write(b"\x15\n")                      # session start (SPEC 3.3)
        self._s, self._buf = s, b""
        self._events = collections.deque(maxlen=1000)
        self._seen = collections.OrderedDict()  # (boot, seq) of recent events, bounded

    def on_deactivate(self):
        self._s.close()

    def _frames(self, deadline):
        while time.monotonic() < deadline:
            self._buf += self._s.read(self._s.in_waiting or 1)
            while b"\n" in self._buf:
                line, self._buf = self._buf.split(b"\n", 1)
                m = FRAME.match(line.decode("utf-8", "replace"))
                if m:
                    try:
                        yield json.loads(m.group(1))
                    except ValueError:
                        pass

    def _event(self, f):
        key = (f.get("boot"), f.get("seq"))
        if key not in self._seen:
            self._seen[key] = None
            if len(self._seen) > 4096:
                self._seen.popitem(last=False)
            self._events.append(f)

    @Driver.check_active
    @step(args=["words"])
    def cmd(self, words, args=(), text=(), actor=None, timeout=15.0):
        rid = "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(8))
        parts = words.split() + ["-i", rid] + (["-a", actor] if actor else [])
        parts += [_quote(a) for a in args]
        if text:
            parts += ["--"] + [_quote(t) for t in text]
        line = " ".join(parts).encode("utf-8")
        if len(line) > 4400:                    # SPEC 4.1
            raise ValueError("command line is %d bytes, at most 4400" % len(line))
        self._s.write(line + b"\n")
        for f in self._frames(time.monotonic() + timeout):
            if f.get("t") == "evt":
                self._event(f)
            elif f.get("t") == "res" and f.get("rid") == rid:
                if not f.get("ok"):
                    raise RuntimeError("%s: %s %s" % (words, f.get("err"), f.get("msg", "")))
                return f
        raise TimeoutError("no response to %r" % words)

    @Driver.check_active
    def events(self, seconds):
        while self._events:
            yield self._events.popleft()
        for f in self._frames(time.monotonic() + seconds):
            if f.get("t") == "evt":
                self._event(f)
                while self._events:
                    yield self._events.popleft()

@target_factory.reg_driver
@attr.s(eq=False)
class EmbpetOutputDriver(Driver, DigitalOutputProtocol):
    bindings = {"board": "EmbpetDriver"}
    index = attr.ib(validator=attr.validators.instance_of(int))

    @Driver.check_active
    def set(self, status):
        self.board.cmd("io set", args=[str(self.index), "on" if status else "off"])

    @Driver.check_active
    def get(self):
        return self.board.cmd("io get")["out"][self.index]["level"] == "on"

    @Driver.check_active
    def pulse(self, ms):
        self.board.cmd("io pulse", args=[str(self.index), "-d", str(ms)])
```
`args` are quoted but still parsed as options by the device, so `"-d"` works there; free text
goes in `text` (after `--`).

### 9.4 Operational rules for a lab setup

- One process owns the port at a time (`EmbpetDriver`); other code goes through it.
- Set boot levels for outputs that must hold across an embpet restart
  (`io boot <pin> on`), and remember the brief off-time during the restart (§8.1).
- Check `boot` in every frame; a change means the embpet restarted (outputs were reset to
  their boot levels, pending table and event ring cleared).
- Provision once per board (stored): `wifi set`, `tg token`, `tg pair` (or `tg owner`),
  optional `io boot`, `io debounce`, `wifi txpower`.
- Wi-Fi/Telegram are optional for GPIO use: outputs and inputs work without them (the LED
  then blinks 1 Hz).

---

## 10. Stored configuration

| Setting | Set with | Default |
|---|---|---|
| Wi-Fi SSID and password | `wifi set` | none (unconfigured) |
| Wi-Fi TX power | `wifi txpower` | 8.5 dBm |
| Bot token | `tg token` | none |
| Owner chat id | `tg pair`, `tg owner` | none |
| Output boot levels | `io boot` | all off |
| Input debounce times | `io debounce` | 20 ms each |

- Stored in the NVS flash partition; kept across resets, power loss and firmware updates
  (flashing the application does not erase NVS; `esptool erase_flash` does).
- The store carries a schema version. If it differs from the firmware's (including after a
  downgrade), all settings are erased and return to their defaults.
- Debounce times are stored per physical GPIO, so they follow the wire, not the logical
  number, if a future firmware remaps inputs.
- Not stored: runtime output levels, log level, pending table, event ring.

---

## 11. Security

- The console has **no authentication**: anyone with access to the USB port can drive the
  outputs, read inputs, send Telegram messages as any actor and change the owner chat.
  Protect the port like the DUT's own console.
- Secrets are never echoed: `wifi status` shows the SSID only, `tg status` and `tg token`
  show a masked token. HTTP client log tags are capped at `warn` even under `sys log debug`,
  because the request line carries the token.
- NVS is not encrypted; physical access to the flash exposes the stored credentials.

---

## 12. Example session

```
→ \x15\n
→ sys info -i a1
← @@{"t":"res","seq":7,"boot":"3bf0304f","cmd":"sys info","rid":"a1","ok":true,"fw":"148a3d9","idf":"v5.5.2","uptime_s":42,"heap_free":128784,"heap_min":113140,"mac":"e8:3d:c1:8b:8e:d4","reset_reason":"poweron","log":"warn","log_dropped":0}
→ evt since 0 -i a2
← @@{"t":"evt","seq":1,"boot":"3bf0304f","ev":"boot","fw":"148a3d9","idf":"v5.5.2","reset_reason":"poweron"}
← @@{"t":"evt","seq":2,"boot":"3bf0304f","ev":"tg","state":"offline","detail":"starting"}
← @@{"t":"evt","seq":3,"boot":"3bf0304f","ev":"wifi","state":"up","ip":"192.168.88.44","rssi":-67}
← @@{"t":"evt","seq":4,"boot":"3bf0304f","ev":"tg","state":"online","detail":"@mybot"}
← @@{"t":"res","seq":8,"boot":"3bf0304f","cmd":"evt since","rid":"a2","ok":true,"count":4,"truncated":false}
→ io set -i a3 0 on
← @@{"t":"res","seq":9,"boot":"3bf0304f","cmd":"io set","rid":"a3","ok":true,"pin":0,"level":"on"}
← @@{"t":"evt","seq":10,"boot":"3bf0304f","ev":"input","pin":0,"active":1,"extra":0}
→ tg send -i a4 -a flash -- "Board 3 needs a USB cable. Reply when done."
← @@{"t":"evt","seq":11,"boot":"3bf0304f","ev":"pending","count":1}
← @@{"t":"res","seq":12,"boot":"3bf0304f","cmd":"tg send","rid":"a4","actor":"flash","ok":true,"msg":501}
   (owner uses Reply on the message: "done")
← @@{"t":"evt","seq":13,"boot":"3bf0304f","ev":"reply","actor":"flash","to":501,"msg":502,"text":"done","date":1760450100,"src":"table"}
← @@{"t":"evt","seq":14,"boot":"3bf0304f","ev":"pending","count":0}
```
(`→` host to device, `←` device to host; echo, prompt and log lines omitted. The `input`
event in this example assumes `OUT0` is wired to `IN0`.)
