#!/usr/bin/env python3
"""Device tests for the Telegram bridge (embpet-64s). Needs the bridge TEST build
(CONFIG_EMBPET_TG_TEST with the bridge, components/tgbot/bridgetest.defaults), Wi-Fi and
TG_TOKEN in .env.local. Run it under the board lock so flashing and testing are one locked
unit:

  scripts/hw.sh python tools/bridge_test.py [-p PORT] [--flash BUILD_DIR] [--chat ID]
                                            [--wait S] SUITE

Suites:
  auto    no Telegram user and no message sent: tg send argument checks, too_long,
          not_configured and offline (Wi-Fi down) answered fast (host-measured round trip
          against a `sys info` baseline), tg status, tg pending / tg ack and the `pending`
          events, table overflow with pending_dropped, routing of injected updates (table,
          prefix, hint, foreign chat, ordering of reply and pending events, UTF-8 text) with
          the token cleared so no hint leaves the board, getUpdates failing while getMe
          works (test fault: bounded backoff, no tg event flapping, one getMe probe per
          Wi-Fi reconnect), and the stack headroom of the console, tg_poll and tg_send
          tasks. Steps that change the token, owner, Wi-Fi or fault restore them in a
          finally block.
  human   README 3.5 with the owner: sends [A] and [B] to the owner chat, asks the owner to
          Reply in Telegram, checks the reply events (actor, to, src table), the pending
          events, the hint for a message without Reply, then reboots the board and checks
          that a Reply to the old [A] message routes via src "prefix". Each step waits up to
          --wait seconds (default 180) for the owner; a step nobody answered is reported as
          PENDING, not as a failure.
  all     auto, then human.

Messages go ONLY to the explicitly configured owner chat: --chat, else TG_OWNER_CHAT in the
git-ignored .env.local. Without one, `human` is skipped (PENDING); the tool never picks a
chat from incoming updates. Output is check names with PASS/FAIL/PENDING and INFO lines with
message ids, timings and stack numbers. The token, the Wi-Fi password, chat ids and the
owner's message texts are never printed. Exit 0 = no check failed (PENDING allowed), 1 = a
check failed, 2 = setup error.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from embpet import Board, CmdTimeout, find_port  # noqa: E402
import tg_from_env as T  # noqa: E402
import wifi_from_env as W  # noqa: E402
from wifi_test import Checks, flash  # noqa: E402

FAKE_OWNER = 777000111           # owner id used only while no token is stored / Wi-Fi down
NOSUCH_SSID = "embpet-no-such-network"
FAST_MS = 100
BOT_ID = 0                       # the bot's user id: the token's digits before ':' (main)


class Ctx:
    def __init__(self, port, token, ssid, password, chat, checks):
        self.port = port
        self.token = token
        self.ssid, self.password = ssid, password
        self.chat = chat
        self.c = checks
        self.pending = []
        self.events = []
        self.b = None

    def open(self):
        self.b = Board(self.port)
        self.b.on_event = self.events.append
        self.b.wait_ready(20)

    def close(self):
        if self.b is not None:
            self.b.close()
            self.b = None

    def cmd(self, command, *args, text=(), actor=None, timeout=20.0):
        try:
            return self.b.cmd(command, *args, text=text, actor=actor, timeout=timeout)
        except CmdTimeout:
            return {}

    def timed(self, command, *args, text=(), actor=None):
        """cmd with a fine-grained serial read timeout; returns (res, round trip in ms)."""
        s = self.b._s                   # Board reads with a 0.2 s timeout, too coarse here
        old = s.timeout
        s.timeout = 0.001
        try:
            t0 = time.monotonic()
            r = self.cmd(command, *args, text=text, actor=actor)
            return r, (time.monotonic() - t0) * 1000.0
        finally:
            s.timeout = old

    def pump(self, seconds):
        """Read the console for SECONDS so trailing events are recorded."""
        for _ in self.b.events(seconds):
            pass

    def take(self, ev=None):
        """Recorded events (of type EV), removed from the record."""
        out = [e for e in self.events if ev is None or e.get("ev") == ev]
        # in place: the Board's on_event appends to this very list
        self.events[:] = [e for e in self.events if not (ev is None or e.get("ev") == ev)]
        return out

    def pend(self, name, detail=None):
        self.pending.append(name)
        print("PENDING %s%s" % (name, "" if detail is None else "  [%s]" % detail), flush=True)

    def tg_state(self):
        return self.cmd("tg status").get("state")

    def wait_online(self, seconds=60):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if self.tg_state() == "online":
                return True
            self.pump(1.0)
        return False

    def wifi_state(self):
        return self.cmd("wifi status").get("state")

    def wait_wifi(self, want, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if self.wifi_state() == want:
                return True
            self.pump(1.0)
        return False

    def set_token(self):
        code, _, err = T.set_token(self.token, cmd="tg t_token", board=self.b)
        return code == 0

    def restore(self):
        """Back to the normal setup after a suite that changes it, also after an error: no
        getUpdates fault, owner 0, the Wi-Fi credentials and the token from .env.local.
        Wi-Fi and token are only rewritten when they differ, so nothing reconnects for
        nothing. If the open connection fails, the helpers' own connections are used."""
        try:
            self.cmd("tg t_fault", "0")
            self.cmd("tg t_owner", "0")
            if self.cmd("wifi status").get("ssid") != self.ssid:
                self.cmd("wifi set", text=[self.ssid] + ([self.password] if self.password
                                                         else []))
            st = self.cmd("tg status")
            if st.get("token") != T.masked(self.token):
                self.set_token()
        except Exception as e:          # serial trouble: reopen through the helper path
            print("INFO restore via the helpers after %s" % type(e).__name__, flush=True)
            self.close()
            W.wifi_set(self.ssid, self.password, self.port)
            T.set_token(self.token, self.port, cmd="tg t_token")
            self.open()
            self.cmd("tg t_fault", "0")
            self.cmd("tg t_owner", "0")


