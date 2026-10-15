#!/usr/bin/env python3
"""Console CPU per command (embpet-s2j.4). Needs the probe test build
(components/proto/probe.defaults: `probe cpu` returns the console and idle tasks' run-time
counters in us, `probe nop` answers ok with no payload, the fixed cost of a command); run
under the lock:

  scripts/hw.sh python tools/console_cpu.py [-p PORT] [--flash BUILD_DIR] [-n N]
                                            [--settle SEC]

Waits until the uptime is at least SEC (default 20, past the boot TLS handshakes), then for
each of `probe nop`, `sys info`, `help` and `evt since 0`: reads `probe cpu`, sends N commands lockstep,
reads `probe cpu` again, and prints console CPU per command, the console and idle shares of
that stretch, and the host round-trip time. It also prints the line-ending statistics of
the raw bytes of one `help` exchange (echo, help text, frame, prompt), so builds can be
compared byte for byte: every LF must come as CRLF.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from embpet.board import Board, new_rid  # noqa: E402
from embpet.frames import build_line  # noqa: E402
from wdt_repro import CMDS, flash  # noqa: E402

CMDS = dict(CMDS, **{"probe nop": (["probe", "nop"], ())})


def probe(b):
    f = b.cmd("probe cpu")
    assert f["ok"] is True, f
    return f["console_us"], f["idle_us"], f["total_us"]


def measure(b, command, n):
    words, args = CMDS[command]
    c0, i0, t0 = probe(b)
    lat = []
    for _ in range(n):
        rid = new_rid()
        s = time.monotonic()
        b.write_line(build_line(words, args, rid=rid))
        for f in b.frames(5.0):
            if f.get("t") == "res" and f.get("rid") == rid:
                assert f.get("ok") is True, f
                break
        else:
            raise SystemExit("no res for %s" % command)
        lat.append(time.monotonic() - s)
    c1, i1, t1 = probe(b)
    dt = t1 - t0
    lat.sort()
    return {"cmd": command, "n": n, "console_us_per_cmd": round((c1 - c0) / n),
            "console_share": round((c1 - c0) / dt, 3), "idle_share": round((i1 - i0) / dt, 3),
            "rtt_ms_median": round(lat[len(lat) // 2] * 1000, 2),
            "rtt_ms_max": round(lat[-1] * 1000, 2)}


def line_endings(b):
    b.flush_input()
    b.write_line(build_line(["help"], (), rid="crlf"))
    raw = b""
    end = time.monotonic() + 2.0
    while time.monotonic() < end:
        raw += b.read_raw()
        if b'"rid":"crlf"' in raw and raw.endswith(b"embpet> "):
            break
    lf = raw.count(b"\n")
    crlf = raw.count(b"\r\n")
    return {"bytes": len(raw), "lf": lf, "crlf": crlf, "bare_lf": lf - crlf,
            "bare_cr": raw.count(b"\r") - crlf, "head": repr(raw[:40]), "tail": repr(raw[-60:])}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port")
    ap.add_argument("--flash", metavar="BUILD_DIR",
                    help="write BUILD_DIR/flash_args first; this replaces the board's firmware "
                         "and the tool does not restore it")
    ap.add_argument("-n", type=int, default=200, help="commands per measurement")
    ap.add_argument("--settle", type=float, default=20.0, help="minimum uptime (s)")
    a = ap.parse_args()
    b = Board(a.port)
    if a.flash:
        b.close()
        flash(b.port, a.flash)
        b = Board(a.port)
    up = b.wait_ready(30.0)["uptime_s"]
    if up < a.settle:
        time.sleep(a.settle - up)
    b.flush_input()
    for command in ("probe nop", "sys info", "help", "evt since 0"):
        print(json.dumps(measure(b, command, a.n)), flush=True)
    print(json.dumps({"line_endings": line_endings(b)}), flush=True)
    b.close()


if __name__ == "__main__":
    main()
