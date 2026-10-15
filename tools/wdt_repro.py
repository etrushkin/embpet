#!/usr/bin/env python3
"""Repro for embpet-94q: dense console traffic right after a reboot must not trip the task
watchdog or lose/split a frame. Needs the board; run under the lock:

  scripts/hw.sh python tools/wdt_repro.py [-p PORT] [--flash BUILD_DIR] [-n RUNS] [-s SEC]
                                          [--cmd CMD] [--depth K] [--no-reboot] [--idle]
                                          [--capture TEXT]

Each run: `sys reboot`, reconnect, wait until the new boot answers, then send CMD (`sys
info`, `help` or `evt since 0`) back to back for SEC seconds, keeping K commands in flight
(default 1: the next command goes out as soon as the previous res arrives). Fails the run
on any task_wdt / panic text, any line starting with @@ that is not a frame, a frame glued
onto another line, a seq gap, a command without a res, or a boot id change during the loop.
Events replayed by `evt since 0` (an old seq again) are counted, not checked. Prints one
JSON summary per run and a total; exit 0 = every run passed.

--no-reboot loads the running boot instead. --idle sends nothing (a baseline). --capture
reports the console lines containing TEXT, e.g. the output of a test-only probe build.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from embpet.board import Board, new_rid  # noqa: E402
from embpet.frames import FrameChecker, build_line, parse_frame  # noqa: E402

BAD_TEXT = ("task_wdt", "Guru Meditation", "abort()", "Backtrace", "MEPC", "panic")
LOG_RE = re.compile(r"^[EWI] \(\d+\) ")
CMDS = {"sys info": (["sys", "info"], ()), "help": (["help"], ()),
        "evt since 0": (["evt", "since"], ("0",))}


def flash(port, build_dir):
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32c3", "-p", port, "-b", "460800",
           "--before", "default_reset", "--after", "hard_reset", "write_flash", "@flash_args"]
    r = subprocess.run(cmd, cwd=build_dir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        sys.stderr.write(r.stdout.decode("utf-8", "replace"))
        raise SystemExit("flash failed")
    time.sleep(3.0)


def one_run(b, seconds, capture, idle, depth, reboot, command):
    words, args = CMDS[command]
    old = b.cmd("sys info")["boot"]
    if reboot:
        assert b.cmd("sys reboot")["ok"] is True
        b.reconnect()
    else:
        old = None
        b.flush_input()
    chk = FrameChecker()
    bad_text = []
    logs = []
    captured = []
    replayed = 0

    def on_line(text):
        nonlocal replayed
        f = parse_frame(text)
        if (f is not None and f.get("t") == "evt" and isinstance(f.get("seq"), int) and
                f["seq"] <= chk.last.get(f.get("boot"), -1)):
            replayed += 1           # `evt since` re-emits old events with their seq
        else:
            chk.feed(text)
        if capture and capture in text:
            captured.append(text[:240])
        elif any(k in text for k in BAD_TEXT):
            bad_text.append(text[:160])
        elif LOG_RE.match(text) and len(logs) < 40:
            logs.append(text[:160])
    b.on_line = on_line
    t0 = time.monotonic()
    info = b.wait_ready(30.0)
    ready_after = time.monotonic() - t0
    boot = info["boot"]
    first_up = info["uptime_s"]
    sent = lost = 0
    max_lat = 0.0
    last_up = first_up
    end = time.monotonic() + seconds
    if idle:
        for _ in b.lines(seconds):
            pass
    outstanding = {}                # rid -> send time
    while time.monotonic() < end or outstanding:
        while time.monotonic() < end and len(outstanding) < depth:
            rid = new_rid()
            b.write_line(build_line(words, args, rid=rid))
            outstanding[rid] = time.monotonic()
            sent += 1
        got = None
        for f in b.frames(1.5):
            if f.get("t") == "res" and f.get("rid") in outstanding:
                got = f
                break
        if got is None:             # nothing answered in 1.5 s: count the oldest as lost
            oldest = min(outstanding, key=outstanding.get)
            del outstanding[oldest]
            lost += 1
            continue
        max_lat = max(max_lat, time.monotonic() - outstanding.pop(got["rid"]))
        if got.get("boot") != boot:
            bad_text.append("boot id changed: %r" % got.get("boot"))
            break
        if got.get("ok") is not True:
            bad_text.append("err res: %r" % got)
        last_up = got.get("uptime_s", last_up)
    if command != "sys info":
        last_up = b.cmd("sys info")["uptime_s"]
    b.on_line = None
    ok = (not bad_text and lost == 0 and chk.bad == 0 and chk.gaps == 0 and boot != old)
    return {"ok": ok, "cmd": command, "boot": boot, "ready_after_s": round(ready_after, 1),
            "uptime_first": first_up, "uptime_last": last_up, "sent": sent, "lost": lost,
            "rate_per_s": round(sent / seconds), "max_latency_ms": round(max_lat * 1000),
            "frames": chk.summary(), "replayed_evts": replayed,
            "frame_problems": chk.problems[:5],
            "bad_text": bad_text[:10], "logs": logs, "captured": captured}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port")
    ap.add_argument("--flash", metavar="BUILD_DIR",
                    help="write BUILD_DIR/flash_args first; this replaces the board's firmware "
                         "and the tool does not restore it")
    ap.add_argument("--cmd", choices=sorted(CMDS), default="sys info",
                    help="the command to flood with (default: sys info)")
    ap.add_argument("-n", "--runs", type=int, default=5)
    ap.add_argument("-s", "--seconds", type=float, default=15.0)
    ap.add_argument("--idle", action="store_true",
                    help="no traffic after the board answers (baseline for --capture)")
    ap.add_argument("--capture", metavar="TEXT",
                    help="also report every console line containing TEXT (test builds)")
    ap.add_argument("--depth", type=int, default=1,
                    help="commands kept outstanding (1 = wait for each res; README 3.2 allows "
                         "up to about 4 KB of input in flight)")
    ap.add_argument("--no-reboot", action="store_true",
                    help="load the running boot instead of rebooting first")
    a = ap.parse_args()
    b = Board(a.port)
    if a.flash:
        b.close()
        flash(b.port, a.flash)
        b = Board(a.port)
        b.wait_ready(30.0)
    results = []
    for k in range(a.runs):
        r = one_run(b, a.seconds, a.capture, a.idle, a.depth, not a.no_reboot, a.cmd)
        r["run"] = k + 1
        print(json.dumps(r), flush=True)
        results.append(r)
    passed = sum(1 for r in results if r["ok"])
    print(json.dumps({"runs": a.runs, "passed": passed}))
    b.close()
    sys.exit(0 if passed == a.runs else 1)


if __name__ == "__main__":
    main()