def redact(obj):
    """An event or res without message text (the owner's words are never printed)."""
    if isinstance(obj, dict):
        return {k: ("<%d chars>" % len(v) if k == "text" and isinstance(v, str) else v)
                for k, v in obj.items() if k != "chat"}
    if isinstance(obj, list):
        return [redact(v) for v in obj]
    return obj


def counts(events):
    return [e.get("count") for e in events if e.get("ev") == "pending"]


def update(msg, chat, text=None, reply_to=None, quoted=None, quoted_from=None, date=1760450100):
    """A getUpdates update object, as Telegram sends it. The replied-to message is written by
    user quoted_from (default: the bot, whose id the firmware takes from the token)."""
    author = quoted_from if quoted_from is not None else BOT_ID
    m = {"message_id": msg, "date": date, "chat": {"id": chat, "type": "private"},
         "from": {"id": chat, "is_bot": False, "first_name": "owner"}}
    if text is not None:
        m["text"] = text
    if reply_to is not None:
        rt = {"message_id": reply_to, "date": date - 60,
              "chat": {"id": chat, "type": "private"},
              "from": {"id": author, "is_bot": author != chat, "first_name": "x"}}
        if quoted is not None:
            rt["text"] = quoted
        m["reply_to_message"] = rt
    return {"update_id": 500000000 + msg, "message": m}


# ------------------------------------------------------------------ auto

def auto_args(x):
    c = x.c
    r = x.cmd("tg send", text=["no actor"])
    c.check("args: tg send without -a -> bad_args", r.get("err") == "bad_args", r)
    for bad in ("abc", "0", "-5", "9007199254740992"):
        r = x.cmd("tg send", "-r", bad, text=["x"], actor="A")
        c.check("args: tg send -r %s -> bad_args" % bad, r.get("err") == "bad_args", r)
    r = x.cmd("tg send", text=["x"], actor="bad.actor")
    c.check("args: invalid -a -> bad_args", r.get("err") == "bad_args", r)

    # text checks run before the settings and the state, so no message can leave here
    r, ms = x.timed("tg send", text=["x" * 4097], actor="A")
    c.check("too_long: 4097 bytes", r.get("err") == "too_long", redact(r))
    print("INFO too_long (4097 bytes) answered in %.1f ms" % ms)
    r = x.cmd("tg send", text=["x" * 4093], actor="A")
    c.check("too_long: [A] + 4093 ASCII = 4097 UTF-16 units", r.get("err") == "too_long",
            redact(r))
    r = x.cmd("tg send", text=["x" * 4078], actor="abcdefghijklmnop")
    c.check("too_long: 16-char actor + 4078 ASCII", r.get("err") == "too_long", redact(r))
    r = x.cmd("tg send", text=["\U0001F600" * 1025], actor="A")
    c.check("too_long: 1025 emoji = 4100 bytes", r.get("err") == "too_long", redact(r))


