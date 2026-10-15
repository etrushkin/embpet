#!/usr/bin/env python3
"""Device tests for the embpet console (embpet-86h). Needs the board; run under the lock:

  scripts/hw.sh python tools/console_test.py [-p PORT] [--flash BUILD_DIR] functional
  scripts/hw.sh python tools/console_test.py [-p PORT] [--flash BUILD_DIR] soak --closed SEC
  scripts/hw.sh python tools/console_test.py [-p PORT] linekill
  scripts/hw.sh python tools/console_test.py [-p PORT] reopen [-n N]

--flash writes BUILD_DIR/flash_args with esptool first, so flash and test run under one
board lock.

functional: every command answers with exactly one parseable res (rid/actor echoed);
  unknown commands, bad options, bad escapes, over-long lines get err res; escapes and
  UTF-8 reach the command decoded; blank lines and CRLF give no extra res; sys log,
  evt since, help; sys reboot answers first, then the boot id changes and the new boot
  event (reset_reason software) is in the ring once. Production firmware (build-86h).

soak: needs the soak firmware (CONFIG_EMBPET_CONSOLE_SOAK: a soak event and a WARN log
  every 100 ms). Reads sys info (first waiting for uptime 16 s: the generator keeps its
  boot-time maxima apart, embpet-s2j.5, and they are only reported), closes the port for
  SEC seconds, reopens and checks: same boot id, uptime advanced by about SEC, log lines
  were dropped, commands still answer,
  `evt since <last seq>` replays the newest events with truncated:true, and the replayed
  soak events show that event writes, log writes and the generator's 100 ms loop never
  stalled.

linekill (embpet-s2j.6): write a partial `sys log debug` (and one with an open quote) without
  newline, close and reopen, send Ctrl-U + newline, then `sys info`: log level unchanged,
  answered, no res for the leftover; a control without Ctrl-U shows the leftover does run.

reopen (embpet-s2j.1): N separate embpet_cli.py runs keep the boot id and a rising
  uptime (opening the port does not reset the chip); `embpet_cli.py --monitor 1 --reset`
  still resets it.

pipeline (embpet-s2j.3): needs the probe firmware (components/proto/probe.defaults).
  `probe sleep MS` blocks the console like a pending `tg send`, and right behind it the
  host writes LINES `probe echo` commands of BYTES text each (default 10 x 1000, about
  10 KB). Every line must be answered once, whole (length and Adler-32), in order. Then
  `probe rxoff` turns the USB RX interrupt off as a lost re-enable would, and the next
  command must still be answered within 1.5 s.

Prints a JSON summary; exit 0 = all checks passed, 1 = a check failed, 2 = serial error.
"""
import argparse
import json
import os
import random
import string
import subprocess
import sys
import threading
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from embpet_cli import find_port, open_port  # noqa: E402

try:
    import serial
except ImportError:
    sys.exit("pyserial missing: run with scripts/idf.sh python ...")

CMDLINE_MAX = 4400
SOAK_SETTLE_S = 16      # the soak generator's checked maxima start at uptime 15 s (console_soak.c)


