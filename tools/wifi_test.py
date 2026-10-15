#!/usr/bin/env python3
"""Device tests for the Wi-Fi station (embpet-y90). Needs the board and .env.local; run it
under the board lock so flashing and testing are one locked unit:

  scripts/hw.sh python tools/wifi_test.py [-p PORT] [--flash BUILD_DIR] SUITE

Suites (SUITE = all runs them in this order, except `unconfigured`):
  connect      the wifi-connect skill via tools/wifi_from_env.py: wifi status shows up + IP
  txpower      wifi txpower: valid/invalid values, driver readback in the INFO log
  transitions  wrong password, non-existent SSID, recovery: exactly one `wifi` event per
               up/down transition, no event per retry, firmware answers throughout
  leak         after `sys log debug`, `wifi set` output does not contain the password
               (the typed command's echo is the only expected occurrence)
  persist      credentials and TX power survive `sys reboot`; the driver TX power still
               equals cfg after the reconnect
  rapid        back-to-back `wifi set` during a connect attempt ends on the newest credentials
  unconfigured DESTRUCTIVE, not part of `all`, run it by name: erases the whole NVS
               partition (also the Telegram token, owner and boot levels; there is no command
               that clears only the Wi-Fi keys). Checks state `unconfigured`, no connect
               attempts and no wifi events; then provisions Wi-Fi again.

The credentials are read from .env.local by this script and never printed: the output only
has check names and PASS/FAIL. Exit 0 = all checks passed, 1 = a check failed, 2 = setup
error. Talks to the board through tools/embpet_cli.py (subprocess) and, for `wifi set`, the
tools/embpet package in-process (wifi_from_env.wifi_set, values never in argv). The leak
suite passes the values in argv on purpose: it needs the CLI's --raw console capture.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wifi_from_env as W  # noqa: E402

NVS_OFFSET, NVS_SIZE = "0x9000", "0x6000"     # partitions.csv
NOSUCH_SSID = "embpet-no-such-network"


class Checks:
    def __init__(self, secrets=()):
        self.results = []
        self.secrets = list(secrets)

    def check(self, name, cond, detail=None):
        detail = W.mask(detail, self.secrets)       # a failure detail never shows credentials
        self.results.append({"check": name, "ok": bool(cond)})
        print("%s %s%s" % ("PASS" if cond else "FAIL", name,
                           "" if cond or detail is None else "  [%s]" % detail), flush=True)
        return bool(cond)

    def ok(self):
        return all(r["ok"] for r in self.results)


class Ctx:
    def __init__(self, port, ssid, password):
        self.port, self.ssid, self.password = port, ssid, password
        self.c = Checks([ssid, password])

    def cli(self, *args, timeout=15.0, extra=()):
        return W.run_cli(list(args), self.port, timeout, extra)

    def res(self, *args, **kw):
        return self.cli(*args, **kw)[1]

    def status(self):
        return self.res("wifi", "status", timeout=10.0) or {}

    def wait_state(self, want, seconds):
        deadline = time.monotonic() + seconds
        st = {}
        while time.monotonic() < deadline:
            st = self.status()
            if st.get("state") == want:
                return st
            time.sleep(1.5)
        return st

    def info(self):
        return self.res("sys", "info", timeout=10.0) or {}

    def set_creds(self, ssid, password):
        return W.wifi_set(ssid, password, self.port)[1]

    def wifi_events(self, since):
        """wifi event states with seq > since, from the replay ring."""
        _, res, err = self.cli("evt", "since", str(since), extra=["--show-events"])
        states = []
        for line in err.splitlines():
            try:
                f = json.loads(line)
            except ValueError:
                continue
            if f.get("t") == "evt" and f.get("ev") == "wifi" and f.get("seq", 0) > since:
                states.append(f.get("state"))
        return states, res

    def responsive(self, seconds):
        """sys info must keep answering for `seconds`; returns (answers, failures)."""
        deadline = time.monotonic() + seconds
        good = bad = 0
        while time.monotonic() < deadline:
            if self.info().get("ok"):
                good += 1
            else:
                bad += 1
            time.sleep(1.0)
        return good, bad

    def recover(self, name):
        """Restore the real credentials and wait for up."""
        r = self.set_creds(self.ssid, self.password)
        st = self.wait_state("up", 45)
        self.c.check(name, bool(r and r.get("ok")) and st.get("state") == "up", st)
        return st


def flash(port, build_dir, erase_nvs=False):
    base = [sys.executable, "-m", "esptool", "--chip", "esp32c3", "-p", port, "-b", "460800"]
    if erase_nvs:
        cmd = base + ["--after", "hard_reset", "erase_region", NVS_OFFSET, NVS_SIZE]
        cwd = None
    else:
        cmd = base + ["--before", "default_reset", "--after", "hard_reset", "write_flash",
                      "@flash_args"]
        cwd = build_dir
    r = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        sys.stderr.write(r.stdout.decode("utf-8", "replace"))
        raise SystemExit("esptool failed")
    time.sleep(4.0)


def suite_connect(x):
    """The skill flow: provision via the helper, then status up with an IP."""
    code, res, _ = W.run_cli(["wifi", "status"], x.port, timeout=10.0)
    x.c.check("connect: firmware answers wifi status", bool(res and res.get("ok")), res)
    r = subprocess.run([sys.executable, os.path.join(W.ROOT, "tools", "wifi_from_env.py")] +
                       (["-p", x.port] if x.port else []),
                       cwd=W.ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out = r.stdout.decode("utf-8", "replace")
    x.c.check("connect: helper output has no credentials",
              x.ssid not in out and x.password not in out)
    try:
        j = json.loads(out)
    except ValueError:
        j = {}
    st = j.get("status") or {}
    x.c.check("connect: wifi_from_env exit 0", r.returncode == 0, r.returncode)
    x.c.check("connect: set res ok", (j.get("set") or {}).get("ok") is True, j.get("set"))
    x.c.check("connect: status up with IP",
              st.get("state") == "up" and re.match(r"^\d+\.\d+\.\d+\.\d+$", st.get("ip", "")),
              {k: v for k, v in st.items() if k != "ssid"})
    print("INFO ip=%s rssi=%s txpower=%s" % (st.get("ip"), st.get("rssi"), st.get("txpower")))
    st2 = x.status()
    x.c.check("connect: status ssid is the configured one, password never shown",
              st2.get("ssid") == x.ssid and "pass" not in json.dumps(st2).lower(),
              sorted(st2))


def suite_txpower(x):
    c = x.c
    c.check("txpower: default 8.5 or previously set value reported",
            isinstance(x.status().get("txpower"), (int, float)))
    for bad in ("1.75", "20.25", "8.3", "abc", "-5", "21"):
        r = x.res("wifi", "txpower", "--", bad)
        c.check("txpower: reject %s" % bad, r and r.get("ok") is False and
                r.get("err") == "bad_args", r)
    x.res("sys", "log", "info")
    _, r, err = x.cli("wifi", "txpower", "10", extra=["--raw"])
    c.check("txpower: 10 accepted", r and r.get("ok") and r.get("txpower") == 10, r)
    m = re.search(r"wifi_sta: tx power (\d+) qdbm requested, driver reports (\d+) \((\w+)\)", err)
    c.check("txpower: set_max_tx_power returned ESP_OK, driver reports a value",
            bool(m) and m.group(3) == "ESP_OK", m.group(0) if m else W.mask(err[-300:], x.c.secrets))
    if m:
        print("INFO txpower 10 dBm -> %s qdbm requested, driver reports %s qdbm" %
              (m.group(1), m.group(2)))
    r = x.res("wifi", "txpower", "8.5")
    c.check("txpower: 8.5 accepted", r and r.get("ok") and r.get("txpower") == 8.5, r)
    x.res("sys", "log", "warn")
    c.check("txpower: status reports 8.5", x.status().get("txpower") == 8.5)


def suite_transitions(x):
    c = x.c
    st = x.wait_state("up", 45)
    if not c.check("transitions: start up", st.get("state") == "up", st):
        return
    boot0 = x.info().get("boot")
    wrong = x.password[:-1] + ("a" if x.password[-1] != "a" else "b")
    cases = [
        ("wrong password", x.ssid, wrong),
        ("non-existent SSID", NOSUCH_SSID, "embpet-wrong-pass-0"),
    ]
    for name, ssid, pw in cases:
        seq0 = x.info().get("seq", 0)
        r = x.set_creds(ssid, pw)
        c.check("transitions[%s]: wifi set answers ok at once" % name, r and r.get("ok"), r)
        st = x.wait_state("down", 40)
        c.check("transitions[%s]: state down" % name, st.get("state") == "down", st)
        good, bad = x.responsive(25)       # spans several backoff retries (1,2,4,8,16 s)
        c.check("transitions[%s]: firmware answers during retries (%d answers)" % (name, good),
                bad == 0 and good >= 5, (good, bad))
        states, _ = x.wifi_events(seq0)
        c.check("transitions[%s]: exactly one wifi event, state down" % name,
                states == ["down"], states)
        seq1 = x.info().get("seq", 0)
        x.recover("transitions[%s]: recovery reaches up" % name)
        states, _ = x.wifi_events(seq1)
        c.check("transitions[%s]: recovery emits exactly one up event" % name,
                states == ["up"], states)
        time.sleep(3)
    c.check("transitions: no reboot during the run", x.info().get("boot") == boot0)


def suite_leak(x):
    c = x.c
    pw = x.password
    mid = pw[len(pw) // 2 - 2:len(pw) // 2 + 2] if len(pw) >= 8 else pw
    x.res("sys", "log", "debug")
    _, r, err = x.cli("wifi", "set", "--", x.ssid, x.password, extra=["--raw"])
    c.check("leak: wifi set ok", r and r.get("ok"), r)
    lines = [ln for ln in err.splitlines() if "wifi set" not in ln]   # drop the input echo
    c.check("leak: wifi set output has no password / middle chars",
            x.password not in "\n".join(lines) and mid not in "\n".join(lines))
    c.check("leak: wifi set output has no SSID", x.ssid not in "\n".join(lines))
    mon = subprocess.run([sys.executable, os.path.join(W.ROOT, "tools", "embpet_cli.py")] +
                         (["-p", x.port] if x.port else []) + ["--monitor", "14"],
                         cwd=W.ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    text = mon.stdout.decode("utf-8", "replace")
    n_ssid = text.count(x.ssid)
    c.check("leak: reconnect log (14 s, debug) has no password / middle chars",
            x.password not in text and mid not in text)
    print("INFO ssid occurrences in 14 s debug reconnect log: %d (driver prints the SSID)" %
          n_ssid)
    x.res("sys", "log", "warn")
    x.wait_state("up", 45)


def suite_persist(x):
    c = x.c
    x.res("wifi", "txpower", "11.25")
    boot0 = x.info().get("boot")
    r = x.res("sys", "reboot")
    c.check("persist: sys reboot ok", r and r.get("ok"), r)
    time.sleep(4)
    info = {}
    for _ in range(6):
        info = x.info()
        if info.get("ok"):
            break
        time.sleep(2)
    c.check("persist: new boot id", info.get("ok") and info.get("boot") != boot0, info)
    st = x.wait_state("up", 45)
    c.check("persist: ssid and txpower read back, reconnected on its own",
            st.get("ssid") == x.ssid and st.get("txpower") == 11.25 and
            st.get("state") == "up", {k: v for k, v in st.items() if k != "ssid"})
    # The AP's country IE may reset the limit on association: the driver value must match
    # cfg after the reconnect (wifi.c re-applies it on GOT_IP and logs it at INFO).
    x.res("sys", "log", "info")
    x.set_creds(x.ssid, x.password)
    mon = subprocess.run([sys.executable, os.path.join(W.ROOT, "tools", "embpet_cli.py")] +
                         (["-p", x.port] if x.port else []) + ["--monitor", "12"],
                         cwd=W.ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    text = W.mask(mon.stdout.decode("utf-8", "replace"), x.c.secrets)
    got = re.findall(r"wifi_sta: tx power (\d+) qdbm requested, driver reports (-?\d+)", text)
    c.check("persist: after reconnect the driver TX power equals cfg (45 qdbm)",
            bool(got) and got[-1] == ("45", "45"), got[-3:])
    x.res("sys", "log", "warn")
    x.res("wifi", "txpower", "8.5")
    x.wait_state("up", 45)


def suite_rapid(x):
    """`wifi set` while a connect attempt is in flight must end on the newest credentials."""
    c = x.c
    wrong = x.password[:-1] + ("a" if x.password[-1] != "a" else "b")
    for name, first, second, want in (
            ("wrong then right", (x.ssid, wrong), (x.ssid, x.password), "up"),
            ("nonexistent then right", (NOSUCH_SSID, "embpet-wrong-pass-0"),
             (x.ssid, x.password), "up"),
            ("right then wrong", (x.ssid, x.password), (x.ssid, wrong), "down")):
        x.set_creds(*first)
        x.set_creds(*second)
        x.wait_state(want, 45)
        if want == "down":
            time.sleep(8)           # it must stay down: the newest (wrong) credentials won
        st = x.status()
        c.check("rapid[%s]: ends %s" % (name, want), st.get("state") == want, st)
        if want == "down":
            x.recover("rapid[%s]: recovery reaches up" % name)
        time.sleep(3)


def suite_unconfigured(x):
    c = x.c
    flash(x.port, None, erase_nvs=True)
    st = {}
    for _ in range(8):
        st = x.status()
        if st.get("ok"):
            break
        time.sleep(2)
    c.check("unconfigured: state unconfigured, empty ssid, default txpower",
            st.get("state") == "unconfigured" and st.get("ssid") == "" and
            st.get("txpower") == 8.5, st)
    time.sleep(12)
    states, _ = x.wifi_events(0)
    c.check("unconfigured: no wifi events and still unconfigured after 12 s",
            states == [] and x.status().get("state") == "unconfigured", states)
    x.recover("unconfigured: provisioning from unconfigured reaches up")


SUITES = {"connect": suite_connect, "txpower": suite_txpower,
          "transitions": suite_transitions, "leak": suite_leak, "persist": suite_persist,
          "rapid": suite_rapid, "unconfigured": suite_unconfigured}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-p", "--port")
    ap.add_argument("--flash", metavar="BUILD_DIR")
    ap.add_argument("suite", choices=["all"] + list(SUITES))
    a = ap.parse_args()
    try:
        env = W.load_env(os.path.join(W.ROOT, ".env.local"))
    except OSError:
        print("cannot read .env.local", file=sys.stderr)
        return 2
    if not env.get("WIFI_SSID") or not env.get("WIFI_PASS"):
        print("WIFI_SSID / WIFI_PASS missing in .env.local", file=sys.stderr)
        return 2
    from embpet import find_port
    port = a.port or find_port()
    if a.flash:
        flash(port, a.flash)
    x = Ctx(port, env["WIFI_SSID"], env["WIFI_PASS"])
    names = [n for n in SUITES if n != "unconfigured"] if a.suite == "all" else [a.suite]
    for n in names:
        print("== %s" % n, flush=True)
        SUITES[n](x)
    failed = [r["check"] for r in x.c.results if not r["ok"]]
    print(json.dumps({"ok": x.c.ok(), "passed": len(x.c.results) - len(failed),
                      "failed": failed}))
    return 0 if x.c.ok() else 1


if __name__ == "__main__":
    sys.exit(main())