def auto_not_configured(x):
    c = x.c
    x.cmd("tg t_owner", "0")
    st = x.cmd("tg status")
    c.check("status: no owner -> owner null", "owner" in st and st["owner"] is None,
            redact(st))
    base = [x.timed("sys info")[1] for _ in range(5)]
    times = []
    for text in ("x" * 4092, "short"):
        r, ms = x.timed("tg send", text=[text], actor="A")
        times.append(ms)
        c.check("not_configured: no owner (%d bytes)" % len(text),
                r.get("err") == "not_configured" and "owner" in (r.get("msg") or ""), redact(r))
    print("INFO not_configured round trip %.1f / %.1f ms (sys info baseline %.1f..%.1f ms)" %
          (times[0], times[1], min(base), max(base)), flush=True)
    c.check("not_configured: answered within %d ms" % FAST_MS, max(times) < FAST_MS, times)


def auto_status(x):
    c = x.c
    st = x.cmd("tg status")
    c.check("status: fields", all(k in st for k in ("state", "bot", "owner", "token", "pending")),
            redact(st))
    c.check("status: token masked as cfg does", st.get("token") == T.masked(x.token))
    c.check("status: online with a bot name", st.get("state") == "online" and st.get("bot"),
            redact(st))
    c.check("status: pending is a count", isinstance(st.get("pending"), int), redact(st))


def auto_pending(x):
    c = x.c
    r = x.cmd("tg ack", "--older", "0")
    x.pump(0.3)
    x.take()
    for msg, actor in ((900001, "A"), (900002, "B"), (900003, "A")):
        x.cmd("tg t_pend", str(msg), actor=actor)
    x.pump(0.3)
    c.check("pending: events count 1, 2, 3", counts(x.take("pending")) == [1, 2, 3])
    r = x.cmd("tg pending")
    items = r.get("items") or []
    c.check("pending: list oldest first",
            [(i.get("msg"), i.get("actor")) for i in items] ==
            [(900001, "A"), (900002, "B"), (900003, "A")], r)
    c.check("pending: age_s present", all(isinstance(i.get("age_s"), int) for i in items), r)
    r = x.cmd("tg pending", actor="A")
    c.check("pending: -a A filters", [i.get("msg") for i in r.get("items") or []] ==
            [900001, 900003] and r.get("actor") == "A", r)
    r = x.cmd("tg ack", "-m", "900002")
    x.pump(0.3)
    c.check("ack -m: acked 1, pending event 2",
            r.get("acked") == 1 and counts(x.take("pending")) == [2], r)
    r = x.cmd("tg ack", "-m", "900002")
    x.pump(0.3)
    c.check("ack -m again: acked 0, no event", r.get("acked") == 0 and x.take("pending") == [],
            r)
    r = x.cmd("tg ack", "--older", "3600")
    c.check("ack --older 3600: nothing that old", r.get("acked") == 0, r)
    for args in ((), ("--older", "1", "-m", "900001"), ("--older", "x"), ("-m", "0")):
        r = x.cmd("tg ack", *args)
        c.check("ack %s -> bad_args" % " ".join(args), r.get("err") == "bad_args", r)
    r = x.cmd("tg ack", "--older", "0")
    x.pump(0.3)
    c.check("ack --older 0: acked 2, pending event 0",
            r.get("acked") == 2 and counts(x.take("pending")) == [0], r)

    # overflow: 64 pending entries, then one more drops the oldest pending
    for k in range(64):
        x.cmd("tg t_pend", str(910000 + k), actor="ov")
    x.pump(0.3)
    cs = counts(x.take("pending"))
    c.check("overflow: 64 entries pending (answered ones evicted first)",
            cs and cs[-1] == 64 and x.take("pending_dropped") == [], cs[-3:])
    x.cmd("tg t_pend", "910064", actor="ov")
    x.pump(0.3)
    drops = x.take("pending_dropped")
    c.check("overflow: pending_dropped {actor ov, msg 910000}, count unchanged",
            [(d.get("actor"), d.get("msg")) for d in drops] == [("ov", 910000)] and
            x.take("pending") == [], drops)
    r = x.cmd("tg pending")
    items = r.get("items") or []
    c.check("overflow: list has 64, oldest is 910001",
            len(items) == 64 and items[0].get("msg") == 910001, len(items))
    r = x.cmd("tg ack", "--older", "0")
    x.pump(0.3)
    c.check("overflow: ack all -> acked 64, pending 0",
            r.get("acked") == 64 and counts(x.take("pending")) == [0], r)


