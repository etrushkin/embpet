---
name: wifi-connect
description: Connect the embpet ESP32-C3 board to Wi-Fi over its USB serial CLI. Use when asked to set, change or check the board's Wi-Fi credentials, connect the board to the network, or when the board reports wifi down / Telegram offline because it has no network.
---

# Wi-Fi connect (embpet board)

Provision Wi-Fi on the board through the serial CLI defined in `README.md` §3. Credentials come from the git-ignored `.env.local`. Never copy them into committed files, commit messages, beads issues, logs or notes, and never print them.

## Preconditions

1. **Board attached:** `ls /dev/cu.usbmodem*`. The dev unit is usually `/dev/cu.usbmodem11101`, a USB-Serial/JTAG device with VID 303A and PID 1001. If several ports are listed, pass `-p`.
2. **Port free:** close any `idf.py monitor`, `screen`, or broker that holds the port; only one process can open it. Agents run every board command through `scripts/hw.sh`, which holds the board lock.
3. **Firmware running:** `scripts/hw.sh python tools/embpet_cli.py sys info` must return `"ok":true`. A timeout means the embpet firmware isn't flashed or the board is stuck. Stop and report that; don't retry endlessly.
4. **Credentials present:** `.env.local` defines `WIFI_SSID` and `WIFI_PASS`. If it's missing, copy `.env.example` and ask the user for the values.

## Steps

**Agents:** use the helper. It parses `.env.local` itself (no sourcing), runs `wifi set`, polls `wifi status` for up to 30 s, and prints only the two responses with the SSID and password masked as `***`:

```bash
scripts/hw.sh python tools/wifi_from_env.py
```

Exit 0 means the board is `"state":"up"` with an IP. Report the state, IP and RSSI; **never print the password**.

**Humans** may do it by hand from the repo root (`. scripts/export.sh` first):

```bash
set -a; . ./.env.local; set +a
python tools/embpet_cli.py -t 20 --show-events wifi set -- "$WIFI_SSID" "$WIFI_PASS"
python tools/embpet_cli.py wifi status
```

- Put the values after `--` so one that starts with `-` is not taken as an option.
- The `wifi set` response `"ok":true` only means the credentials were stored (and survive reboots). Connection happens asynchronously; a `wifi` event follows when the state changes. An open network takes no password.
- `wifi status` shows `state` (`up`, `down` or `unconfigured`), `ssid`, `txpower`, and `ip` and `rssi` when up.

## Troubleshooting

| Symptom | Action |
|---|---|
| `state` stays `down`, or associates then drops | Super Mini TX-power issue: `scripts/hw.sh python tools/embpet_cli.py wifi txpower 8.5` (try 7–11). Keep wires away from the antenna end (GPIO20/21). |
| `state` stays `down` after several wrong attempts | The firmware retries with backoff (1 s up to 30 s). Some routers briefly refuse a client after repeated failed WPA3 handshakes; wait a minute. |
| Network not found | The C3 is 2.4 GHz only. Check that the SSID is a 2.4 GHz band and not hidden or 5 GHz-only. |
| Auth failure | Re-check `.env.local`; WPA2/WPA3-Personal only, no enterprise or captive portal. |
| `state` is `unconfigured` | No SSID is stored: run the helper. |
| Board resets when the port opens | Something opened the port with DTR/RTS asserted, or cleared DTR before RTS (README §3.1). Use `tools/embpet_cli.py`, which clears RTS first, then DTR. |
| `timeout: no response` | Run with `--raw` to see console output; the board may be printing a boot loop or panic. |

## Notes

- The dev network is `singular` (WPA3-SAE capable); the password lives only in `.env.local`.
- Device test for this flow: `scripts/hw.sh python tools/wifi_test.py [--flash BUILD_DIR] all`. The separate `unconfigured` suite erases the whole NVS partition (Telegram token, owner and boot levels too), so run it only on purpose.
