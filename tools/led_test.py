#!/usr/bin/env python3
"""Device test for the status LED (embpet-m5o, README 2.2). Needs the LED TEST build
(CONFIG_EMBPET_LED_TEST with the bridge test commands, components/io/ledtest.defaults) and
Wi-Fi + TG_TOKEN in .env.local. Run it under the board lock so flashing and testing are one
locked unit:

  scripts/hw.sh python tools/led_test.py [-p PORT] [--flash BUILD_DIR]

--flash writes BUILD_DIR/flash_args and then erases the NVS partition (fresh, unconfigured
board). Without it the board must already be unconfigured for the first check.

No one watches the LED: the test build makes GPIO8 INPUT_OUTPUT and an any-edge ISR timestamps
the pin (`led t_edges`), so the electrical pin level and its timing are what is checked.
Polarity comes from the build (active low: logical on = pin low). Checked in order:
  unconfigured (NVS erased)         -> 1 Hz blink, 500 ms on / 500 ms off
  booting flag set (led t_boot 1)   -> solid on, and it beats the blink
  Wi-Fi configured but down         -> 1 Hz blink
  Wi-Fi up, token, tg online        -> off
  pending via `tg t_pend` (real table path, nothing is sent) -> 5 x (100 on / 100 off), then
                                      3.1 s off (4 s cycle)
  offline while pending             -> blink again (the pending pattern is hidden)
  back online, `tg ack --older 0`   -> off
plus the esp_timer task stack headroom.

Prints PASS/FAIL lines and a JSON summary; exit 0 = all passed, 1 = a check failed,
2 = setup error. Credentials are never printed. The run erases NVS and restores the Wi-Fi
credentials and token at the end; if it dies early, a re-run or the wifi-connect helper
(tools/wifi_from_env.py) recovers them.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from embpet import find_port  # noqa: E402
import bridge_test as B  # noqa: E402
import tg_from_env as T  # noqa: E402
import wifi_from_env as W  # noqa: E402
from wifi_test import Checks, flash  # noqa: E402

TOL_MS = 40.0           # 50 ms tick quantisation plus timer jitter, per interval


class Led:
    def __init__(self, x):
        self.x = x
        self.active_low = True

    def drain(self):
        r = self.x.cmd("led t_edges")
        if r.get("active_low") is not None:
            self.active_low = r["active_low"]
        return r

    def window(self, seconds):
        """Edges recorded over SECONDS: ([(is_on, t_ms)], is_on at the end, drain res)."""
        self.drain()
        self.x.pump(seconds)
        r = self.drain()
        on = 0 if self.active_low else 1
        edges = [(lvl == on, t / 1000.0) for lvl, t in r.get("edges", [])]
        return edges, r.get("level") == on, r


def intervals(edges):
    """[(is_on, duration_ms)] of the completed intervals between consecutive edges."""
    return [(a[0], b[1] - a[1]) for a, b in zip(edges, edges[1:])]


def near(v, want):
    return abs(v - want) <= TOL_MS


def is_blink(edges):
    iv = intervals(edges)
    return (len(iv) >= 6 and all(near(d, 500.0) for _, d in iv) and
            all(iv[i][0] != iv[i + 1][0] for i in range(len(iv) - 1)))


def is_burst(edges):
    """5 x (100 on / 100 off), then 3100 ms off; the off gaps go 100 x4 then 3100."""
    iv = intervals(edges)
    ons = [d for o, d in iv if o]
    offs = [d for o, d in iv if not o]
    if len(ons) < 8 or not all(near(d, 100.0) for d in ons):
        return False
    long_offs = [d for d in offs if d > 1000.0]
    short_offs = [d for d in offs if d <= 1000.0]
    return (len(long_offs) >= 2 and all(near(d, 3100.0) for d in long_offs) and
            all(near(d, 100.0) for d in short_offs) and
            len(short_offs) >= 6)       # a 13 s window holds at least 2 long gaps


def solid(edges, lit, level):
    return not edges and level == lit


def suite(x, c, led):
    # 1. unconfigured (booting is over by the time a console exists)
    e, lvl, r = led.window(5.0)
    c.check("test build running (led t_edges answers)", r.get("ok"), r)
    c.check("unconfigured: 1 Hz blink 500/500 ms (%d edges)" % len(e), is_blink(e),
            intervals(e)[:8])
    print("INFO active_low=%s dropped=%s timer_stack_free_min=%s" %
          (led.active_low, r.get("dropped"), r.get("timer_stack_free_min")), flush=True)

    # 2. booting beats everything
    x.cmd("led t_boot", "1")
    x.pump(0.5)
    e, lvl, r = led.window(3.0)
    c.check("booting: solid on (no edges, pin lit)", solid(e, True, lvl), (len(e), lvl))
    x.cmd("led t_boot", "0")
    x.pump(0.5)
    e, lvl, r = led.window(5.0)
    c.check("booting cleared: blink again", is_blink(e), intervals(e)[:8])

    # 3. configured, Wi-Fi down (no such network)
    x.cmd("wifi set", text=[B.NOSUCH_SSID])
    x.pump(2.0)
    c.check("wifi status is down", x.wifi_state() == "down", x.wifi_state())
    e, lvl, r = led.window(5.0)
    c.check("Wi-Fi down: 1 Hz blink", is_blink(e), intervals(e)[:8])

    # 4. online, nothing pending
    x.cmd("wifi set", text=[x.ssid] + ([x.password] if x.password else []))
    if not c.check("setup: wifi up", x.wait_wifi("up", 60)):
        return
    c.check("setup: token stored", x.set_token())
    if not c.check("setup: tg online", x.wait_online(60), x.cmd("tg status")):
        return
    x.cmd("tg ack", "--older", "0")
    x.pump(1.0)
    e, lvl, r = led.window(6.0)
    c.check("online, nothing pending: off (no edges, pin dark)", solid(e, False, lvl),
            (len(e), lvl))

    # 5. pending through the real table path
    x.cmd("tg t_pend", "930001", actor="L")
    x.pump(0.5)
    st = x.cmd("tg pending")
    c.check("pending > 0 in the table", bool(st.get("items")), st)
    e, lvl, r = led.window(13.0)
    c.check("pending: 5 x 100/100 ms burst, then 3.1 s off (%d edges)" % len(e), is_burst(e),
            [(o, round(d)) for o, d in intervals(e)[:12]])

    # 6. pending is hidden while offline
    x.cmd("wifi set", text=[B.NOSUCH_SSID])
    x.wait_wifi("down", 20)
    x.pump(1.0)
    e, lvl, r = led.window(5.0)
    c.check("offline while pending: 1 Hz blink", is_blink(e), intervals(e)[:8])
    x.cmd("wifi set", text=[x.ssid] + ([x.password] if x.password else []))
    c.check("back: wifi up", x.wait_wifi("up", 60))
    c.check("back: tg online", x.wait_online(60))
    x.pump(1.0)
    e, lvl, r = led.window(13.0)
    c.check("back online, still pending: burst again", is_burst(e),
            [(o, round(d)) for o, d in intervals(e)[:12]])

    # 7. answered / acked -> off
    ack = x.cmd("tg ack", "--older", "0")
    x.pump(1.0)
    e, lvl, r = led.window(6.0)
    c.check("acked (%s): off" % ack.get("acked"), solid(e, False, lvl) and ack.get("acked", 0) >= 1,
            (len(e), lvl, ack))

    r = led.drain()
    free = r.get("timer_stack_free_min")
    print("INFO esp_timer task stack free (lifetime min): %s B" % free, flush=True)
    c.check("esp_timer stack keeps >= 1500 B free", (free or 0) >= 1500, free)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-p", "--port")
    ap.add_argument("--flash", metavar="BUILD_DIR")
    a = ap.parse_args()
    try:
        env = W.load_env(os.path.join(W.ROOT, ".env.local"))
    except OSError:
        print("cannot read .env.local", file=sys.stderr)
        return 2
    token = env.get("TG_TOKEN", "")
    ssid = env.get("WIFI_SSID", "")
    if not token or not ssid or "WIFI_PASS" not in env:
        print("TG_TOKEN / WIFI_SSID / WIFI_PASS missing in .env.local", file=sys.stderr)
        return 2
    port = a.port or find_port()
    c = Checks(T.secrets_of(token) + [env["WIFI_PASS"]])
    x = B.Ctx(port, token, ssid, env["WIFI_PASS"], 0, c)
    try:
        if a.flash:
            flash(port, a.flash)
            flash(port, a.flash, erase_nvs=True)
        x.open()
        suite(x, c, Led(x))
    finally:
        try:
            x.restore()
        finally:
            x.close()
    failed = [r["check"] for r in c.results if not r["ok"]]
    print(json.dumps({"ok": c.ok(), "passed": len(c.results) - len(failed), "failed": failed}))
    return 0 if c.ok() else 1


if __name__ == "__main__":
    sys.exit(main())