def auto_routing(x):
    """Injected updates with the token cleared: nothing can be sent (hints are dropped)."""
    c = x.c
    owner = x.chat or FAKE_OWNER
    r = x.cmd("tg t_untoken")
    x.pump(0.5)
    c.check("route: token cleared (no message can leave the board)",
            r.get("ok") is True and x.tg_state() == "unconfigured", r)
    x.cmd("tg t_owner", str(owner))
    x.take()
    h0 = x.cmd("tg t_bridge")

    def inject(u):
        return x.cmd("tg t_inject", text=[json.dumps(u, ensure_ascii=False)])

    x.cmd("tg t_pend", "920001", actor="A")
    x.pump(0.3)
    x.take()
    r = inject(update(1001, owner, "auto table", reply_to=920001, quoted="[A] question"))
    x.pump(0.3)
    ev = x.take()
    kinds = [(e.get("ev"), e.get("count")) for e in ev]
    rep = [e for e in ev if e.get("ev") == "reply"]
    c.check("route: table reply event", r.get("handled") is True and len(rep) == 1 and
            {k: rep[0].get(k) for k in ("actor", "to", "msg", "text", "date", "src")} ==
            {"actor": "A", "to": 920001, "msg": 1001, "text": "auto table",
             "date": 1760450100, "src": "table"}, redact(ev))
    c.check("route: reply event, then pending 0", kinds == [("reply", None), ("pending", 0)],
            kinds)
    seqs = [e.get("seq") for e in ev]
    c.check("route: reply seq < pending seq", seqs == sorted(seqs), seqs)

    inject(update(1002, owner, "again", reply_to=920001))
    x.pump(0.3)
    ev = x.take()
    c.check("route: second reply to an answered message routes, no pending event",
            [(e.get("ev"), e.get("src")) for e in ev] == [("reply", "table")], redact(ev))

    inject(update(1003, owner, "after reboot", reply_to=920099, quoted="[B] old question"))
    x.pump(0.3)
    ev = x.take()
    c.check("route: unknown msg with [B] prefix -> src prefix, actor B",
            [(e.get("ev"), e.get("actor"), e.get("to"), e.get("src")) for e in ev] ==
            [("reply", "B", 920099, "prefix")], redact(ev))

    text = "Привет \"q\" \\ x\nline 2 \U0001F600 中"
    inject(update(1004, owner, text, reply_to=920001))
    x.pump(0.3)
    ev = x.take("reply")
    c.check("route: UTF-8, quotes, backslash, newline survive in the event text",
            len(ev) == 1 and ev[0].get("text") == text, redact(ev))

    for name, u in (
            ("no Reply", update(1005, owner, "hello")),
            ("Reply to the owner's own [A] message", update(1006, owner, "x", reply_to=1005,
                                                            quoted="[A] mine",
                                                            quoted_from=owner)),
            ("Reply to another bot's [A] message", update(1010, owner, "x", reply_to=999,
                                                          quoted="[A] other bot",
                                                          quoted_from=BOT_ID + 1)),
            ("Reply to an untagged bot message", update(1007, owner, "x", reply_to=1000,
                                                        quoted="embpet online"))):
        inject(u)
        x.pump(0.3)
        ev = x.take()
        c.check("route: %s -> no event (hint)" % name, ev == [], redact(ev))
    h1 = x.cmd("tg t_bridge")
    c.check("route: 4 hints requested, none sent (no token; rate limit)",
            h1.get("hints_dropped", 0) - h0.get("hints_dropped", 0) == 4 and
            h1.get("hints_sent") == h0.get("hints_sent"), (h0, h1))

    inject(update(1008, owner + 1, "x", reply_to=920001))
    inject(update(1009, owner + 1, "x"))
    x.pump(0.3)
    h2 = x.cmd("tg t_bridge")
    c.check("route: another chat -> ignored, no event, no hint",
            x.take() == [] and h2.get("hints_dropped") == h1.get("hints_dropped"))
    r = x.cmd("tg t_inject", text=["not json"])
    c.check("route: t_inject rejects non-JSON", r.get("err") == "bad_args", r)

    # the fake owner goes before the token comes back, so nothing is ever sent to it
    x.cmd("tg t_owner", str(x.chat or 0))
    c.check("route: token stored again", x.set_token(), "set_token")
    c.check("route: tg online again", x.wait_online(60))


