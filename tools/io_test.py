#!/usr/bin/env python3
"""Device tests for the io component (embpet-9uk outputs, embpet-ayo inputs). Needs the
board running the TEST build (CONFIG_EMBPET_IO_TEST, components/io/iotest.defaults); run
under the board lock:

  scripts/hw.sh python tools/io_test.py [-p PORT] [--flash BUILD_DIR] outputs|inputs

--flash writes BUILD_DIR/flash_args with esptool first, so flash and test run under one
board lock.

outputs: error cases; io set/toggle/get; pulse width measured on-chip (an any-edge ISR
  timestamps the output pins with esp_timer_get_time, `io t_edges`); remaining_ms
  decreases; re-pulse restarts the timer; set/toggle cancel a pulse; boot levels survive a
  reboot.

inputs: the test build drives the input pins itself (open-drain, like a jumper to GND,
  `io t_drive`). Clean edge, 5-edge bounce, glitch then change, an edge storm, debounce 20
  and 0, the debounce setting in NVS, input events on all three pins, and an esp_timer-
  context proto_evt emitter run with the port closed in the middle (`io t_emit`), with the
  esp_timer task stack headroom (`io t_stat`).

Prints a JSON summary; exit 0 = all checks passed, 1 = a check failed, 2 = serial error.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from console_test import Board, Checks, flash  # noqa: E402
from embpet_cli import find_port  # noqa: E402

try:
    import serial
except ImportError:
    sys.exit("pyserial missing: run with scripts/idf.sh python ...")


class Dev:
    """Board plus a request-id counter."""

    def __init__(self, port, settle=3.0):
        self.port = port
        self.b = Board(port, settle=settle)
        self.n = 0

    def cmd(self, line, timeout=5.0):
        self.n += 1
        rid = "t%d" % self.n
        f, _ = self.b.cmd("%s -i %s" % (line, rid), rid, timeout=timeout)
        return f

    def edges(self, pin=None):
        """Drain the on-chip edge recorder: [(pin, level, t_us)], plus the raw level mask."""
        f = self.cmd("io t_edges")
        if not f or not f.get("ok"):
            return [], None, f
        e = [tuple(x) for x in f["edges"] if pin is None or x[0] == pin]
        return e, f["raw"], f

    def get(self):
        f = self.cmd("io get")
        return {o["pin"]: o for o in f["out"]} if f and f.get("ok") else {}

    def reboot(self):
        self.cmd("sys reboot")
        self.b.close()
        time.sleep(4.0)
        self.b = Board(self.port, settle=3.0)


def widths(edges, pin):
    """Consecutive-edge intervals in ms for one pin."""
    e = [x for x in edges if x[0] == pin]
    return [(b[2] - a[2]) / 1000.0 for a, b in zip(e, e[1:])]


def outputs(port):
    c = Checks()
    d = Dev(port)
    info = {}
    f = d.edges()[2]
    if not c.check("test build running (io test answers)", f and f.get("ok"), f):
        return c, info

    # clean slate: boot levels off, all outputs off
    for p in range(8):
        d.cmd("io boot %d off" % p)
    for p in range(8):
        d.cmd("io set %d off" % p)
    d.edges()

    g = d.get()
    c.check("io get: 8 pins, all off, none pulsing",
            len(g) == 8 and all(o["level"] == "off" and not o["pulsing"]
                                and o["remaining_ms"] == 0 for o in g.values()), g)

    # error cases
    for line, err, name in [
            ("io set 8 on", "bad_pin", "set pin 8 is not an output"),
            ("io set 9 on", "bad_pin", "set pin 9"),
            ("io set -1 on", "bad_args", "set pin -1 (looks like an option)"),
            ("io set 10 on", "bad_pin", "set pin 10 (input)"),
            ("io pulse 20", "bad_pin", "pulse pin 20"),
            ("io boot 21 on", "bad_pin", "boot pin 21"),
            ("io set 1 maybe", "bad_args", "set bad level"),
            ("io set 1", "bad_args", "set missing level"),
            ("io pulse 1 -d 0", "bad_args", "pulse -d 0"),
            ("io pulse 1 -d 600001", "bad_args", "pulse -d over max"),
            ("io pulse 1 -d x", "bad_args", "pulse -d not a number"),
            ("io boot 1", "bad_args", "boot pin without level"),
            ("io boot 1 toggle", "bad_args", "boot bad level")]:
        f = d.cmd(line)
        c.check("error: " + name, f and f.get("ok") is False and f.get("err") == err, f)

    # set / toggle (logical on = pin low)
    d.edges()
    f = d.cmd("io set 3 on")
    e, raw, _ = d.edges(3)
    c.check("set on: res level on, pin driven low",
            f and f.get("ok") and f.get("level") == "on" and f.get("pin") == 3
            and e and e[-1][1] == 0 and not raw & (1 << 3), [f, e, raw])
    f = d.cmd("io set 3 toggle")
    c.check("toggle on -> off", f and f.get("level") == "off", f)
    f = d.cmd("io set 3 toggle")
    c.check("toggle off -> on", f and f.get("level") == "on", f)
    f = d.cmd("io set 3 off")
    _, raw, _ = d.edges()
    c.check("set off: pin back high", f and f.get("level") == "off" and raw & (1 << 3), raw)
    g = d.get()
    c.check("get after set", g.get(3, {}).get("level") == "off", g.get(3))

    # pulse width, measured on the chip (95..100 ms default and others)
    meas = {}
    for ms, extra in [(None, ""), (100, " -d 100"), (20, " -d 20"), (250, " -d 250"),
                      (1, " -d 1"), (1000, " -d 1000")]:
        want = 100 if ms is None else ms
        ok_all = True
        ws = []
        for pin in (0, 2, 7):
            d.edges()
            f = d.cmd("io pulse %d%s" % (pin, extra))
            d.b.pump(want / 1000.0 + 0.4)
            e, _, _ = d.edges(pin)
            w = widths(e, pin)
            ws.append(round(w[0], 3) if w else None)
            ok_all = (ok_all and f and f.get("ok") and f.get("level") == "on"
                      and 0 < f.get("remaining_ms", 0) <= want and len(e) == 2
                      and e[0][1] == 0 and e[1][1] == 1 and abs(w[0] - want) <= 2.0)
        meas["%dms" % want] = ws
        c.check("pulse %d ms measured within +-2 ms on pins 0,2,7" % want, ok_all, ws)
    info["pulse_width_ms"] = meas

    # pulse from the on level goes off, then back on
    d.cmd("io set 4 on")
    d.edges()
    f = d.cmd("io pulse 4 -d 100")
    d.b.pump(0.5)
    e, _, _ = d.edges(4)
    w = widths(e, 4)
    c.check("pulse from on: res level off, high 100 ms, then low again",
            f and f.get("level") == "off" and len(e) == 2 and e[0][1] == 1 and e[1][1] == 0
            and abs(w[0] - 100) <= 2.0, [f, e])
    g = d.get()
    c.check("level restored to on after pulse", g[4]["level"] == "on" and not g[4]["pulsing"], g[4])
    d.cmd("io set 4 off")

    # remaining_ms decreases
    d.edges()
    f = d.cmd("io pulse 5 -d 3000")
    r0 = f.get("remaining_ms") if f else None
    g1 = d.get()[5]
    d.b.pump(1.0)
    g2 = d.get()[5]
    d.b.pump(1.0)
    g3 = d.get()[5]
    c.check("remaining_ms decreases while pulsing",
            r0 and r0 <= 3000 and g1["pulsing"] and g1["level"] == "on"
            and g1["remaining_ms"] > g2["remaining_ms"] > g3["remaining_ms"] > 0
            and 900 <= g1["remaining_ms"] - g2["remaining_ms"] <= 1600,
            [r0, g1["remaining_ms"], g2["remaining_ms"], g3["remaining_ms"]])
    d.b.pump(1.4)
    g4 = d.get()[5]
    e, _, _ = d.edges(5)
    w = widths(e, 5)
    c.check("pulse ended: restored, not pulsing, remaining 0",
            g4["level"] == "off" and not g4["pulsing"] and g4["remaining_ms"] == 0
            and len(e) == 2 and abs(w[0] - 3000) <= 3.0, [g4, w])

    # re-pulse restarts the timer and keeps the original restore level
    d.edges()
    # (the serial round trip takes a few hundred ms, so the pulses here are long)
    d.cmd("io pulse 6 -d 1500")
    d.b.pump(0.3)
    f = d.cmd("io pulse 6 -d 1500")
    c.check("re-pulse: still the pulse level, remaining restarted",
            f and f.get("level") == "on" and f.get("remaining_ms", 0) > 1480, f)
    d.b.pump(2.0)
    e, _, _ = d.edges(6)
    w = widths(e, 6)
    c.check("re-pulse: one longer pulse (1500 ms + the delay to the 2nd command), "
            "restored to off",
            len(e) == 2 and e[0][1] == 0 and e[1][1] == 1 and 1800 <= w[0] <= 3000
            and d.get()[6]["level"] == "off", [e, w])

    # set during a pulse cancels it (no later restore edge)
    d.edges()
    d.cmd("io pulse 1 -d 2000")
    d.b.pump(0.1)
    f = d.cmd("io set 1 on")
    d.b.pump(2.5)
    e, _, _ = d.edges(1)
    g = d.get()[1]
    c.check("set during pulse: stays on, not pulsing, no restore edge",
            f and f.get("level") == "on" and g["level"] == "on" and not g["pulsing"]
            and len(e) == 1 and e[0][1] == 0, [f, g, e])
    d.cmd("io set 1 off")

    # toggle during a pulse cancels it too
    d.edges()
    d.cmd("io pulse 2 -d 2000")
    d.b.pump(0.1)
    f = d.cmd("io set 2 toggle")
    d.b.pump(2.5)
    e, _, _ = d.edges(2)
    g = d.get()[2]
    c.check("toggle during pulse: off, not pulsing, no later edge",
            f and f.get("level") == "off" and g["level"] == "off" and not g["pulsing"]
            and len(e) == 2 and e[0][1] == 0 and e[1][1] == 1, [f, g, e])

    # boot levels persist and are applied after a reboot
    f = d.cmd("io boot 2 on")
    f2 = d.cmd("io boot 5 on")
    c.check("io boot set returns the map",
            f2 and f2.get("levels", {}).get("2") == "on" and f2["levels"].get("5") == "on"
            and f2["levels"].get("0") == "off" and len(f2["levels"]) == 8, f2)
    d.cmd("io boot 5 off")
    f = d.cmd("io boot")
    c.check("io boot lists", f and f["levels"]["2"] == "on" and f["levels"]["5"] == "off", f)
    d.cmd("io set 7 on")                    # runtime level must not survive the reboot
    d.reboot()
    g = d.get()
    _, raw, _ = d.edges()
    c.check("after reboot: pin 2 on (boot level), the others off",
            g and g[2]["level"] == "on" and all(g[p]["level"] == "off"
                                                for p in range(8) if p != 2), g)
    c.check("after reboot: pin 2 low, the others high on the pins",
            raw is not None and (raw & 0xFF) == 0xFB, raw)
    d.cmd("io boot 2 off")
    f = d.cmd("io boot")
    c.check("boot levels restored to off", f and all(v == "off" for v in f["levels"].values()), f)
    d.cmd("io set 2 off")
    d.b.close()
    return c, info


def input_events(d, i0, pin):
    return [x for x in d.b.frames[i0:]
            if x.get("t") == "evt" and x.get("ev") == "input" and x.get("pin") == pin]


def drive(d, pin, pattern, wait=0.3):
    """Run a drive pattern on the input pin; return the input events it caused."""
    i0 = len(d.b.frames)
    f = d.cmd("io t_drive %d %s" % (pin, pattern))
    d.b.pump(wait)
    return f, input_events(d, i0, pin)


def ex(events):
    return [(e["active"], e["extra"]) for e in events]


def in_state(d):
    f = d.cmd("io in")
    return {o["pin"]: o for o in f["in"]} if f and f.get("ok") else {}


def inputs(port):
    c = Checks()
    d = Dev(port)
    info = {}
    f = d.edges()[2]
    if not c.check("test build running (io t_edges answers)", f and f.get("ok"), f):
        return c, info

    # debounce setting: show, set, errors
    for p in (10, 20, 21):
        d.cmd("io debounce %d 20" % p)
    f = d.cmd("io debounce")
    c.check("io debounce lists the three inputs",
            f and f.get("debounce") == {"10": 20, "20": 20, "21": 20}, f)
    for line, err, name in [("io debounce 9 5", "bad_pin", "pin 9"),
                            ("io debounce 0 5", "bad_pin", "pin 0 (an output)"),
                            ("io debounce 22 5", "bad_pin", "pin 22"),
                            ("io debounce 10", "bad_args", "pin without ms"),
                            ("io debounce 10 1001", "bad_args", "ms 1001"),
                            ("io debounce 10 x", "bad_args", "ms not a number")]:
        f = d.cmd(line)
        c.check("error: " + name, f and f.get("ok") is False and f.get("err") == err, f)
    f = d.cmd("io debounce 10 -1")
    c.check("error: ms -1", f and f.get("ok") is False and f.get("err") == "bad_args", f)

    g = in_state(d)
    c.check("io in: three inputs, released (pull-up), active 0, extra 0",
            sorted(g) == [10, 20, 21] and all(o["active"] == 0 and o["level"] == 1
                                              and o["extra"] == 0 for o in g.values()), g)

    # debounce 20 ms
    f, ev = drive(d, 10, "L1000")
    c.check("clean press: one event, active 1, extra 0", ex(ev) == [(1, 0)] and f["level"] == 0,
            [f, ev])
    g = in_state(d)[10]
    c.check("io in after press: active 1, level 0, extra 0",
            (g["active"], g["level"], g["extra"]) == (1, 0, 0), g)
    f, ev = drive(d, 10, "H1000")
    c.check("clean release: one event, active 0, extra 0", ex(ev) == [(0, 0)], ev)
    c.check("input event has pin/active/extra and the common fields",
            ev and all(k in ev[0] for k in ("t", "seq", "boot", "ev", "pin", "active", "extra")),
            ev)

    f, ev = drive(d, 10, "L200H200L200H200L200")
    c.check("5-edge bounce press: ONE event, extra 4", ex(ev) == [(1, 4)], ev)
    f, ev = drive(d, 10, "H200L200H200L200H200")
    c.check("5-edge bounce release: ONE event, extra 4", ex(ev) == [(0, 4)], ev)
    f, ev = drive(d, 10, "L200H200L200H200")
    c.check("4-edge bounce that ends released: no event", ev == [], ev)
    g = in_state(d)[10]
    c.check("io in: 4 pending edges, still inactive", (g["active"], g["extra"]) == (0, 4), g)
    f, ev = drive(d, 10, "L1000")
    c.check("press after the glitch: extra includes the 4 glitch edges",
            ex(ev) == [(1, 4)], ev)
    drive(d, 10, "H1000")

    f, ev = drive(d, 10, "L300H1000")
    c.check("glitch shorter than the debounce: no event", ev == [], ev)
    g = in_state(d)[10]
    c.check("io in after glitch: inactive, extra 2", (g["active"], g["extra"]) == (0, 2), g)
    f, ev = drive(d, 10, "L1000")
    c.check("change after a glitch: extra 2 (the glitch edges)", ex(ev) == [(1, 2)], ev)
    f, ev = drive(d, 10, "H1000")
    c.check("next event is per-event, extra back to 0", ex(ev) == [(0, 0)], ev)

    # press shorter than the debounce time vs. a long one
    f, ev = drive(d, 10, "L10000H1000")
    c.check("press shorter than 20 ms (10 ms): no event", ev == [], ev)
    d.cmd("io debounce 10 100")
    f, ev = drive(d, 10, "L50000H1000", wait=0.4)
    c.check("debounce 100: a 50 ms press is ignored", ev == [], ev)
    f, ev = drive(d, 10, "L200000", wait=0.5)
    c.check("debounce 100: a 200 ms press is reported, extra = the 4 edges of the two "
            "ignored pulses", ex(ev) == [(1, 4)], ev)
    drive(d, 10, "H1000", wait=0.4)
    d.cmd("io debounce 10 20")

    # edge storm: 201 edges in about 20 ms; the ISR re-arms at most once per ms
    f, ev = drive(d, 10, "L100H100" * 100 + "L100")
    c.check("edge storm (201 edges in 20 ms): one event, extra 200",
            ex(ev) == [(1, 200)], ev)
    f = d.cmd("io in")
    c.check("console still answers after the storm", f and f.get("ok"), f)
    drive(d, 10, "H1000")

    # debounce 0
    d.cmd("io debounce 10 0")
    f, ev = drive(d, 10, "L1000")
    c.check("debounce 0: clean press is reported at once, extra 0", ex(ev) == [(1, 0)], ev)
    drive(d, 10, "H1000")
    i0 = len(d.b.frames)
    drive(d, 10, "L500H500L500H500L500")
    ev = input_events(d, i0, 10)
    pend = in_state(d)[10]["extra"]
    c.check("debounce 0, 5 slow edges: edges conserved (sum(extra+1) + pending == 5)",
            sum(e["extra"] + 1 for e in ev) + pend == 5 and ev and ev[-1]["active"] == 1,
            [ex(ev), pend])
    info["debounce0_slow_bounce"] = ex(ev)
    drive(d, 10, "H1000")
    f, ev = drive(d, 10, "L100H100L100H100L100")
    c.check("debounce 0: 5-edge bounce inside 1 ms folds into ONE event, extra 4",
            ex(ev) == [(1, 4)], ev)
    drive(d, 10, "H1000")
    i0 = len(d.b.frames)
    drive(d, 10, "L10H10L10H10L10H10L10H10L10")
    ev = input_events(d, i0, 10)
    g = in_state(d)[10]
    c.check("debounce 0, fast bounce: settles on the real level, edges counted",
            ev and ev[-1]["active"] == 1 and g["active"] == 1
            and sum(e["extra"] + 1 for e in ev) + g["extra"] >= 1, [ex(ev), g])
    info["debounce0_fast_bounce"] = ex(ev)
    drive(d, 10, "H1000")
    i0 = len(d.b.frames)                    # about 1 us apart: edges merge in the GPIO latch
    drive(d, 10, "L1H1L1H1L1H1L1H1L1")
    ev = input_events(d, i0, 10)
    info["debounce0_1us_bounce"] = {"events": ex(ev), "pending": in_state(d)[10]["extra"]}
    drive(d, 10, "H1000")
    d.cmd("io debounce 10 20")

    # all three pins report their own events
    for p in (20, 21):
        f, ev = drive(d, p, "L1000")
        c.check("pin %d press: event active 1 extra 0" % p, ex(ev) == [(1, 0)], ev)
        f, ev = drive(d, p, "H1000")
        c.check("pin %d release: event active 0 extra 0" % p, ex(ev) == [(0, 0)], ev)
    c.check("driving a non-input is bad_pin",
            (d.cmd("io t_drive 5 L100") or {}).get("err") == "bad_pin")

    # debounce persists in NVS
    d.cmd("io debounce 21 77")
    d.reboot()
    f = d.cmd("io debounce")
    c.check("debounce survives a reboot", f and f.get("debounce") == {"10": 20, "20": 20,
                                                                      "21": 77}, f)
    d.cmd("io debounce 21 20")
    f = d.cmd("io debounce")
    c.check("debounce back to 20", f and f["debounce"] == {"10": 20, "20": 20, "21": 20}, f)

    # esp_timer-context emitter: proto_evt from a timer callback, with the port closed in
    # the middle (the writer stalls ~50 ms once), plus input events from the same task
    f = d.cmd("io t_emit 250 20")
    c.check("emitter started", f and f.get("ok"), f)
    d.b.pump(1.0)
    d.b.close()
    time.sleep(2.5)
    d.b = Board(port, settle=2.0)
    f, ev = drive(d, 10, "L1000", wait=0.5)
    c.check("input event while the emitter runs (same esp_timer task)", ex(ev) == [(1, 0)], ev)
    drive(d, 10, "H1000", wait=0.2)
    d.b.pump(3.0)
    st = d.cmd("io t_stat")
    c.check("emitter finished all 250 events", st and st.get("emitted") == 250
            and st.get("running") is False, st)
    c.check("longest proto_evt from the timer task < 120 ms (one host stall at most)",
            st and 0 < st.get("max_evt_us", 0) < 120000, st)
    c.check("esp_timer task stack: at least 512 bytes never used",
            st and st.get("timer_stack_free_min", 0) >= 512, st)
    f = d.cmd("evt since 0")
    last = [x for x in d.b.frames if x.get("ev") == "iotest"]
    c.check("last emitted event is in the replay/live stream",
            any(x.get("n") == 250 for x in last), [x.get("n") for x in last][-3:])
    info["emitter"] = {k: st.get(k) for k in ("emitted", "max_evt_us", "timer_stack",
                                              "timer_stack_free_min")} if st else None
    d.b.close()
    return c, info


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-p", "--port")
    ap.add_argument("--flash", metavar="BUILD_DIR")
    sub = ap.add_subparsers(dest="mode", required=True)
    sub.add_parser("outputs")
    sub.add_parser("inputs")
    a = ap.parse_args()
    port = a.port or find_port()
    try:
        if a.flash:
            flash(port, a.flash)
        c, info = {"outputs": outputs, "inputs": inputs}[a.mode](port)
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
