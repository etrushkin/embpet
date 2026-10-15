#!/usr/bin/env python3
"""Device tests for token, pairing, owner and the online notice (embpet-z5d). Needs the
bridge TEST build (components/tgbot/bridgetest.defaults), Wi-Fi and TG_TOKEN in .env.local.
Run it under the board lock so flashing and testing are one locked unit:

  scripts/hw.sh python tools/pair_test.py [-p PORT] [--flash BUILD_DIR]

No Telegram user is involved and nothing is sent to any chat: pairing is driven with injected
updates from made-up chat ids, and in the test build the online notice and the pairing
confirmation are recorded on the board instead of sent (`tg t_notice`). The only network
traffic is getMe/deleteWebhook with the real token (and one getMe with a made-up token). The
human step is tools/tg_pair.py.

Checks: tg owner (show/set/negative/clear/bad args), tg token (checked with Telegram: a
made-up token is refused and not stored, the real one is stored, res shows the bot and the
masked token), pairing (code format, injected /start binds the owner and emits `paired`,
the confirmation is queued once, wrong code / expired code / non-owner chat afterwards /
no pairing running are ignored, lockout after 5 wrong codes, re-pairing, token change voids
a code), persistence across `sys reboot`, exactly one online notice per boot over two
reboots (not again after a Wi-Fi bounce, none without an owner), and that the token's
secret part never appears in console output under `sys log debug`.

Exit 0 = no check failed, 1 = a check failed, 2 = setup error. Output is check names with
PASS/FAIL and INFO lines; the token, Wi-Fi password and pairing codes are never printed.
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

C1, C2, C3 = 777000111, 777000222, -1001777000333     # made-up chats: nothing is ever sent
FAKE_TOKEN = "123456789:AAFakeFakeFakeFakeFakeFakeFakeFake1"
CONFIRM = "Paired. This chat is now the embpet owner."


def start(chat, code, msg=1):
    return B.update(msg, chat, "/start %s" % code)


class X(B.Ctx):
    lines = []

    def open(self):
        super().open()
        self.b.on_line = self.lines.append

    def inject(self, u):
        return self.cmd("tg t_inject", text=[json.dumps(u)])

    def owner(self):
        return self.cmd("tg owner").get("chat_id")

    def notice(self):
        return self.cmd("tg t_notice")

    def pair(self):
        r = self.cmd("tg pair")
        return r.get("code"), r

    def paired(self):
        self.pump(0.3)
        return self.take("paired")

    def reboot(self):
        r = self.cmd("sys reboot")
        self.close()
        time.sleep(4.0)
        self.open()
        return r.get("ok") is True


def step_owner(x):
    c = x.c
    r = x.cmd("tg owner", "--clear")
    c.check("owner: --clear -> chat_id null", r.get("ok") is True and r.get("chat_id") is None, r)
    r = x.cmd("tg owner", text=[str(C1)])
    c.check("owner: set", r.get("chat_id") == C1, r)
    c.check("owner: show", x.cmd("tg owner").get("chat_id") == C1)
    c.check("owner: tg status agrees", x.cmd("tg status").get("owner") == C1)
    r = x.cmd("tg owner", text=[str(C3)])
    c.check("owner: negative (group) id after --", r.get("chat_id") == C3, r)
    for bad in ("abc", "0", "1.5", "12345678901234567"):
        r = x.cmd("tg owner", text=[bad])
        c.check("owner: %r -> bad_args" % bad, r.get("err") == "bad_args", r)
    r = x.cmd("tg owner", "--clear", text=[str(C1)])
    c.check("owner: id and --clear together -> bad_args", r.get("err") == "bad_args", r)
    c.check("owner: unchanged by refused commands", x.owner() == C3)
    r = x.cmd("tg owner", "--clear")
    c.check("owner: cleared again", r.get("chat_id") is None and x.owner() is None, r)


def step_token(x):
    c = x.c
    st0 = x.cmd("tg status")
    r = x.cmd("tg token", text=["not-a-token"])
    c.check("token: malformed -> bad_args", r.get("err") == "bad_args", r)
    r = x.cmd("tg token", text=[FAKE_TOKEN])
    c.check("token: made-up token -> auth_error, not stored", r.get("err") == "auth_error", r)
    c.check("token: status keeps the old token and state",
            x.cmd("tg status").get("token") == st0.get("token") == T.masked(x.token))
    code, r, err = T.set_token(x.token, board=x.b)
    c.check("token: real token accepted", code == 0 and r.get("bot"), r)
    c.check("token: res has the masked token only", r.get("token") == T.masked(x.token), r)
    c.check("token: tg online again", x.wait_online(60))
    st = x.cmd("tg status")
    c.check("token: status bot equals the res bot", st.get("bot") == r.get("bot"))


def step_pairing(x):
    c = x.c
    n0 = x.notice()
    c.check("pair: test build is in capture mode (nothing leaves the board)",
            n0.get("live") is False, n0)
    x.cmd("tg owner", "--clear")
    code, r = x.pair()
    c.check("pair: code is 6 digits, valid 300 s",
            isinstance(code, str) and len(code) == 6 and code.isdigit() and
            r.get("expires_s") == 300, {k: v for k, v in r.items() if k != "code"})
    left = x.notice().get("pair_left_s", 0)
    c.check("pair: running, about 300 s left", 295 <= left <= 300, left)
    code2, r2 = x.pair()
    c.check("pair: a second tg pair replaces the running one", r2.get("ok") is True and
            isinstance(code2, str) and len(code2) == 6, None)
    code = code2

    wrong = "%06d" % ((int(code) + 1) % 1000000)
    x.take()
    x.inject(start(C1, wrong, 10))
    c.check("pair: wrong code -> no paired event, no owner",
            x.paired() == [] and x.owner() is None)
    x.inject(B.update(11, C1, "hello"))
    x.inject(start(C1, "abc", 12))
    c.check("pair: other texts do not count as attempts",
            x.paired() == [] and x.owner() is None)
    x.inject(B.update(18, C1, "/start@some_other_bot %s" % code))
    c.check("pair: another bot's /start@name is no attempt", x.paired() == [] and
            x.owner() is None)
    bot = x.cmd("tg status").get("bot")
    x.inject(B.update(13, C1, "/start@%s %s" % (bot, code)))
    ev = x.paired()
    c.check("pair: right code -> one paired event with the chat", len(ev) == 1 and
            ev[0].get("chat_id") == C1, ev)
    c.check("pair: owner stored, tg owner shows it", x.owner() == C1)
    n = x.notice()
    c.check("pair: confirmation queued exactly once, plain text to the new owner",
            n.get("confirms") == n0.get("confirms", 0) + 1 and n.get("confirm_text") == CONFIRM, n)
    c.check("pair: pairing ended", n.get("pair_left_s") == 0, n)

    x.inject(start(C1, code, 14))
    x.inject(start(C2, code, 15))
    x.inject(B.update(16, C2, "let me in"))
    x.inject(B.update(17, C2, "x", reply_to=999, quoted="[A] q"))
    c.check("pair: reused code, non-owner chat: ignored (no event, owner kept, no hint)",
            x.take() == [] and x.owner() == C1 and
            x.notice().get("confirms") == n.get("confirms"))

    # expiry
    x.cmd("tg t_pairttl", "2")
    code, r = x.pair()
    c.check("pair: test ttl 2 s reported", r.get("expires_s") == 2, r)
    time.sleep(3.2)
    x.take()
    x.inject(start(C2, code, 20))
    c.check("pair: expired code ignored", x.paired() == [] and x.owner() == C1)
    x.cmd("tg t_pairttl", "300")

    # lockout
    code, _ = x.pair()
    bad = "%06d" % ((int(code) + 7) % 1000000)
    for k in range(5):
        x.inject(start(C2, bad, 30 + k))
    c.check("pair: 5 wrong codes cancel pairing", x.notice().get("pair_left_s") == 0)
    x.inject(start(C2, code, 40))
    c.check("pair: right code after the lockout ignored", x.paired() == [] and x.owner() == C1)

    # re-pair to another chat
    code, _ = x.pair()
    x.inject(start(C3, code, 50))
    ev = x.paired()
    c.check("pair: re-pairing binds the new chat", len(ev) == 1 and ev[0].get("chat_id") == C3 and
            x.owner() == C3, ev)

    # a token change voids the code
    code, _ = x.pair()
    T.set_token(x.token, board=x.b)
    c.check("pair: setting the token voids a running pairing",
            x.notice().get("pair_left_s") == 0)
    x.inject(start(C2, code, 60))
    c.check("pair: its code no longer works", x.paired() == [] and x.owner() == C3)
    c.check("pair: tg online after the token was set again", x.wait_online(60))
    x.cmd("tg owner", "--clear")


def step_persist_and_notice(x):
    c = x.c
    x.cmd("tg owner", text=[str(C1)])
    masked = x.cmd("tg status").get("token")
    for boot in (1, 2):
        c.check("notice: reboot %d" % boot, x.reboot())
        info = x.cmd("sys info")
        c.check("notice: online after reboot %d" % boot, x.wait_online(90))
        x.pump(8.0)
        st = x.cmd("tg status")
        c.check("persist: token and owner survived reboot %d" % boot,
                st.get("token") == masked == T.masked(x.token) and st.get("owner") == C1)
        n = x.notice()
        want = "embpet online · fw %s · reset %s" % (info.get("fw"),
                                                             info.get("reset_reason"))
        print("INFO boot %d: reset_reason %s, notices %s" % (boot, info.get("reset_reason"),
                                                            n.get("notices")), flush=True)
        c.check("notice: boot %d: exactly one, text 'embpet online · fw <ver> · reset <reason>'"
                % boot, n.get("notices") == 1 and n.get("notice_text") == want and
                n.get("live") is False, n)
    # a Wi-Fi reconnect is not a new boot
    x.cmd("wifi set", text=[B.NOSUCH_SSID, "embpet-wrong-pass"])
    c.check("notice: wifi down", x.wait_wifi("down", 20))
    x.cmd("wifi set", text=[x.ssid] + ([x.password] if x.password else []))
    c.check("notice: wifi up again", x.wait_wifi("up", 60))
    c.check("notice: tg online again", x.wait_online(60))
    x.pump(5.0)
    c.check("notice: still one after the reconnect", x.notice().get("notices") == 1)
    # a boot without an owner sends none
    x.cmd("tg owner", "--clear")
    c.check("notice: reboot without owner", x.reboot())
    c.check("notice: online", x.wait_online(90))
    x.pump(8.0)
    n = x.notice()
    c.check("notice: no owner -> no notice this boot", n.get("notices") == 0, n)
    x.cmd("tg owner", text=[str(C1)])
    x.pump(3.0)
    c.check("notice: owner set after the first online moment -> still none this boot",
            x.notice().get("notices") == 0)
    x.cmd("tg owner", "--clear")


def step_leak(x):
    c = x.c
    x.cmd("tg owner", "--clear")
    x.cmd("sys log", "debug")
    x.pump(0.5)
    mark = len(x.lines)
    code, r, err = T.set_token(x.token, board=x.b)
    c.check("leak: token set under sys log debug", code == 0, r)
    x.pump(6.0)
    x.cmd("sys log", "warn")
    x.pump(0.5)
    new = x.lines[mark:]
    text = "\n".join(ln for ln in new if "tg token" not in ln or ln.startswith("@@"))
    leaks = [s for s in T.secrets_of(x.token) if s and s in text]
    dbg = sum(1 for ln in new if ln.startswith("D ("))
    print("INFO captured %d lines, %d at DEBUG" % (len(new), dbg), flush=True)
    c.check("leak: DEBUG output was on", dbg > 0, dbg)
    c.check("leak: no part of the token in the captured output (typed echo excluded)",
            not leaks)
    st = x.cmd("tg status")
    c.check("leak: token masked in tg status", st.get("token") == T.masked(x.token))


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
    token, ssid = env.get("TG_TOKEN", ""), env.get("WIFI_SSID", "")
    if not token or not ssid or "WIFI_PASS" not in env:
        print("TG_TOKEN / WIFI_SSID / WIFI_PASS missing in .env.local", file=sys.stderr)
        return 2
    port = a.port or find_port()
    if a.flash:
        flash(port, a.flash)
    checks = Checks(T.secrets_of(token) + [env["WIFI_PASS"]])
    x = X(port, token, ssid, env["WIFI_PASS"], 0, checks)
    x.open()
    try:
        if x.wifi_state() != "up":
            x.cmd("wifi set", text=[ssid, env["WIFI_PASS"]] if env["WIFI_PASS"] else [ssid])
            if not checks.check("setup: wifi up", x.wait_wifi("up", 60)):
                return 1
        code, r, err = T.set_token(token, board=x.b)
        if not checks.check("setup: tg token stored", code == 0, r or err):
            return 1
        checks.check("setup: tg online", x.wait_online(90))
        for step in (step_owner, step_token, step_pairing, step_persist_and_notice, step_leak):
            print("== %s" % step.__name__, flush=True)
            try:
                step(x)
            finally:
                try:
                    x.cmd("tg t_pairttl", "300")
                    x.cmd("tg owner", "--clear")
                except Exception:
                    pass
    finally:
        x.close()
    failed = [r["check"] for r in checks.results if not r["ok"]]
    print(json.dumps({"ok": checks.ok(), "passed": len(checks.results) - len(failed),
                      "failed": failed}))
    return 0 if checks.ok() else 1


if __name__ == "__main__":
    sys.exit(main())