def auto_offline(x):
    c = x.c
    r = x.cmd("wifi set", text=[NOSUCH_SSID, "embpet-wrong-pass"])
    c.check("offline: wrong network stored", r.get("ok") is True, r)
    down = x.wait_wifi("down", 20)
    c.check("offline: wifi down", down)
    if down:
        # only now an owner is needed for the check order; Wi-Fi is down, nothing can be sent
        x.cmd("tg t_owner", str(x.chat or FAKE_OWNER))
        x.pump(0.5)
        times = []
        for _ in range(5):
            r, ms = x.timed("tg send", text=["offline check"], actor="A")
            times.append(ms)
            c.check("offline: tg send -> offline", r.get("err") == "offline", r)
        print("INFO offline round trips: %s ms" % ", ".join("%.1f" % t for t in times),
              flush=True)
        c.check("offline: answered within %d ms" % FAST_MS, max(times) < FAST_MS, times)
        st = x.cmd("tg status")
        c.check("offline: tg status not online", st.get("state") == "offline", redact(st))
        x.cmd("tg t_owner", str(x.chat or 0))
    r = x.cmd("wifi set", text=[x.ssid, x.password] if x.password else [x.ssid])
    up = x.wait_wifi("up", 60)
    c.check("offline: wifi restored", r.get("ok") is True and up)
    t0 = time.monotonic()
    online = x.wait_online(90)
    print("INFO tg online %.1f s after wifi up was seen" % (time.monotonic() - t0), flush=True)
    c.check("offline: tg online again within 15 s (getMe probe, not the next long poll)",
            online and time.monotonic() - t0 < 15)


def auto_fault(x):
    """getUpdates fails while getMe works (test fault): bounded backoff, no getMe probe, no
    tg event flapping; across a Wi-Fi reconnect exactly one probe and one online/offline
    pair."""
    c = x.c
    x.take()
    x.cmd("tg t_fault", "1")
    # the long poll in flight (up to 50 s) ends first; the next getUpdates fails
    deadline = time.monotonic() + 75
    while time.monotonic() < deadline and not any(
            e.get("ev") == "tg" and e.get("detail") == "test fault" for e in x.events):
        x.pump(1.0)
    ev = x.take("tg")
    c.check("fault: getUpdates fault -> one tg offline event",
            [(e.get("state"), e.get("detail")) for e in ev] == [("offline", "test fault")], ev)
    a = x.cmd("tg t_bridge")
    x.pump(40.0)
    b = x.cmd("tg t_bridge")
    ev = x.take("tg")
    polls = b.get("polls", 0) - a.get("polls", 0)
    probes = b.get("probes", 0) - a.get("probes", 0)
    print("INFO fault, 40 s: %d getUpdates attempts, %d getMe probes, %d tg events" %
          (polls, probes, len(ev)), flush=True)
    c.check("fault: getUpdates backs off (1..6 attempts in 40 s)", 1 <= polls <= 6, polls)
    c.check("fault: no getMe probe, no tg events (no flapping)", probes == 0 and ev == [],
            (probes, ev))
    st = x.cmd("tg status")
    c.check("fault: tg stays offline", st.get("state") == "offline", redact(st))

    # a Wi-Fi reconnect during the fault arms exactly one probe
    x.cmd("wifi set", text=[NOSUCH_SSID, "embpet-wrong-pass"])
    c.check("fault: wifi down", x.wait_wifi("down", 20))
    x.pump(1.0)
    x.take()
    a = x.cmd("tg t_bridge")
    x.cmd("wifi set", text=[x.ssid] + ([x.password] if x.password else []))
    up = x.wait_wifi("up", 60)
    c.check("fault: wifi up again", up)
    x.pump(40.0)
    b = x.cmd("tg t_bridge")
    states = [e.get("state") for e in x.take("tg")]
    polls = b.get("polls", 0) - a.get("polls", 0)
    probes = b.get("probes", 0) - a.get("probes", 0)
    print("INFO fault, Wi-Fi back + 40 s: %d getUpdates attempts, %d probes, tg %s" %
          (polls, probes, states), flush=True)
    c.check("fault: reconnect -> exactly one probe, one online/offline pair, bounded polls",
            probes == 1 and states == ["online", "offline"] and polls <= 6,
            (probes, states, polls))

    t0 = time.monotonic()
    x.cmd("tg t_fault", "0")
    online = x.wait_online(30)
    print("INFO tg online %.1f s after the fault ended" % (time.monotonic() - t0), flush=True)
    c.check("fault: cleared -> online within 5 s (timeout-0 getUpdates)",
            online and time.monotonic() - t0 < 5)


