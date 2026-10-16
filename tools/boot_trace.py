#!/usr/bin/env python3
"""Boot stall trace (embpet-s2j.5). Needs the trace firmware (components/proto/
boottrace.defaults); run under the board lock:

  scripts/hw.sh python tools/boot_trace.py [-p PORT] [--flash BUILD_DIR] [--runs N]
      [--pulse PIN:MS]

Each run resets the chip, waits for uptime 21 s (the trace stops at 20 s), reads every
`trace` record and the newest soak event (boot_max_*), and prints the records in time
order: start (ms of uptime), kind, duration (ms; the event id for wifi_evt/ip_evt) and
task.

--pulse needs the io test firmware as well (components/io/iotest.defaults): from the
first answer until uptime PULSE_UNTIL_S it sends `io pulse PIN -d MS` back to back, then
reads the on-chip edge timestamps (`io t_edges`) and prints each pulse's start and
measured width (ms).

Prints a JSON summary at the end; exit 0, or 2 on a serial error.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from console_test import flash  # noqa: E402
from embpet.board import Board, CmdTimeout  # noqa: E402
from embpet.serialport import find_port  # noqa: E402

TRACE_END_S = 21
PULSE_UNTIL_S = 7


def read_trace(b):
    recs = []
    while True:
        f = b.cmd("trace", str(len(recs)))
        if not f.get("ok"):
            raise SystemExit("trace failed: %s" % f)
        recs += f["recs"]
        if not f["recs"] or len(recs) >= f["n"]:
            return recs, f["lost"]


def pulse_widths(b, pin, ms):
    """[start ms, width ms] of each pulse sent until uptime PULSE_UNTIL_S."""
    b.cmd("io t_edges")                             # drop older edges
    while b.cmd("sys info").get("uptime_s", 0) < PULSE_UNTIL_S:
        b.cmd("io pulse", str(pin), "-d", str(ms))
        time.sleep((ms + 50) / 1000.0)
    edges = [e for e in b.cmd("io t_edges")["edges"] if e[0] == pin]
    # on = LOW (sink wiring): a pulse from off is a falling then a rising edge
    return [[round(e0[2] / 1000.0, 1), round((e1[2] - e0[2]) / 1000.0, 1)]
            for e0, e1 in zip(edges, edges[1:]) if e0[1] == 0 and e1[1] == 1]


def one_run(port, pulse):
    b = Board(port, reset=True)
    try:
        info = b.wait_ready(timeout=20.0)
        t_ready = info.get("uptime_s")
        widths = pulse_widths(b, *pulse) if pulse else []
        while b.cmd("sys info").get("uptime_s", 0) < TRACE_END_S:
            time.sleep(0.5)
        recs, lost = read_trace(b)
        soak = None
        for f in b.events(0.5):                     # the newest soak event wins
            if f.get("ev") == "soak":
                soak = {k: v for k, v in f.items() if k.startswith("boot_max")}
        return {"ready_uptime_s": t_ready, "recs": sorted(recs, key=lambda r: r[0]),
                "lost": lost, "soak": soak, "pulses": widths,
                "pulse_max_ms": max((w[1] for w in widths), default=None)}
    finally:
        b.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-p", "--port")
    ap.add_argument("--flash", metavar="BUILD_DIR")
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--pulse", metavar="PIN:MS")
    a = ap.parse_args()
    port = a.port or find_port()
    if not port:
        raise SystemExit("no board found")
    pulse = tuple(int(x) for x in a.pulse.split(":")) if a.pulse else None
    if a.flash:
        flash(port, a.flash)
    runs = []
    try:
        for i in range(a.runs):
            r = one_run(port, pulse)
            runs.append(r)
            print("== run %d (console ready at uptime %s s, %d records, %d lost)"
                  % (i + 1, r["ready_uptime_s"], len(r["recs"]), r["lost"]))
            for t_us, kind, dur, who in r["recs"]:
                d = "%d" % dur if kind.endswith("_evt") else "%.1f" % (dur / 1000.0)
                print("%9.1f  %-10s %8s  %s" % (t_us / 1000.0, kind, d, who))
            print("soak", json.dumps(r["soak"]))
            if r["pulses"]:
                print("pulses [start ms, width ms]", json.dumps(r["pulses"]))
    except (CmdTimeout, OSError) as e:
        print("serial error: %s" % e, file=sys.stderr)
        sys.exit(2)
    print(json.dumps({"runs": [{k: v for k, v in r.items() if k not in ("recs", "pulses")}
                               for r in runs]}))


if __name__ == "__main__":
    main()
