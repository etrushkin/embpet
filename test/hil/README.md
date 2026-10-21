# Hardware-in-the-loop tests (test/hil)

pytest suites that talk to a real board over the USB serial console. They target the
**production firmware** (`build/`, no test-only Kconfig options).

```bash
scripts/dev-venv.sh                                         # once: creates .venv
scripts/hw.sh python tools/hil.py -m "not owner and not loopback"    # default run, about 2 min
scripts/hw.sh python tools/hil.py -k wifi                   # one suite
scripts/hw.sh python tools/hil.py -m "not slow"             # skip reboots and Wi-Fi outages
```

`tools/hil.py` runs the project `.venv` pytest; `scripts/hw.sh` holds the board lock for the
whole run, so nobody flashes or monitors meanwhile. Close `idf.py monitor` first. Without a
board every test is skipped, unless `--require-board` or `EMBPET_PORT` is given.

## Suites

| File | Covers |
|---|---|
| `test_smoke.py` | the four checks every firmware build passes |
| `test_protocol.py` | frame format, `seq`/`boot`, rid and actor echo, errors, bad quoting, `too_long`, `evt since` (replay, verbatim, bad seq), reboot and the boot event |
| `test_io.py` | `io get/set/pulse` (timing through `remaining_ms` and the observed pulse end), re-pulse, cancel by set/toggle, error cases, `io boot` levels across `sys reboot`, `io in` shape, `io debounce` settings and persistence |
| `test_wifi.py` | `wifi status`, `wifi txpower`, `wifi set` reconnect, no password in output at debug level, wrong password -> down -> recovery, credentials across a reboot |
| `test_tg.py` | `tg status` (masked token), `tg pending`/`tg ack` error handling, `tg send` checks that fail before the network (`bad_args`, `too_long`, `not_configured`), `tg token` bad input, `tg owner` argument checks |
| `test_tg_offline.py` | one Wi-Fi outage shared by the tests: `tg send` -> `offline` (with a placeholder owner chat that is cleared again), `not_configured` first, `tg token` -> `offline`, `tg owner` set/show/clear |
| `test_tg_owner.py` | marker `owner`: real sends to the paired owner and human replies |
| `test_io_loopback.py` | marker `loopback`: input events through OUTn -> opto -> IN wiring |

Markers (`pytest.ini`): `smoke`, `owner`, `loopback`, `slow` (reboots, Wi-Fi outages).

## Owner suite (marker `owner`)

Skipped unless selected with `-m owner` or `EMBPET_HIL_OWNER=1`. Needs Wi-Fi up, the bot token
stored, an owner paired (`tg status` shows `owner`; `tools/tg_pair.py` pairs one) and a
terminal. It **sends short messages tagged `[hil-e3d]` to the owner chat** (and nothing to any
other chat), acknowledges each one afterwards, and prompts the human where a Telegram reply is
needed:

```bash
scripts/hw.sh python tools/hil.py -m owner
```

Steps with a prompt: Reply routing (`src: table`), `reply` before the `pending` event, a
message without Reply (hint, no event), Reply after `sys reboot` (`src: prefix`). Type `skip`
at a prompt to skip that test. Telethon automation of the replies is undecided (bead
embpet-vgj) and not implemented.

## Loopback suite (marker `loopback`)

Skipped unless `-m loopback` or `EMBPET_HIL_LOOPBACK=1`. Wire each output so that `io set n on`
energises an input's opto (README 1.3). Pairs are `OUT:IN` in `EMBPET_LOOPBACK`, default
`0:0,1:1,2:2` (logical numbers; GPIO4 -> GPIO10, GPIO3 -> GPIO20, GPIO2 -> GPIO21). Covers
press/release events, `io in`, a timed pulse, a pulse shorter than the debounce time (no event, counted in `extra`), debounce 0, and `evt since` reporting
`truncated: true` after more than 32 events (this is the only production-firmware check of
that flag; the ring logic itself has host tests).

## Do not use `-l` / `--showlocals` / `--tb=long`

They print fixture and test locals on a failure, and those include `env` (the `.env.local`
values: Wi-Fi password, bot token). Keep the default traceback style (or `--tb=short`).

## Coverage gaps

- `evt since` with `truncated: true` is checked only by the loopback suite (it needs more than
  32 events); the default run checks `truncated: false`. The ring logic has host tests.
- Reply routing (`reply` events, `src` table/prefix, the hint for a message without Reply) needs
  the human owner: owner suite only.
- The online notice after boot and a successful `tg token` (valid token stored, bridge
  restarted) are covered only by the owner suite and the device scripts (`tools/pair_test.py`,
  `tools/tg_test.py`); the default run exercises `tg token` only with bad input and with
  Wi-Fi down, so it never changes the token.
- Input edges need the loopback wiring; the default run covers only `io in` and the debounce
  settings.

## State restore

A session fixture (`board_guard` in `conftest.py`) snapshots the settings before the first
test and puts them back after the last one, also after failures: boot levels, debounce times,
output levels, log level, Wi-Fi TX power, owner, Wi-Fi (credentials from `.env.local`) and the
bot token (from `.env.local`). Tests that change things also restore them in their own
fixtures (`restore_io_state`, `wifi_restore`, the module Wi-Fi outage fixture). The NVS is
never erased. Limits of the restore:

- Fixtures cannot undo a hard kill of the run (SIGKILL, a dropped port, power loss). After one,
  check `tg status` (a leftover placeholder owner `1` is skipped by the owner suite; clear it
  with `tg owner --clear`), `wifi status` and `io get`.
- The Wi-Fi-changing tests are skipped when the board starts on an SSID that is neither empty
  nor the `.env.local` one (its password is unknown). A board with no SSID ends up on the
  `.env.local` network.
- The bot token is put back only if the original was the `.env.local` one (compared by its
  masked form); otherwise, or if there was none, the teardown reports that it cannot restore it.

Secrets stay in `.env.local`: they are sent in-process, never put in argv, and
no assertion prints them.
