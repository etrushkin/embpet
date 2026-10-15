---
name: wifi-connect
description: Connect the embpet ESP32-C3 board to Wi-Fi over its USB serial CLI. Use when asked to set, change or check the board's Wi-Fi credentials, connect the board to the network, or when the board reports wifi down / Telegram offline because it has no network.
---

# Wi-Fi connect (embpet board)

Provision Wi-Fi on the board through the serial CLI defined in `README.md` §3. Credentials come from the git-ignored `.env.local`. Never copy them into committed files, commit messages, or beads issues.

## Preconditions

1. **Board attached:** `ls /dev/cu.usbmodem*`. The dev unit is usually `/dev/cu.usbmodem11101`, a USB-Serial/JTAG device with VID 303A and PID 1001. If several ports are listed, pass `-p`.
2. **Port free:** close any `idf.py monitor`, `screen`, or broker that holds the port; only one process can open it. `lsof /dev/cu.usbmodem*` shows the holder.
3. **Python with pyserial:** use the ESP-IDF environment (`. scripts/export.sh` from the repo root), whose `python` has pyserial. Without ESP-IDF, use a venv: `python3 -m venv .venv && .venv/bin/pip install pyserial`.
4. **Firmware running:** `python tools/embpet_cli.py sys info` must return `"ok":true`. A timeout means the embpet firmware isn't flashed or the board is stuck. Stop and report that; don't retry endlessly.
5. **Credentials present:** `.env.local` defines `WIFI_SSID` and `WIFI_PASS`. If it's missing, copy `.env.example` and ask the user for the values.

## Steps

Run from the repo root:

```bash
set -a; . ./.env.local; set +a
python tools/embpet_cli.py -t 20 --show-events wifi set "$WIFI_SSID" "$WIFI_PASS"
```

- The response `"ok":true` only means the credentials were stored. Connection happens asynchronously.
- Confirm with `wifi status` and wait up to about 20 s for `"state":"up"`:

```bash
for i in 1 2 3 4; do python tools/embpet_cli.py wifi status && break; sleep 5; done
```

If the state is `"up"`, report the SSID, IP and RSSI. **Never print the password.**

## Troubleshooting

| Symptom | Action |
|---|---|
| `state` stays `down`, or associates then drops | Super Mini TX-power issue: `python tools/embpet_cli.py wifi txpower 8.5` (try 7–11). Keep wires away from the antenna end (GPIO20/21). |
| Network not found | The C3 is 2.4 GHz only. Check that the SSID is a 2.4 GHz band and not hidden or 5 GHz-only. |
| Auth failure | Re-check `.env.local`; WPA2/WPA3-Personal only, no enterprise or captive portal. |
| Board resets when the port opens | Something opened the port with DTR/RTS asserted, or cleared DTR before RTS (README §3.1). Use `tools/embpet_cli.py`, which clears RTS first, then DTR. |
| `timeout: no response` | Run with `--raw` to see console output; the board may be printing a boot loop or panic. |

## Notes

- The CLI is a draft until the firmware exists. If a command errors with an unknown name, check `README.md` §3.4 and update this skill to match.
- The dev network is `singular`; the password lives only in `.env.local`.
