#!/usr/bin/env python3
"""Device tests for the Telegram Bot API client (embpet-q9h). Needs the TEST build
(CONFIG_EMBPET_TG_TEST, components/tgbot/tgtest.defaults), Wi-Fi and TG_TOKEN in .env.local.
Run it under the board lock so flashing and testing are one locked unit:

  scripts/hw.sh python tools/tg_test.py [-p PORT] [--flash BUILD_DIR] SUITE [--seconds S]

Suites:
  api     with the console at DEBUG and every console line captured in memory: store the
          token (in-process, never in argv), getMe (TLS through the certificate bundle),
          deleteWebhook, getUpdates, sendMessage with the bold [A] entity, as a reply and as
          plain text plus the response-size measurement (owner chat only, see below), the
          too_long limit, an auth_error round trip with a fake token, `tg` events on every
          transition, `sys log` toggles during traffic, reconnect after 60 s idle, and
          finally: the captured output contains no part of the token.
  soak    S seconds (default 3600) of long polling plus getMe every 15 s and sendMessage
          every 600 s to the owner chat (getMe only without one), sampling `sys info` and
          `tg t_stat` every 60 s; afterwards the sendMessage checks run against the owner
          chat. Confirms (consumes) every update it receives.

Messages go ONLY to the explicitly configured owner chat: --chat, else TG_OWNER_CHAT in the
git-ignored .env.local. Without one every sending check is skipped (reported as PENDING);
the tool never picks a chat from incoming updates.

Output is check names with PASS/FAIL, INFO lines with message ids, timings and heap
numbers, and for soak one line per sample. Message text, chat ids, the token and the Wi-Fi
password are never printed. Exit 0 = all checks passed, 1 = a check failed, 2 = setup error.
"""
import argparse
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from embpet import Board, CmdTimeout, find_port  # noqa: E402
import tg_from_env as T  # noqa: E402
import wifi_from_env as W  # noqa: E402
from wifi_test import Checks, flash  # noqa: E402

FAKE_TOKEN = "123456:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"     # well-formed, not a real bot
ACTOR = "q9h"
# "[q9h] " + this; bold entity over "[q9h]" = 5 UTF-16 units
SEND_TEXT = ("embpet q9h test ✅ Привет "
             "\U0001F600 \U00020000 end")
TG_STATES = ("unconfigured", "offline", "online", "auth_error")


class Ctx:
    def __init__(self, board, token, checks):
        self.b = board
        self.token = token
        self.c = checks
        self.lines = []
        self.events = []
        board.on_line = self.lines.append
        board.on_event = self.events.append

    def cmd(self, command, *args, text=(), actor=None, timeout=40.0):
        try:
            return self.b.cmd(command, *args, text=text, actor=actor, timeout=timeout)
        except CmdTimeout:
            return {}

    def tg_events(self):
        """tg events seen since the last call: [(state, detail)]."""
        out = [(e.get("state"), e.get("detail")) for e in self.events if e.get("ev") == "tg"]
        self.events.clear()
        return out

    def settle(self, seconds=1.0):
        """Read the console for a moment so trailing events / log lines are captured."""
        for f in self.b.frames(seconds):
            if f.get("t") == "evt":
                self.events.append(f)


def wifi_up(port):
    """Make sure Wi-Fi is up (provision from .env.local if not)."""
    with Board(port) as b:
        b.wait_ready(20)
        st = b.cmd("wifi status", timeout=10)
    if st.get("state") == "up":
        return True
    env = W.load_env(os.path.join(W.ROOT, ".env.local"))
    W.wifi_set(env.get("WIFI_SSID", ""), env.get("WIFI_PASS", ""), port)
    return (W.wait_up(port, 45) or {}).get("state") == "up"