class Board:
    """One open port. Collects every frame in arrival order; counts res per rid."""

    def __init__(self, port, settle=1.5):
        self.s = open_port(port)
        self.buf = b""
        self.frames = []            # every parsed frame, arrival order
        self.res_by_rid = {}        # rid -> number of res frames seen
        self.bad_lines = []         # lines starting with @@ that are not JSON frames
        self.pump(settle)

    def close(self):
        self.s.close()

    def _line(self, raw):
        text = raw.decode("utf-8", "replace").rstrip("\r")
        if not text.startswith("@@{"):
            return None
        try:
            f = json.loads(text[2:])
        except ValueError:
            self.bad_lines.append(text[:120])
            return None
        self.frames.append(f)
        if f.get("t") == "res" and "rid" in f:
            self.res_by_rid[f["rid"]] = self.res_by_rid.get(f["rid"], 0) + 1
        return f

    def pump(self, seconds, until=None):
        """Read for up to `seconds`; stop early when until(frame) is true. Returns that
        frame or None."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.buf += self.s.read(4096)
            while b"\n" in self.buf:
                raw, self.buf = self.buf.split(b"\n", 1)
                f = self._line(raw)
                if f is not None and until is not None and until(f):
                    return f
        return None

    def send(self, data):
        if isinstance(data, str):
            data = data.encode("utf-8")
        self.s.write(data)
        self.s.flush()

    def cmd(self, line, rid=None, match=None, timeout=5.0):
        """Send one line; wait for its res (by rid, or by match(frame))."""
        start = len(self.frames)
        self.send(line + "\n")
        pred = match or (lambda f: f.get("t") == "res" and f.get("rid") == rid)
        f = self.pump(timeout, pred)
        return f, self.frames[start:]


class Checks:
    def __init__(self):
        self.results = []

    def check(self, name, cond, detail=None):
        self.results.append({"check": name, "ok": bool(cond), "detail": detail})
        if not cond:
            print("FAIL %s: %s" % (name, json.dumps(detail, ensure_ascii=False)[:300]),
                  file=sys.stderr)
        return cond

    def ok(self):
        return all(r["ok"] for r in self.results)


def flash(port, build_dir):
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32c3", "-p", port, "-b", "460800",
           "--before", "default_reset", "--after", "hard_reset", "write_flash", "@flash_args"]
    r = subprocess.run(cmd, cwd=build_dir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        sys.stderr.write(r.stdout.decode("utf-8", "replace"))
        raise SystemExit("flash failed")
    time.sleep(3.0)


def expect_err(c, b, name, line, rid, err, **extra):
    f, _ = b.cmd(line, rid)
    good = (f is not None and f.get("ok") is False and f.get("err") == err and
            all(f.get(k) == v for k, v in extra.items()))
    c.check(name, good, f)
    return f


def functional(port):
    c = Checks()
    b = Board(port, settle=3.0)
    f, _ = b.cmd("sys info -i r1 -a A", "r1")
    c.check("sys info ok, rid/actor/cmd echoed",
            f and f.get("ok") is True and f.get("actor") == "A" and f.get("cmd") == "sys info"
            and all(k in f for k in ("fw", "idf", "uptime_s", "heap_free", "heap_min", "mac",
                                     "reset_reason", "log", "log_dropped")), f)
    boot = f.get("boot") if f else None
    c.check("runtime log level is warn after init", f and f.get("log") == "warn", f)

    f, _ = b.cmd("help -i r2", "r2")
    c.check("help lists commands", f and f.get("ok") and "sys info" in f.get("cmds", []), f)

    expect_err(c, b, "unknown command", "foo -i r3 -a B", "r3", "bad_args", actor="B",
               cmd="foo")
    expect_err(c, b, "unknown subcommand", "sys nope -i r4", "r4", "bad_args", cmd="sys nope")
    expect_err(c, b, "unknown option", "sys info -i r5 -x", "r5", "bad_args", cmd="sys info")
    f = expect_err(c, b, "-- ends options", "sys info -i r5c -- -x", "r5c", "bad_args")
    c.check("after --, -x is an argument", f and "unexpected argument" in f.get("msg", ""), f)
    f, _ = b.cmd("sys info -a A7 -i bad.rid", match=lambda f: f.get("t") == "res"
                 and f.get("actor") == "A7")
    c.check("invalid rid: err, rid not echoed",
            f and f.get("err") == "bad_args" and "rid" not in f, f)
    expect_err(c, b, "actor too long", "sys info -i r5b -a 0123456789abcdefg", "r5b",
               "bad_args")

    f, _ = b.cmd("sys log -i r6 info", "r6")
    c.check("sys log info", f and f.get("ok") and f.get("level") == "info", f)
    f, _ = b.cmd("sys info -i r7", "r7")
    c.check("sys info shows log info", f and f.get("log") == "info", f)
    f, _ = b.cmd("sys log -i r8 \"warn\"", "r8")
    c.check("sys log with quoted arg", f and f.get("ok") and f.get("level") == "warn", f)
    f, _ = b.cmd("sys\tlog\t-i r8t\twarn", "r8t")
    c.check("TAB separates arguments", f and f.get("ok") and f.get("level") == "warn", f)
    f = expect_err(c, b, "TAB inside quotes is text", "sys info -i r8q \"a\tb\"", "r8q",
                   "bad_args")
    c.check("quoted TAB kept", f and "a\tb" in f.get("msg", ""), f)
    expect_err(c, b, "sys log bad level", "sys log -i r9 loud", "r9", "bad_args")
    expect_err(c, b, "sys log missing level", "sys log -i r10", "r10", "bad_args")

    f, frames = b.cmd("evt since -i r11 0", "r11")
    boots = [x for x in frames if x.get("t") == "evt" and x.get("ev") == "boot"]
    c.check("evt since 0 replays the boot event once, then res",
            f and f.get("ok") and f.get("truncated") is False and len(boots) == 1
            and f.get("count", 0) >= 1 and boots[0].get("seq", 0) < f["seq"], [f] + boots)
    expect_err(c, b, "evt since non-numeric", "evt since -i r12 abc", "r12", "bad_args")
    expect_err(c, b, "evt since > u32", "evt since -i r13 4294967296", "r13", "bad_args")

    expect_err(c, b, "unterminated quote", "sys log -i r14 \"warn", "r14", "bad_args",
               msg="missing closing quote")
    expect_err(c, b, "unknown escape", "sys log -i r15 \"wa\\qrn\"", "r15", "bad_args")
    expect_err(c, b, "trailing backslash", "sys log -i r16 \"warn\\", "r16", "bad_args",
               msg="line ends with a backslash")
    f = expect_err(c, b, "escapes decoded", "sys info -i r17 \"a\\nb \\\"q\\\" c\\\\d\"",
                   "r17", "bad_args")
    c.check("escaped text reaches argtable decoded",
            f and 'a\nb "q" c\\d' in f.get("msg", ""), f)
    f = expect_err(c, b, "utf-8 arg", "sys info -i r18 \"Grüße 👋\"", "r18", "bad_args")
    c.check("utf-8 passes through", f and "Grüße 👋" in f.get("msg", ""), f)

    head = "sys info -i r19 \""
    line = head + "x" * (CMDLINE_MAX - len(head) - 1) + "\""
    f = expect_err(c, b, "line of exactly CMDLINE_MAX is parsed", line, "r19", "bad_args")
    c.check("max line is not too_long", f and f.get("err") == "bad_args", len(line))
    head = "sys info -i r20 \""
    expect_err(c, b, "line longer than CMDLINE_MAX", head + "x" * CMDLINE_MAX + "\"", "r20",
               "too_long", cmd="sys info")

    b.send("\r\n\n   \r\n")
    b.pump(0.5)
    f, frames = b.cmd("sys info -i r21\r", "r21")
    c.check("CRLF/blank lines: one res", f and f.get("ok"), f)
    b.send("sys info -i r22\nsys info -i r23\n")
    f = b.pump(3.0, lambda x: x.get("rid") == "r23")
    c.check("two commands in one write", f is not None and b.res_by_rid.get("r22") == 1, f)
    f, _ = b.cmd("sys infx\x7fo -i r24", "r24")
    c.check("backspace edits the line", f and f.get("ok") and f.get("cmd") == "sys info", f)

    b.pump(0.5)                                     # late duplicates would show up here
    dup = {k: v for k, v in b.res_by_rid.items() if v != 1}
    c.check("exactly one res per rid", not dup and len(b.res_by_rid) >= 20,
            {"dups": dup, "rids": len(b.res_by_rid)})
    c.check("all @@ lines parse", not b.bad_lines, b.bad_lines[:3])

    f, _ = b.cmd("sys reboot -i r30", "r30")
    c.check("sys reboot answers first", f and f.get("ok") and f.get("boot") == boot, f)
    b.close()
    time.sleep(4.0)
    b = Board(port, settle=3.0)
    f, frames = b.cmd("evt since -i r31 0", "r31")
    boots = [x for x in frames if x.get("t") == "evt" and x.get("ev") == "boot"]
    c.check("after reboot: new boot id, one boot event, reset_reason software",
            f and f.get("boot") != boot and len(boots) == 1
            and boots[0].get("reset_reason") == "software" and boots[0].get("seq") == 1,
            [f] + boots)
    f, _ = b.cmd("sys info -i r32", "r32")
    c.check("after reboot: log level warn", f and f.get("log") == "warn", f)
    b.close()
    return c


def soak(port, closed):
    c = Checks()
    b = Board(port, settle=3.0)
    f0, _ = b.cmd("sys info -i s1", "s1")
    if not c.check("sys info before close", f0 and f0.get("ok"), f0):
        return c, {}
    if f0.get("uptime_s", 0) < SOAK_SETTLE_S:       # close only after the boot window
        time.sleep(SOAK_SETTLE_S - f0["uptime_s"])
        f0, _ = b.cmd("sys info -i s0", "s0")
        if not c.check("sys info after the settle wait", f0 and f0.get("ok"), f0):
            return c, {}
    soak_evts = [x for x in b.frames if x.get("ev") == "soak"]
    c.check("soak generator running", len(soak_evts) > 5, len(soak_evts))
    last_seq = max(x["seq"] for x in b.frames if x.get("t") == "evt")
    b.close()
    t_closed = time.monotonic()
    time.sleep(closed)
    gap = time.monotonic() - t_closed

    b = Board(port, settle=2.0)                     # also drains stale ring/FIFO output
    f1, _ = b.cmd("sys info -i s2", "s2")
    c.check("same boot after reconnect", f1 and f1.get("boot") == f0.get("boot"), f1)
    d_up = (f1 or {}).get("uptime_s", 0) - f0.get("uptime_s", 0)
    c.check("uptime advanced by the closed time", abs(d_up - gap) < 10, {"d_up": d_up,
                                                                          "gap": gap})
    c.check("log lines were dropped while closed",
            (f1 or {}).get("log_dropped", 0) > f0.get("log_dropped", 0),
            [f0.get("log_dropped"), (f1 or {}).get("log_dropped")])

    f2, frames = b.cmd("evt since -i s3 %d" % last_seq, "s3")
    c.check("evt since after reconnect: truncated", f2 and f2.get("ok")
            and f2.get("truncated") is True, f2)
    # The replay is one uninterrupted block (proto holds its lock), recognisable by the
    # backward seq jump at its start; it can include events the host also got live.
    evts = [x for x in frames if x.get("t") == "evt"]
    start = next((i for i in range(1, len(evts)) if evts[i]["seq"] <= evts[i - 1]["seq"]), 0)
    replay = evts[start:start + (f2 or {}).get("count", 0)]
    seqs = [x["seq"] for x in replay]
    c.check("replay: count frames, strictly increasing, newer than since, gap at start",
            f2 and len(replay) == f2.get("count")
            and all(b2 > a2 for a2, b2 in zip(seqs, seqs[1:]))
            and seqs and seqs[0] > last_seq + 1,
            {"count": (f2 or {}).get("count"), "seqs": seqs[:3] + seqs[-3:]})
    stats = {}
    last_soak = [x for x in b.frames if x.get("ev") == "soak"]
    if last_soak:
        s = last_soak[-1]
        stats = {k: s.get(k) for k in ("n", "settled", "max_evt_us", "max_log_us",
                                       "max_period_us", "boot_max_evt_us", "boot_max_log_us",
                                       "boot_max_period_us")}
    # Only the settled maxima are checked; boot_max_* are reported (embpet-s2j.5).
    c.check("no output stall > 60 ms, generator loop never late > 200 ms",
            stats and stats["settled"] is True and stats["max_evt_us"] < 60000
            and stats["max_log_us"] < 60000 and stats["max_period_us"] < 200000, stats)
    c.check("generator kept counting while closed",
            stats and stats["n"] * 0.1 >= gap * 0.9, {"n": stats.get("n"), "gap": gap})
    for i in range(5):
        f, _ = b.cmd("sys info -i s%d" % (10 + i), "s%d" % (10 + i))
        c.check("responsive after reconnect %d" % i, f and f.get("ok"), f)
    dup = {k: v for k, v in b.res_by_rid.items() if v != 1}
    c.check("exactly one res per rid", not dup, dup)
    b.close()
    info = {"closed_s": round(gap, 1), "uptime_before": f0.get("uptime_s"),
            "uptime_after": (f1 or {}).get("uptime_s"), "boot": f0.get("boot"),
            "log_dropped": (f1 or {}).get("log_dropped"), "since": last_seq,
            "replayed": (f2 or {}).get("count"), "truncated": (f2 or {}).get("truncated"),
            "soak": stats, "heap_min": (f1 or {}).get("heap_min")}
    return c, info


def reopen(port, n):
    """embpet-s2j.1: n separate embpet_cli.py runs (each opens and closes the port) keep
    the boot id and a rising uptime; then `--monitor 1 --reset` does reset the chip."""
    c = Checks()
    cli = [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                        "embpet_cli.py"), "-p", port]
    runs = []
    for i in range(n):
        r = subprocess.run(cli + ["sys", "info"], stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL)
        try:
            f = json.loads(r.stdout.decode("utf-8"))
        except ValueError:
            f = {}
        runs.append((f.get("boot"), f.get("uptime_s")))
        time.sleep(1.1)
    boots = {b for b, _ in runs}
    ups = [u for _, u in runs]
    c.check("%d CLI runs answered" % n, all(b for b, _ in runs), runs)
    c.check("same boot id across all runs", len(boots) == 1, runs)
    c.check("uptime increases", all(isinstance(u, int) for u in ups)
            and all(b > a for a, b in zip(ups, ups[1:])), ups)
    subprocess.run(cli + ["--monitor", "1", "--reset"], stdout=subprocess.DEVNULL)
    time.sleep(3.0)
    r = subprocess.run(cli + ["sys", "info"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    try:
        f = json.loads(r.stdout.decode("utf-8"))
    except ValueError:
        f = {}
    c.check("--reset resets the chip (new boot id)",
            f.get("boot") and f.get("boot") not in boots, f)
    return c, {"runs": runs, "after_reset": [f.get("boot"), f.get("uptime_s"),
                                             f.get("reset_reason")]}


def linekill(port):
    """embpet-s2j.6: a partial line left by a closed session is discarded by Ctrl-U. Two
    rounds, each: write the partial line without a newline, close, reopen, send Ctrl-U +
    newline, then `sys info`. The log level must be unchanged, sys info answered once, and no
    res may appear for the leftover. Round 2 leaves an open quote."""
    c = Checks()
    b = Board(port, settle=1.0)
    b.send("\x15\n")
    f, _ = b.cmd("sys log -i k0 warn", "k0")
    c.check("start at log level warn", f and f.get("ok") and f.get("level") == "warn", f)
    for n, partial in enumerate(("sys log debug", "sys log -i kq \"debug")):
        b.send(partial)                             # no newline
        b.pump(0.3)
        b.close()
        time.sleep(0.5)
        b = Board(port, settle=0.5)
        b.send("\x15\n")
        rid = "k%d" % (n + 1)
        f, frames = b.cmd("sys info -i %s" % rid, rid)
        c.check("round %d: sys info answered" % (n + 1), f and f.get("ok") is True, f)
        c.check("round %d: log level unchanged" % (n + 1), f and f.get("log") == "warn", f)
        b.pump(0.5)
        stray = [x for x in b.frames if x.get("t") == "res" and x.get("rid") != rid]
        c.check("round %d: no res for the leftover" % (n + 1), not stray, stray[:2])
    # control: without Ctrl-U the leftover joins the next line and does run
    b.send("sys log debug")
    b.pump(0.3)
    b.close()
    time.sleep(0.5)
    b = Board(port, settle=0.5)
    f, _ = b.cmd(" -i k9", "k9")
    c.check("control: without Ctrl-U the leftover runs", f and f.get("level") == "debug", f)
    b.send("\x15\n")
    f, _ = b.cmd("sys log -i k10 warn", "k10")
    c.check("level restored to warn", f and f.get("ok") and f.get("level") == "warn", f)
    b.close()
    return c, {}


def pipeline(port, sleep_ms, lines, length):
    """embpet-s2j.3: while one command blocks for SLEEP_MS, the host pipelines LINES
    `probe echo` commands of LENGTH text bytes each (written by a second thread, so the main
    thread keeps reading echo and frames). Every line must arrive whole: one ok res per rid,
    with the text's length and Adler-32, in send order."""
    c = Checks()
    b = Board(port, settle=1.0)
    f, _ = b.cmd("probe nop -i p0", "p0")
    if not c.check("probe build (probe nop answers)", f and f.get("ok") is True, f):
        return c, {}
    rnd = random.Random(sleep_ms * 7919 + lines)
    texts = ["".join(rnd.choice(string.ascii_letters + string.digits) for _ in range(length))
             for _ in range(lines)]
    blob = "probe sleep -i p1 %d\n" % sleep_ms
    blob += "".join("probe echo -i e%d -- %s\n" % (k, t) for k, t in enumerate(texts))
    want = {"p1"} | {"e%d" % k for k in range(lines)}
    start = len(b.frames)
    done = {}

    def writer():
        t0 = time.monotonic()
        b.send(blob)
        done["write_s"] = time.monotonic() - t0
    th = threading.Thread(target=writer)
    t0 = time.monotonic()
    th.start()
    b.pump(sleep_ms / 1000.0 + 20.0,
           lambda f: want <= set(b.res_by_rid) and "write_s" in done)
    th.join(30.0)
    elapsed = time.monotonic() - t0
    res = [x for x in b.frames[start:] if x.get("t") == "res" and x.get("rid") in want]
    by_rid = {x["rid"]: x for x in res}
    c.check("sleep answered ok", by_rid.get("p1", {}).get("ok") is True, by_rid.get("p1"))
    bad = []
    for k, t in enumerate(texts):
        x = by_rid.get("e%d" % k)
        if not (x and x.get("ok") is True and x.get("len") == len(t)
                and x.get("sum") == zlib.adler32(t.encode("ascii"))):
            bad.append({"rid": "e%d" % k, "res": x})
    c.check("every pipelined line arrived whole (len and Adler-32)", not bad, bad[:3])
    order = [x["rid"] for x in res]
    c.check("responses in send order", order == ["p1"] + ["e%d" % k for k in range(lines)],
            order[:5])
    dup = {r: n for r, n in b.res_by_rid.items() if n != 1}
    c.check("exactly one res per rid", not dup, dup)
    c.check("no broken frame lines", not b.bad_lines, b.bad_lines[:3])
    f, _ = b.cmd("sys info -i p2", "p2")
    c.check("responsive afterwards", f and f.get("ok") is True, f)
    # Fault: RX interrupt off while not gated (a lost re-enable); the REPL's read timeout
    # must turn it back on, so the next command is answered within about 1 s.
    fx, _ = b.cmd("probe rxoff -i p3", "p3")
    t_rx = time.monotonic()
    fr, _ = b.cmd("probe nop -i p4", "p4", timeout=5.0)
    recover_s = time.monotonic() - t_rx
    c.check("input recovers from a lost RX re-enable within 1.5 s",
            fx and fx.get("ok") is True and fr and fr.get("ok") is True and recover_s < 1.5,
            {"rxoff": fx, "nop": fr, "recover_s": round(recover_s, 2)})
    b.close()
    return c, {"bytes": len(blob), "sleep_ms": sleep_ms, "lines": lines,
               "answered": len(res), "write_s": round(done.get("write_s", -1), 2),
               "elapsed_s": round(elapsed, 2), "heap_min": (f or {}).get("heap_min"),
               "rx_recover_s": round(recover_s, 2)}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-p", "--port")
    ap.add_argument("--flash", metavar="BUILD_DIR")
    sub = ap.add_subparsers(dest="mode", required=True)
    sub.add_parser("functional")
    sub.add_parser("linekill")
    sp = sub.add_parser("soak")
    sp.add_argument("--closed", type=float, default=120.0, metavar="SEC")
    rp = sub.add_parser("reopen")
    rp.add_argument("-n", type=int, default=10)
    pp = sub.add_parser("pipeline")
    pp.add_argument("--sleep", type=int, default=10000, metavar="MS")
    pp.add_argument("--lines", type=int, default=10)
    pp.add_argument("--length", type=int, default=1000, metavar="BYTES")
    a = ap.parse_args()
    port = a.port or find_port()
    try:
        if a.flash:
            flash(port, a.flash)
        if a.mode == "functional":
            c, info = functional(port), {}
        elif a.mode == "linekill":
            c, info = linekill(port)
        elif a.mode == "reopen":
            c, info = reopen(port, a.n)
        elif a.mode == "pipeline":
            c, info = pipeline(port, a.sleep, a.lines, a.length)
        else:
            c, info = soak(port, a.closed)
    except serial.SerialException as e:
        print("serial error: %s" % e, file=sys.stderr)
        return 2
    print(json.dumps({"mode": a.mode, "ok": c.ok(),
                      "passed": sum(r["ok"] for r in c.results),
                      "failed": [r["check"] for r in c.results if not r["ok"]],
                      "info": info}, ensure_ascii=False))
    return 0 if c.ok() else 1


if __name__ == "__main__":
    sys.exit(main())