def auto_stacks(x):
    c = x.c
    r = x.cmd("tg t_bridge")
    print("INFO stack headroom (bytes free, lifetime minimum): console %s of 6144, "
          "tg_poll %s of 8192, tg_send %s of 8192; heap free %s, min %s" %
          (r.get("stack_console"), r.get("stack_tg_poll"), r.get("stack_tg_send"),
           r.get("heap_free"), r.get("heap_min")), flush=True)
    c.check("stacks: console, tg_poll, tg_send each keep >= 1 KB free",
            all((r.get(k) or 0) >= 1024 for k in ("stack_console", "stack_tg_poll",
                                                    "stack_tg_send")), r)
    return r


def suite_auto(x):
    print("== auto", flush=True)
    c = x.c
    c.check("setup: token stored", x.set_token())
    c.check("setup: tg online", x.wait_online(90))
    auto_args(x)
    auto_not_configured(x)
    auto_status(x)
    auto_pending(x)
    # these change the token, the owner, the Wi-Fi network or inject a fault
    for step in (auto_routing, auto_offline, auto_fault):
        try:
            step(x)
        finally:
            x.restore()
    auto_stacks(x)


# ------------------------------------------------------------------ human

def wait_replies(x, want, seconds, hints_before=None):
    """Collect reply events until every (actor, to, src) in WANT arrived (and, with
    hints_before, one more hint was sent). Returns (found {key: event}, hint seen)."""
    found = {}
    hint = hints_before is None
    deadline = time.monotonic() + seconds
    seen_msgs = set()
    while time.monotonic() < deadline and (len(found) < len(want) or not hint):
        for e in x.b.events(5.0):
            if e.get("ev") != "reply" or e.get("msg") in seen_msgs:
                continue
            seen_msgs.add(e.get("msg"))                 # hosts dedupe on msg
            key = (e.get("actor"), e.get("to"), e.get("src"))
            if key in want and key not in found:
                found[key] = e
                print("INFO reply event actor %s to %s src %s msg %s (%d chars)" %
                      (key[0], key[1], key[2], e.get("msg"), len(e.get("text") or "")),
                      flush=True)
            else:
                print("INFO other reply event %s" % redact(e), flush=True)
        if not hint:
            hint = x.cmd("tg t_bridge").get("hints_sent", 0) > hints_before
    return found, hint