def check_send(x, chat, suite):
    """sendMessage with the bold [A] entity, then as a reply, to the owner chat."""
    c = x.c
    r = x.cmd("tg t_send", str(chat), text=[SEND_TEXT], actor=ACTOR)
    want_len = len(("[%s]" % ACTOR).encode("utf-16-le")) // 2
    c.check("%s: sendMessage ok, msg id" % suite, r.get("st") == "ok" and r.get("msg", 0) > 0, r)
    c.check("%s: Telegram echoes bold entity offset 0 length %d" % (suite, want_len),
            r.get("ent_off") == 0 and r.get("ent_len") == want_len, r)
    msg = int(r.get("msg") or 0)
    r = x.cmd("tg t_send", str(chat), "-r", str(msg), text=["reply to %d" % msg], actor=ACTOR)
    c.check("%s: sendMessage as reply (reply_parameters)" % suite,
            r.get("st") == "ok" and r.get("reply_echo") == msg, r)
    print("INFO sent msg %s and reply %s to the owner chat" % (msg, r.get("msg")), flush=True)


def check_plain_and_escaping(x, chat):
    """Plain-text send (no tag, no entities), then measure how big Telegram's JSON answer
    gets per character on the wire (\\uXXXX escapes) and as the firmware holds it (escapes
    of non-ASCII rewritten to raw UTF-8 while reading), to check the getUpdates cap
    (docs/PLAN.md 1.2)."""
    c = x.c
    r = x.cmd("tg t_send", str(chat), text=["embpet q9h plain text check"])
    c.check("api: plain sendMessage ok, no entities",
            r.get("st") == "ok" and r.get("msg", 0) > 0 and r.get("ent_len") == 0, r)
    r = x.cmd("tg t_send", str(chat), text=["x"])
    base, base_held = r.get("bytes", 0), r.get("held")
    wire, held = {}, {}
    # Texts go as -n repeats of one character (a 4096-char CJK text is over the console line
    # limit). 4096 CJK chars answer with ~24.9 KB on the wire, over TG_RESP_MAX: accepted only
    # because the firmware unescapes while reading (~12.5 KB held).
    for name, ch, n in (("ascii", "x", 4000), ("cyrillic", "Ж", 2000),
                        ("cjk", "中", 4096), ("emoji", "\U0001F600", 1000)):
        r = x.cmd("tg t_send", str(chat), "-n", str(n), text=[ch])
        ok = r.get("st") == "ok"
        label = "api: %d x %s sent" % (n, name)
        if name == "cjk":
            label += " (over TG_RESP_MAX on the wire, within it held)"
            c.check(label, ok and r.get("bytes", 0) > 16384 >= r.get("held", 16385), r)
        else:
            c.check(label, ok, r)
        units = n * (len(ch.encode("utf-16-le")) // 2)
        if ok:
            wire[name] = (r["bytes"] - base) / (units - 1)
            if r.get("held") is not None and base_held is not None:
                held[name] = (r["held"] - base_held) / (units - 1)
            print("INFO response %d bytes for %d %s chars (%d UTF-16 units): %.2f bytes/unit; "
                  "held %s bytes unescaped: %s bytes/unit"
                  % (r["bytes"], n, name, units, wire[name], r.get("held"),
                     "%.2f" % held[name] if name in held else "?"), flush=True)
    if wire:
        # limit-1 update: owner reply text + quoted bot text, 4096 units each, + metadata
        est_wire = int(2 * 4096 * max(wire.values()) + 1500)
        print("INFO worst case %.2f bytes/unit on the wire -> limit-1 reply update ~%d bytes "
              "(cap TG_RESP_MAX_ONE 32768, TG_RESP_MAX 16384)"
              % (max(wire.values()), est_wire), flush=True)
        label = "api: worst-case single update fits TG_RESP_MAX_ONE unescaped"
        if len(held) == len(wire):
            worst = max(held.values())
            est = int(2 * 4096 * worst + 1500)
            print("INFO worst case %.2f bytes/unit held -> limit-1 reply update ~%d bytes"
                  % (worst, est), flush=True)
            c.check(label, est <= 32768, est)
        else:
            c.check(label, False, "no held size in the t_send answers (old firmware?)")


def suite_api(x, chat=0):
    c = x.c
    r = x.cmd("sys log", "debug")
    c.check("api: console at debug", r.get("level") == "debug", r)

    # --- token: stored in-process, answered masked
    code, r, err = T.set_token(x.token, cmd="tg t_token", board=x.b)
    r = r or {}
    c.check("api: t_token stored", code == 0, err or r)
    c.check("api: t_token answers the cfg mask only", r.get("token") == T.masked(x.token))
    x.settle(0.5)
    ev = x.tg_events()
    c.check("api: token set -> tg offline event (or none if unchanged)",
            ev in ([], [("offline", "token changed")]), ev)

    # --- getMe: TLS handshake with the bundle, then keep-alive
    r1 = x.cmd("tg t_getme")
    c.check("api: getMe ok (TLS to api.telegram.org verified by the bundle)",
            r1.get("st") == "ok" and r1.get("bot"), r1)
    bot = r1.get("bot", "")
    x.settle(0.5)
    ev = x.tg_events()
    c.check("api: getMe -> one tg online event with @bot", ev == [("online", "@" + bot)], ev)
    r2 = x.cmd("tg t_getme")
    c.check("api: second getMe ok", r2.get("st") == "ok", r2)
    print("INFO bot=@%s getMe first %s ms (handshake), second %s ms (kept-alive)" %
          (bot, r1.get("ms"), r2.get("ms")), flush=True)
    c.check("api: second getMe reuses the session (faster)",
            r2.get("ms", 1e9) < r1.get("ms", 0), (r1.get("ms"), r2.get("ms")))
    bundle = [ln for ln in x.lines if "esp-x509-crt-bundle" in ln]
    print("INFO cert bundle log lines: %d%s" %
          (len(bundle), (" (last: %s)" % bundle[-1].split(": ", 1)[-1][:80]) if bundle else ""))

    r = x.cmd("tg t_hook")
    c.check("api: deleteWebhook ok", r.get("st") == "ok", r)

    # --- getUpdates on the poll connection
    r = x.cmd("tg t_poll", "-t", "0")
    c.check("api: getUpdates ok", r.get("st") == "ok" and isinstance(r.get("count"), int), r)
    print("INFO getUpdates: %s update(s) pending (%s private messages), next offset %s" %
          (r.get("count"), r.get("private"), r.get("next")), flush=True)
    r = x.cmd("tg t_poll", "-t", "2")
    c.check("api: getUpdates long poll 2 s ok", r.get("st") == "ok" and r.get("ms", 0) >= 0, r)
    print("INFO long poll -t 2 took %s ms" % r.get("ms"))

    if chat:
        check_send(x, chat, "api")
        check_plain_and_escaping(x, chat)
    else:
        print("INFO PENDING sendMessage: no owner chat configured (--chat or TG_OWNER_CHAT "
              "in .env.local); nothing was sent", flush=True)
    # too_long is decided before anything is sent (chat 1 is never contacted)
    r = x.cmd("tg t_send", str(chat or 1), text=["x" * 4093], actor="A")
    c.check("api: [A] + 4093 chars -> too_long, nothing sent",
            r.get("st") == "too_long" and r.get("http") == 0, r)

    # --- auth_error round trip
    code, r, err = T.set_token(FAKE_TOKEN, cmd="tg t_token", board=x.b)
    c.check("api: fake token stored", code == 0, err or r)
    r = x.cmd("tg t_getme")
    c.check("api: fake token -> getMe auth (401/404)",
            r.get("st") == "auth" and r.get("code") in (401, 404), r)
    x.settle(0.5)
    ev = x.tg_events()
    c.check("api: events offline(token changed) then auth_error",
            len(ev) == 2 and ev[0] == ("offline", "token changed") and ev[1][0] == "auth_error",
            ev)
    st = x.cmd("tg t_stat")
    c.check("api: state auth_error, bot cleared", st.get("tg") == "auth_error" and
            st.get("bot") == "", {k: st.get(k) for k in ("tg", "bot", "detail")})
    code, r, err = T.set_token(x.token, cmd="tg t_token", board=x.b)
    r = x.cmd("tg t_getme")
    x.settle(0.5)
    ev = x.tg_events()
    c.check("api: real token again -> offline then online",
            r.get("st") == "ok" and ev == [("offline", "token changed"), ("online", "@" + bot)],
            ev)

    # --- sys log toggles while getMe runs every 300 ms on the send worker: the window
    #     between esp_log_level_set("*") and the runtime re-cap must not leak either
    r = x.cmd("tg t_soak", "40", "0", "600", "-g", "300")
    c.check("api: getMe traffic started for the toggle test", r.get("ok") is True, r)
    levels = ["debug", "info", "debug", "warn", "debug", "none", "debug", "error"]
    toggles = 0
    t_end = time.monotonic() + 35
    while time.monotonic() < t_end:
        r = x.cmd("sys log", levels[toggles % len(levels)], timeout=10)
        toggles += r.get("ok") is True
    r = x.cmd("sys log", "debug")
    x.settle(3.0)
    st = x.cmd("tg t_stat")
    x.cmd("tg t_soak", "0", "0", "600")
    send = st.get("send") or {}
    print("INFO %d sys log toggles during %s getMe calls (%s failed)" %
          (toggles, send.get("ok"), send.get("fail")), flush=True)
    c.check("api: toggles ran during traffic", toggles >= 20 and send.get("ok", 0) >= 50,
            (toggles, send))

    # --- a session idle for over 60 s is reconnected, not reused
    x.settle(3.0)
    warm = x.cmd("tg t_getme")
    x.settle(65.0)
    cold = x.cmd("tg t_getme")
    print("INFO getMe on a warm session %s ms, after 65 s idle %s ms" %
          (warm.get("ms"), cold.get("ms")), flush=True)
    c.check("api: after 65 s idle getMe ok on a fresh session (slower: new handshake)",
            cold.get("st") == "ok" and cold.get("ms", 0) > warm.get("ms", 1e9), (warm, cold))

    # --- nothing of the token in any captured line (the typed t_token lines are echoed
    #     back by the console, as any typed input is; they are excluded, frames are not)
    r = x.cmd("sys log", "warn")
    x.settle(1.0)

    def is_echo(ln):
        return "t_token" in ln and not ln.startswith("@@")

    text = "\n".join(ln for ln in x.lines if not is_echo(ln))
    leaks = [s for s in T.secrets_of(x.token) if s and s in text]
    debug_lines = sum(1 for ln in x.lines if ln.startswith("D ("))
    http_debug = sum(1 for ln in x.lines if re.search(r"^D \(\d+\) (HTTP_CLIENT|transport)", ln))
    print("INFO captured %d console lines, %d at DEBUG, %d DEBUG lines from HTTP tags" %
          (len(x.lines), debug_lines, http_debug))
    c.check("api: DEBUG output was on during the run", debug_lines > 0, debug_lines)
    c.check("api: HTTP client tags stayed capped at WARN under sys log debug", http_debug == 0,
            http_debug)
    c.check("api: no part of the token in the captured output (echoes excluded)", not leaks)
    echoes = sum(1 for ln in x.lines if is_echo(ln))
    print("INFO %d typed t_token echo line(s) excluded" % echoes)
    return chat


def suite_soak(x, seconds, chat):
    c = x.c
    r = x.cmd("tg t_getme")
    c.check("soak: getMe ok before start", r.get("st") == "ok", r)
    info0 = x.cmd("sys info")
    boot0 = info0.get("boot")
    r = x.cmd("tg t_soak", str(seconds), str(chat), "600")
    c.check("soak: started", r.get("ok") is True, r)
    print("INFO soak %d s, sendMessage every 600 s to %s, getMe every 15 s" %
          (seconds, "the owner chat" if chat else "nobody (getMe only)"), flush=True)
    t0 = time.monotonic()
    samples = []
    while True:
        elapsed = time.monotonic() - t0
        info = x.cmd("sys info", timeout=15)
        st = x.cmd("tg t_stat", timeout=15)
        x.events.clear()
        s = {"t": int(elapsed), "heap_free": info.get("heap_free"),
             "heap_min": info.get("heap_min"), "largest": st.get("heap_largest"),
             "poll_ok": (st.get("poll") or {}).get("ok"),
             "poll_fail": (st.get("poll") or {}).get("fail"),
             "send_ok": (st.get("send") or {}).get("ok"),
             "send_fail": (st.get("send") or {}).get("fail"), "sent": st.get("sent"),
             "poll_stack": (st.get("poll") or {}).get("stack_free"),
             "send_stack": (st.get("send") or {}).get("stack_free"), "tg": st.get("tg")}
        samples.append(s)
        print("SAMPLE " + json.dumps(s), flush=True)
        if elapsed >= seconds + 5 or st.get("soak_left_s", 1) == 0 and elapsed > 10:
            break
        for _ in x.b.frames(60):
            pass
    last = x.cmd("tg t_stat", timeout=15)
    info = x.cmd("sys info", timeout=15)
    print("INFO final t_stat %s" % json.dumps({k: last.get(k) for k in (
        "poll", "send", "sent", "heap_free", "heap_min", "heap_largest", "tg")}))
    mins = [s["heap_min"] for s in samples if isinstance(s["heap_min"], int)]
    frees = [s["heap_free"] for s in samples if isinstance(s["heap_free"], int)]
    print("INFO heap_min after soak %s, lowest heap_free sample %s, highest %s" %
          (info.get("heap_min"), min(frees) if frees else None, max(frees) if frees else None))
    c.check("soak: no reboot", info.get("boot") == boot0, (boot0, info.get("boot")))
    print("INFO sendMessage during soak: %s sent" % last.get("sent"))
    if chat:
        check_send(x, chat, "soak")
    poll, send = last.get("poll") or {}, last.get("send") or {}
    c.check("soak: polls ok, no failures", poll.get("ok", 0) > 0 and poll.get("fail", 1) == 0,
            poll)
    c.check("soak: sends ok, no failures", send.get("ok", 0) > 0 and send.get("fail", 1) == 0,
            send)
    c.check("soak: min free heap stays >= 60 KB", (info.get("heap_min") or 0) >= 60 * 1024,
            info.get("heap_min"))
    if len(frees) >= 4:
        early = sum(frees[1:3]) / 2
        late = sum(frees[-2:]) / 2
        print("INFO heap_free early %.0f, late %.0f (drift %+.0f B)" % (early, late, late - early))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-p", "--port")
    ap.add_argument("--flash", metavar="BUILD_DIR")
    ap.add_argument("--seconds", type=int, default=3600)
    ap.add_argument("--chat", type=int, default=None,
                    help="owner chat id for sendMessage (default: TG_OWNER_CHAT in .env.local; "
                         "none = nothing is sent)")
    ap.add_argument("suite", choices=["api", "soak"])
    a = ap.parse_args()
    try:
        env = W.load_env(os.path.join(W.ROOT, ".env.local"))
    except OSError:
        print("cannot read .env.local", file=sys.stderr)
        return 2
    token = env.get("TG_TOKEN", "")
    if not token:
        print("TG_TOKEN missing in .env.local", file=sys.stderr)
        return 2
    chat = a.chat
    if chat is None:
        try:
            chat = int(env.get("TG_OWNER_CHAT") or 0)
        except ValueError:
            print("TG_OWNER_CHAT in .env.local is not a number", file=sys.stderr)
            return 2
    port = a.port or find_port()
    if a.flash:
        flash(port, a.flash)
    checks = Checks(T.secrets_of(token))
    if not checks.check("setup: wifi up", wifi_up(port)):
        return 1
    with Board(port) as b:
        b.wait_ready(20)
        x = Ctx(b, token, checks)
        print("== %s" % a.suite, flush=True)
        if a.suite == "api":
            suite_api(x, chat)
        else:
            T.set_token(token, cmd="tg t_token", board=b)
            suite_soak(x, a.seconds, chat)
    failed = [r["check"] for r in checks.results if not r["ok"]]
    print(json.dumps({"ok": checks.ok(), "passed": len(checks.results) - len(failed),
                      "failed": failed}))
    return 0 if checks.ok() else 1


if __name__ == "__main__":
    sys.exit(main())
