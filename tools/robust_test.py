#!/usr/bin/env python3
"""Robustness scenarios for embpet-dk1. Needs the bridge TEST build
(components/tgbot/bridgetest.defaults: `tg t_fault`, `tg t_pend`, `tg t_bridge`), Wi-Fi and
TG_TOKEN in .env.local. Nothing is sent to Telegram. Run under the board lock:

  scripts/hw.sh python tools/robust_test.py [-p PORT] [--flash BUILD_DIR] SCENARIO

Scenarios (each recovers on its own, nothing is repaired by hand in between):
  wifi        AP loss emulated with a non-existent SSID and with the real SSID and a wrong
              password (the router cannot be switched off): Wi-Fi goes down, Telegram goes
              offline, both return after the credentials are restored; `wifi txpower` change
              and back. Same boot throughout, heap_min stays above the 60 KB budget.
  telegram    getUpdates failing (test fault) for 150 s: attempts back off to the 30 s cap,
              no tg event flapping; recovery within seconds after the fault ends.
  reboot      5 pending entries (`tg t_pend`), `sys reboot`: new boot id, empty pending table
              (RAM only), Wi-Fi and Telegram come back, no crash reset reason.
  token       the same valid token set 5 times with `tg token` while polling runs: polling
              resumes, tg stays or returns online, free heap does not drift.
  all         the four above in this order.

Not emulated (see the bead notes): DNS failure (no hook), HTTP 429 (tgapi's 429 parsing and
retry_after are covered by the host tests), host port closed for hours (tools/soak24.py).

After every scenario: no fault, owner 0, Wi-Fi and token from .env.local, outputs off. The
token, Wi-Fi password and SSID are never printed. Exit 0 = no check failed, 1 = a check
failed, 2 = setup error.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from embpet import CmdTimeout, NoBoard, find_port  # noqa: E402
import bridge_test as BT  # noqa: E402
import tg_from_env as T  # noqa: E402
import wifi_from_env as W  # noqa: E402
from wifi_test import Checks, flash  # noqa: E402

HEAP_MIN_BUDGET = 60 * 1024
NOSUCH_SSID = "embpet-no-such-network"


def info(msg):
    print("INFO " + msg, flush=True)


def sys_info(x):
    return x.cmd("sys info", timeout=10.0)


def creds(x):
    return [x.ssid] + ([x.password] if x.password else [])


def heap_ok(x, name):
    si = sys_info(x)
    info("%s: heap_free %s heap_min %s uptime %s" %
         (name, si.get("heap_free"), si.get("heap_min"), si.get("uptime_s")))
    x.c.check(name + ": heap_min above 60 KB", (si.get("heap_min") or 0) > HEAP_MIN_BUDGET, si)
    return si


def wait_tg(x, want, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if x.tg_state() == want:
            return True
        x.pump(1.0)
    return False


def outage(x, name, ssid, password, hold):
    """Wi-Fi credentials that cannot associate: down, Telegram offline, then restored."""
    c = x.c
    boot0 = sys_info(x).get("boot")
    x.take()
    x.cmd("wifi set", text=[ssid] + ([password] if password else []))
    c.check(name + ": wifi goes down", x.wait_wifi("down", 30))
    x.pump(hold)
    st = x.cmd("tg status")
    c.check(name + ": tg offline while wifi is down", st.get("state") == "offline",
            BT.redact(st))
    c.check(name + ": still down after %d s of retries (bounded backoff, no crash)" % hold,
            x.wifi_state() == "down")
    t0 = time.monotonic()
    x.cmd("wifi set", text=creds(x))
    up = x.wait_wifi("up", 60)
    t_up = time.monotonic() - t0
    c.check(name + ": wifi up again after restore", up)
    online = wait_tg(x, "online", 60)
    info("%s: wifi up %.1f s, tg online %.1f s after restore" %
         (name, t_up, time.monotonic() - t0))
    c.check(name + ": tg online again", online)
    x.pump(1.0)
    ev = [(e.get("ev"), e.get("state")) for e in x.take() if e.get("ev") in ("wifi", "tg")]
    info("%s: events %s" % (name, ev))
    c.check(name + ": wifi down and up events seen",
            ("wifi", "down") in ev and ("wifi", "up") in ev, ev)
    c.check(name + ": tg offline and online events seen",
            ("tg", "offline") in ev and ("tg", "online") in ev, ev)
    c.check(name + ": same boot (no reset)",
            boot0 is not None and sys_info(x).get("boot") == boot0, boot0)


def sc_wifi(x):
    c = x.c
    reason0 = sys_info(x).get("reset_reason")
    outage(x, "wifi/no-such-ssid", NOSUCH_SSID, "embpet-wrong-pass", 60)
    outage(x, "wifi/wrong-password", x.ssid, "embpet-wrong-pass-0", 60)
    tx0 = x.cmd("wifi status").get("txpower")
    r = x.cmd("wifi txpower", "5")
    c.check("wifi: txpower 5 accepted", r.get("ok"), r)
    x.pump(5.0)
    st = x.cmd("wifi status")
    c.check("wifi: txpower applied and still up", st.get("txpower") == 5 and
            st.get("state") == "up", BT.redact(st))
    r = x.cmd("wifi txpower", str(tx0))
    c.check("wifi: txpower restored", r.get("ok") and
            x.cmd("wifi status").get("txpower") == tx0, r)
    si = heap_ok(x, "wifi")
    c.check("wifi: reset_reason unchanged", si.get("reset_reason") == reason0)


def sc_telegram(x):
    c = x.c
    x.take()
    a = x.cmd("tg t_bridge")
    x.cmd("tg t_fault", "1")
    t0 = time.monotonic()
    deadline = t0 + 75
    while time.monotonic() < deadline and not any(
            e.get("ev") == "tg" and e.get("detail") == "test fault" for e in x.events):
        x.pump(1.0)
    c.check("telegram: fault -> tg offline event",
            any(e.get("ev") == "tg" and e.get("state") == "offline" for e in x.events))
    x.take()
    b0 = x.cmd("tg t_bridge")
    # sample the attempt counter every 2 s: a count alone cannot tell the 30 s cap from
    # uncapped doubling (about 8 vs 7 attempts in 150 s), the gap between attempts can
    seen, last, gaps = b0.get("polls", 0), time.monotonic(), []
    end = last + 150.0
    while time.monotonic() < end:
        x.pump(2.0)
        n = x.cmd("tg t_bridge").get("polls", seen)
        if n > seen:
            gaps.append(round(time.monotonic() - last, 1))
            seen, last = n, time.monotonic()
    b1 = x.cmd("tg t_bridge")
    ev = x.take("tg")
    polls = b1.get("polls", 0) - b0.get("polls", 0)
    gap = max(gaps[1:] + [time.monotonic() - last]) if gaps else 150.0
    info("telegram: %d getUpdates attempts in 150 s of fault, gaps %s s, %d tg events, "
         "probes %d" % (polls, gaps, len(ev), b1.get("probes", 0) - b0.get("probes", 0)))
    c.check("telegram: backoff keeps retrying (>= 5 attempts in 150 s)", polls >= 5, polls)
    c.check("telegram: backoff capped (longest gap <= 40 s, cap 30 s)", gap <= 40.0,
            round(gap, 1))
    c.check("telegram: no tg event flapping during the fault", ev == [], ev)
    c.check("telegram: still offline during the fault", x.tg_state() == "offline")
    t1 = time.monotonic()
    x.cmd("tg t_fault", "0")
    online = wait_tg(x, "online", 60)
    info("telegram: online %.1f s after the fault ended" % (time.monotonic() - t1))
    c.check("telegram: recovers without manual action (within 35 s)",
            online and time.monotonic() - t1 < 35)
    # a long poll lasts CONFIG_EMBPET_TG_POLL_TIMEOUT_S (50 s); the next one starts after it
    p0 = x.cmd("tg t_bridge").get("polls", 0)
    t2, p1 = time.monotonic(), p0
    while p1 <= p0 and time.monotonic() - t2 < 120.0:
        x.pump(5.0)
        p1 = x.cmd("tg t_bridge").get("polls", p0)
    info("telegram: after recovery polls %d -> %d in %.0f s" % (p0, p1, time.monotonic() - t2))
    c.check("telegram: polling continues after recovery (a new getUpdates within 120 s)",
            p1 > p0 and x.tg_state() == "online", (p0, p1))
    heap_ok(x, "telegram")
    info("telegram: DNS failure not emulated (no hook); HTTP 429 covered by host tests only "
         "(started with %s polls)" % a.get("polls"))


def sc_reboot(x):
    c = x.c
    si0 = sys_info(x)
    x.take()
    for i in range(5):
        r = x.cmd("tg t_pend", str(900000 + i), actor="SOAK")
        if not r.get("ok"):
            c.check("reboot: t_pend %d accepted" % i, False, r)
    n = x.cmd("tg pending")
    c.check("reboot: 5 pending entries before reboot", len(n.get("items") or []) == 5,
            BT.redact(n))
    x.cmd("sys reboot")
    x.b.reconnect(delay=3.0, timeout=40.0)
    ready = None
    try:
        ready = x.b.wait_ready(30)
    except Exception as e:
        c.check("reboot: firmware answers after reboot", False, type(e).__name__)
        return
    c.check("reboot: new boot id", ready.get("boot") != si0.get("boot"), ready.get("boot"))
    reason = ready.get("reset_reason")
    info("reboot: reset_reason after sys reboot: %s" % reason)
    c.check("reboot: software reset reason, not a crash",
            str(reason).lower() in ("sw", "software", "esp_rst_sw"), reason)
    n = x.cmd("tg pending")
    c.check("reboot: pending table empty (RAM only)", n.get("ok") and (n.get("items") or []) == [],
            BT.redact(n))
    c.check("reboot: tg status pending 0", x.cmd("tg status").get("pending") == 0)
    c.check("reboot: wifi comes back by itself", x.wait_wifi("up", 60))
    c.check("reboot: tg online again by itself", wait_tg(x, "online", 60))
    heap_ok(x, "reboot")


def sc_token(x):
    c = x.c
    x.take()
    heaps = []
    p_before = x.cmd("tg t_bridge").get("polls", 0)
    for i in range(5):
        code, res, err = T.set_token(x.token, board=x.b)
        c.check("token: same token accepted (round %d)" % (i + 1), code == 0,
                T.W.mask(res, T.secrets_of(x.token)) if res else err)
        c.check("token: online after the restart (round %d)" % (i + 1), wait_tg(x, "online", 30))
        x.pump(15.0)
        heaps.append(sys_info(x).get("heap_free") or 0)
    info("token: heap_free after each round %s" % heaps)
    c.check("token: heap_free does not drift (max-min < 4 KB)", max(heaps) - min(heaps) < 4096,
            heaps)
    c.check("token: polling resumed", x.cmd("tg t_bridge").get("polls", 0) > p_before)
    c.check("token: token still masked in tg status",
            x.cmd("tg status").get("token") == T.masked(x.token))
    heap_ok(x, "token")


SCENARIOS = [("wifi", sc_wifi), ("telegram", sc_telegram), ("reboot", sc_reboot),
             ("token", sc_token)]


def reset_state(x):
    """Wi-Fi and token from .env.local, no fault, owner 0, outputs off."""
    try:
        x.restore()
        for pin in range(8):
            x.cmd("io set", str(pin), "off")
    except Exception as e:
        info("reset_state error %s" % type(e).__name__)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-p", "--port")
    ap.add_argument("--flash", metavar="BUILD_DIR")
    ap.add_argument("scenario", choices=[n for n, _ in SCENARIOS] + ["all"])
    a = ap.parse_args()
    try:
        env = W.load_env(os.path.join(W.ROOT, ".env.local"))
    except OSError:
        print("cannot read .env.local", file=sys.stderr)
        return 2
    token, ssid = env.get("TG_TOKEN", ""), env.get("WIFI_SSID", "")
    if not token or not ssid or "WIFI_PASS" not in env:
        print("TG_TOKEN / WIFI_SSID / WIFI_PASS missing in .env.local", file=sys.stderr)
        return 2
    BT.BOT_ID = int(token.split(":", 1)[0]) if token.split(":", 1)[0].isdigit() else 0
    port = a.port or find_port()
    if a.flash:
        flash(port, a.flash)
    checks = Checks(T.secrets_of(token) + [env["WIFI_PASS"], ssid])
    x = BT.Ctx(port, token, ssid, env["WIFI_PASS"], 0, checks)
    try:
        x.open()
    except (CmdTimeout, NoBoard, OSError) as e:
        print("board not ready: %s" % type(e).__name__, file=sys.stderr)
        x.close()
        return 2
    try:
        x.cmd("tg t_owner", "0")
        if x.wifi_state() != "up":
            x.cmd("wifi set", text=creds(x))
            if not checks.check("setup: wifi up", x.wait_wifi("up", 60)):
                return 2
        if x.cmd("tg status").get("token") != T.masked(token):
            T.set_token(token, board=x.b)
        if not checks.check("setup: tg online", wait_tg(x, "online", 60)):
            return 2
        for name, fn in SCENARIOS:
            if a.scenario in (name, "all"):
                print("=== scenario %s" % name, flush=True)
                try:
                    fn(x)
                except Exception as e:
                    checks.check("%s: no exception" % name, False, type(e).__name__)
                reset_state(x)
        reset_state(x)
        c2 = x.cmd("tg status")
        checks.check("final: wifi up, tg online, owner null",
                     x.wifi_state() == "up" and c2.get("state") == "online" and
                     c2.get("owner") is None, BT.redact(c2))
    finally:
        x.close()
    failed = [r["check"] for r in checks.results if not r["ok"]]
    print(json.dumps({"ok": checks.ok(), "passed": len(checks.results) - len(failed),
                      "failed": failed}))
    return 0 if checks.ok() else 1


if __name__ == "__main__":
    sys.exit(main())