def suite_human(x, wait_s):
    print("== human", flush=True)
    c = x.c
    if not x.chat:
        x.pend("human: no owner chat (--chat or TG_OWNER_CHAT in .env.local); nothing sent")
        return
    x.cmd("tg t_owner", str(x.chat))
    if not c.check("human: tg online", x.wait_online(90)):
        return
    x.cmd("tg ack", "--older", "0")
    x.pump(0.5)
    x.take()

    sent = {}
    for actor, text in (("A", "Build #42 failed on step 'test'. Retry? (embpet bridge test)"),
                        ("B", "Disk at 91% on nas01. Clean /tmp? (embpet bridge test)")):
        r, ms = x.timed("tg send", text=[text], actor=actor)
        ok = r.get("ok") is True and isinstance(r.get("msg"), int)
        c.check("human: tg send -a %s -> msg" % actor, ok, r)
        print("INFO tg send -a %s msg %s in %.0f ms" % (actor, r.get("msg"), ms), flush=True)
        if not ok:
            return
        sent[actor] = r["msg"]
    x.pump(0.5)
    c.check("human: pending events 1, 2", counts(x.take("pending")) == [1, 2])
    r = x.cmd("tg send", "-r", str(sent["A"]), text=["follow-up threaded under [A] "
                                                     "(no need to answer this one)"], actor="A")
    c.check("human: tg send -r threads a reply", r.get("ok") is True, r)
    if r.get("ok"):
        x.cmd("tg ack", "-m", str(r["msg"]))
    st = x.cmd("tg status")
    c.check("human: tg status pending 2", st.get("pending") == 2, redact(st))
    x.pump(0.5)
    x.take()

    hints0 = x.cmd("tg t_bridge").get("hints_sent", 0)
    print("\n>>> OWNER, in Telegram now:\n"
          ">>>   1. use Reply on the [B] message (msg %d) and send any text;\n"
          ">>>   2. use Reply on the FIRST [A] message (msg %d) and send any text;\n"
          ">>>   3. send one message WITHOUT Reply (the bot answers with a hint).\n"
          ">>> Waiting up to %d s.\n" % (sent["B"], sent["A"], wait_s), flush=True)
    want = {("B", sent["B"], "table"), ("A", sent["A"], "table")}
    found, hint = wait_replies(x, want, wait_s, hints0)
    for key in sorted(want):
        name = "human: Reply on [%s] -> reply event actor %s, to %d, src table" % (
            key[0], key[0], key[1])
        if key in found:
            c.check(name, True)
        else:
            x.pend(name, "no reply within %d s" % wait_s)
    if hint:
        c.check("human: message without Reply -> hint sent, no reply event", True)
    else:
        x.pend("human: message without Reply -> hint", "no hint within %d s" % wait_s)
    x.pump(1.0)
    if found:
        st = x.cmd("tg status")
        c.check("human: answered messages left the pending count",
                st.get("pending") == 2 - len(found), redact(st))
    x.take()

    # reboot: the table is gone, the [A] prefix still routes
    r = x.cmd("sys reboot")
    c.check("human: sys reboot", r.get("ok") is True, r)
    x.close()
    time.sleep(4.0)
    x.open()
    if not c.check("human: online after reboot", x.wait_online(90)):
        return
    r = x.cmd("tg pending")
    c.check("human: pending table empty after reboot", r.get("items") == [], r)
    x.take()
    print("\n>>> OWNER: use Reply on the FIRST [A] message (msg %d) once more and send any "
          "text.\n>>> Waiting up to %d s.\n" % (sent["A"], wait_s), flush=True)
    want = {("A", sent["A"], "prefix")}
    found, _ = wait_replies(x, want, wait_s)
    name = "human: after reboot, Reply on [A] -> reply event actor A, src prefix"
    if found:
        c.check(name, True)
    else:
        x.pend(name, "no reply within %d s" % wait_s)
    x.cmd("tg ack", "--older", "0")


# ------------------------------------------------------------------ main

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-p", "--port")
    ap.add_argument("--flash", metavar="BUILD_DIR")
    ap.add_argument("--chat", type=int, default=None,
                    help="owner chat id (default: TG_OWNER_CHAT in .env.local; none = the "
                         "human suite is skipped and nothing is sent)")
    ap.add_argument("--wait", type=int, default=180, metavar="S",
                    help="seconds to wait for the owner per human step")
    ap.add_argument("suite", choices=["auto", "human", "all"])
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
    global BOT_ID
    BOT_ID = int(token.split(":", 1)[0]) if token.split(":", 1)[0].isdigit() else 0
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
    checks = Checks(T.secrets_of(token) + [env["WIFI_PASS"]] + ([str(chat)] if chat else []))
    x = Ctx(port, token, ssid, env["WIFI_PASS"], chat, checks)
    x.open()
    try:
        if x.wifi_state() != "up":
            x.cmd("wifi set", text=[ssid, env["WIFI_PASS"]] if env["WIFI_PASS"] else [ssid])
            if not checks.check("setup: wifi up", x.wait_wifi("up", 60)):
                return 1
        if a.suite in ("auto", "all"):
            suite_auto(x)
        if a.suite in ("human", "all"):
            suite_human(x, a.wait)
    finally:
        x.close()
    failed = [r["check"] for r in checks.results if not r["ok"]]
    print(json.dumps({"ok": checks.ok(), "passed": len(checks.results) - len(failed),
                      "failed": failed, "pending": x.pending}))
    return 0 if checks.ok() else 1


if __name__ == "__main__":
    sys.exit(main())
