#!/usr/bin/env python3
"""Long soak of the PRODUCTION firmware (embpet-dk1). Run under the board lock, in the
background, and leave it alone:

  scripts/hw.sh python tools/soak24.py --hours 24 --log <dir>/soak24.jsonl [-p PORT]

Needs only python3 + pyserial and tools/embpet (no ESP-IDF), e.g. on a separate host:

  flock /tmp/embpet-hw.lock .venv/bin/python tools/soak24.py -p /dev/ttyACM0 --hours 24 \
      --log soak24.jsonl

Every --period seconds (default 180) it records, as one JSON line in --log:
  sys info (uptime, heap_free, heap_min, reset_reason, log_dropped, boot id), wifi status
  (state, rssi, txpower), tg status (state, detail, pending, owner set), and it toggles
  outputs (`io set`, `io pulse` on pins 0, 1, 3, 4, 5, 6: not the strapping pins 2/8/9 and
  not 7), reads `io get` / `io in`, and runs `tg pending` and `tg ack --older 0`.
Every --closed-every seconds (default 1800) it closes the port for --closed-for seconds
(default 600), reopens it with Ctrl-U session start (tools/embpet.Board does this) and runs
`evt since <last seq>` (`evt since 0` if the board reset meanwhile). Nothing is ever sent to
Telegram and no secret is read or printed. Because every cycle acks all pending messages, it
refuses (exit 2) a board with an owner or pending messages unless --allow-side-effects; it
also refuses an existing non-empty --log, so each run has its own log.

Every evt frame, live or replayed, is counted once (by boot and seq); wifi, tg and boot
events are logged as "event" records, so a Wi-Fi drop or Telegram outage between two samples
is not missed. Serial errors (unplugged, port gone, no answer) never stop it: the port is
reopened, the reopens are counted, and a boot id or reset_reason change is logged as a
"reset" record (the baseline then follows the new boot). An unexpected exception in the tool
itself is counted apart as "exceptions" (logged with repr). At the end (deadline, SIGINT or
SIGTERM) it writes a final {"summary": ...} line to the log, to <log>.summary.json and to
stdout. The summary has: samples, resets, serial errors, reopens and planned port closes,
network events (wifi_drops, tg_offline, and every event kind as "<ev>:<state>"), command
failures, heap_free min/max and trend (median of the first vs the last quarter, and the
longest run of consecutive declines), heap_min first/last/lowest, uptime continuity,
log_dropped. If an owner is paired during the run (without --allow-side-effects), it stops
before that cycle acks anything. Exit 0 = ran to the end or was stopped by a signal, 1 = the
board never answered before the deadline, 2 = refused (at the start: tg status unanswered,
an owner or pending messages, and no log is left behind; or an owner paired mid-run).
"""
import argparse
import json
import os
import signal
import statistics
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from embpet import Board, CmdTimeout, NoBoard, find_port  # noqa: E402

OUT_PINS = [0, 1, 3, 4, 5, 6]      # 2 (usable, but a strapping pin) and 7 are left alone


class Soak:
    def __init__(self, a):
        self.a = a
        self.port = a.port or find_port()
        self.b = None
        self.log = open(a.log, "a", buffering=1, encoding="utf-8")
        self.t0 = time.monotonic()
        self.end = self.t0 + a.hours * 3600.0
        self.stop = False
        self.refused = None          # "start" (nothing logged) or "mid-run" (owner paired)
        self.base_boot = None
        self.base_reason = None
        self.last_seq = 0
        self.last_uptime = None
        self.samples = []            # (t, heap_free, heap_min, uptime, log_dropped)
        self.n = {"samples": 0, "resets": 0, "serial_errors": 0, "exceptions": 0,
                  "reopens": 0, "planned_closes": 0, "cmd_timeouts": 0, "cmd_failures": 0,
                  "uptime_regressions": 0, "evt_replays": 0, "evt_truncated": 0,
                  "evt_replay_new_boot": 0}
        self.errors = {}             # distinct command failures, for the summary
        self.events = {}             # "<ev>" or "<ev>:<state>" -> count, from evt frames
        self.seen_evt = set()        # (boot, seq): live and replayed events counted once

    # --- logging
    def rec(self, **kw):
        kw["t"] = round(time.monotonic() - self.t0, 1)
        kw["ts"] = time.strftime("%Y-%m-%dT%H:%M:%S")
        self.log.write(json.dumps(kw, ensure_ascii=False) + "\n")

    # --- port handling
    def open(self, why):
        """Open the port, retrying until it works or we are told to stop."""
        t = time.monotonic()
        while not self.stop and time.monotonic() < self.end:
            try:
                if self.port is None or not os.path.exists(self.port):
                    self.port = find_port()
                self.b = Board(self.port)
                self.b.on_event = self.on_event
                self.n["reopens"] += 1
                self.rec(ev="open", why=why, waited_s=round(time.monotonic() - t, 1))
                return True
            except (NoBoard, OSError) as e:
                self.b = None
                self.rec(ev="open_failed", why=why, err=type(e).__name__)
                self.sleep(5.0)
        return False

    def on_event(self, f):
        """Count every evt frame once (a replay repeats live ones); log wifi and tg changes,
        which are the network reconnects the 180 s snapshots could miss."""
        key = (f.get("boot"), f.get("seq"))
        if key in self.seen_evt:
            return
        self.seen_evt.add(key)
        ev = f.get("ev")
        name = "%s:%s" % (ev, f["state"]) if "state" in f else str(ev)
        self.events[name] = self.events.get(name, 0) + 1
        if ev in ("wifi", "tg", "boot"):
            self.rec(ev="event", evt=ev, state=f.get("state"), detail=f.get("detail"),
                     rssi=f.get("rssi"), seq=f.get("seq"), boot=f.get("boot"))

    def close(self):
        if self.b is not None:
            try:
                self.b.close()
            except OSError:
                pass
            self.b = None

    def sleep(self, s):
        end = time.monotonic() + s
        while not self.stop and time.monotonic() < end:
            time.sleep(min(1.0, max(0.0, end - time.monotonic())))

    def cmd(self, command, *args, timeout=15.0):
        """One command; None on a timeout or a command failure (counted), SerialLost on a
        serial error."""
        try:
            r = self.b.cmd(command, *args, timeout=timeout)
        except CmdTimeout:
            self.n["cmd_timeouts"] += 1
            self.rec(ev="cmd_timeout", cmd=command)
            return None
        except (OSError, NoBoard) as e:
            raise SerialLost(type(e).__name__)
        if r.get("seq"):
            self.last_seq = r["seq"]        # per-boot counter; a reset is logged separately
        if not r.get("ok"):
            self.n["cmd_failures"] += 1
            key = "%s:%s" % (command, r.get("err"))
            self.errors.setdefault(key, 0)
            self.errors[key] += 1
        return r

    # --- one sampling cycle
    def cycle(self):
        si = self.cmd("sys info")
        wf = self.cmd("wifi status")
        tg = self.cmd("tg status")
        rec = {"ev": "sample"}
        if si:
            boot, reason = si.get("boot"), si.get("reset_reason")
            if self.base_boot is None:
                self.base_boot, self.base_reason = boot, reason
                self.last_seq = si.get("seq", 0)
                self.rec(ev="baseline", boot=boot, reset_reason=reason, fw=si.get("fw"),
                         uptime_s=si.get("uptime_s"))
            elif boot != self.base_boot or reason != self.base_reason:
                self.n["resets"] += 1
                self.rec(ev="reset", old_boot=self.base_boot, boot=boot,
                         old_reason=self.base_reason, reset_reason=reason,
                         uptime_s=si.get("uptime_s"))
                self.base_boot, self.base_reason = boot, reason
                self.last_uptime = None
                self.last_seq = si.get("seq", 0)
            up = si.get("uptime_s")
            if self.last_uptime is not None and up is not None and up < self.last_uptime:
                self.n["uptime_regressions"] += 1
            self.last_uptime = up
            rec.update(boot=boot, reset_reason=reason, uptime_s=up,
                       heap_free=si.get("heap_free"), heap_min=si.get("heap_min"),
                       log_dropped=si.get("log_dropped"))
            self.samples.append((time.monotonic() - self.t0, si.get("heap_free"),
                                 si.get("heap_min"), up, si.get("log_dropped")))
            self.n["samples"] += 1
        if wf:
            rec.update(wifi=wf.get("state"), rssi=wf.get("rssi"), txpower=wf.get("txpower"))
        if tg:
            rec.update(tg=tg.get("state"), tg_detail=tg.get("detail"),
                       pending=tg.get("pending"), owner_set=tg.get("owner") is not None)
            if tg.get("owner") is not None and not self.a.allow_side_effects:
                # paired during the run: stop before the cycle acks the owner's messages
                rec["refused"] = "owner paired during the soak"
                self.rec(**rec)
                self.refused, self.stop = "mid-run", True
                return
        # outputs: set on/off, one pulse, readbacks
        for pin in OUT_PINS[:3]:
            self.cmd("io set", str(pin), "on")
        g = self.cmd("io get")
        on = [o.get("pin") for o in (g or {}).get("out", []) if o.get("level") == "on"]
        for pin in OUT_PINS[:3]:
            self.cmd("io set", str(pin), "off")
        self.cmd("io pulse", str(OUT_PINS[3]), "-d", "200")
        self.cmd("io pulse", str(OUT_PINS[4]), "-d", "50")
        self.cmd("io set", str(OUT_PINS[5]), "toggle")
        self.cmd("io set", str(OUT_PINS[5]), "toggle")
        self.cmd("io in")
        rec["outs_on_seen"] = on
        p = self.cmd("tg pending")
        a = self.cmd("tg ack", "--older", "0")
        rec["pending_items"] = len((p or {}).get("items") or [])
        rec["acked"] = (a or {}).get("acked")
        self.rec(**rec)

    def closed_period(self):
        """Close the port, wait, reopen with Ctrl-U session start and replay events."""
        self.n["planned_closes"] += 1
        seq = self.last_seq
        self.close()
        self.rec(ev="port_closed", for_s=self.a.closed_for, after_seq=seq)
        self.sleep(self.a.closed_for)
        if not self.open("after planned close"):
            return
        r = self.cmd("evt since", str(seq))
        if r and self.base_boot is not None and r.get("boot") != self.base_boot:
            # reset while closed: seq restarted, so replay the whole new boot (the next
            # cycle logs the reset itself)
            self.n["evt_replay_new_boot"] += 1
            seq = 0
            r = self.cmd("evt since", "0")
        self.n["evt_replays"] += 1
        if r and r.get("truncated"):
            self.n["evt_truncated"] += 1
        self.rec(ev="evt_since", since=seq, ok=bool(r and r.get("ok")),
                 count=(r or {}).get("count"), truncated=(r or {}).get("truncated"),
                 boot=(r or {}).get("boot"))

    def side_effects_ok(self):
        """The cycle acks every pending message and switches outputs 0, 1 and 3-6: refuse
        a board with an owner or pending messages unless --allow-side-effects."""
        if self.a.allow_side_effects:
            return True
        tg = self.cmd("tg status")
        if tg is None or not tg.get("ok"):
            print("refusing: tg status did not answer, so owner and pending are unknown",
                  file=sys.stderr)
        elif tg.get("owner") is None and not tg.get("pending"):
            return True
        else:
            print("refusing: the board has an owner or pending messages, which every cycle "
                  "would ack (tg ack --older 0); pass --allow-side-effects to soak it anyway",
                  file=sys.stderr)
        self.refused = "start"
        return False

    def run(self):
        a = self.a
        end = self.end
        self.rec(ev="start", hours=a.hours, period_s=a.period, closed_every_s=a.closed_every,
                 closed_for_s=a.closed_for, port=self.port)
        if not self.open("start"):
            return 0 if self.stop else 1
        try:
            if not self.side_effects_ok():
                return 2
        except SerialLost as e:
            self.rec(ev="serial_error", err=str(e))
            return 0 if self.stop else 1
        next_close = time.monotonic() + a.closed_every
        while not self.stop and time.monotonic() < end:
            t = time.monotonic()
            try:
                self.cycle()
                if time.monotonic() >= next_close and time.monotonic() + a.closed_for < end:
                    self.closed_period()
                    next_close = time.monotonic() + a.closed_every
            except SerialLost as e:
                self.n["serial_errors"] += 1
                self.rec(ev="serial_error", err=str(e))
                self.close()
                self.sleep(3.0)
                self.open("after serial error")
                continue
            except Exception as e:          # keep the soak alive on anything unexpected
                self.n["exceptions"] += 1
                self.rec(ev="exception", err=repr(e))
                self.close()
                self.sleep(3.0)
                self.open("after exception")
                continue
            self.sleep(max(1.0, a.period - (time.monotonic() - t)))
        return 2 if self.refused else 0

    # --- summary
    def summary(self):
        s = self.samples
        heaps = [x[1] for x in s if x[1] is not None]
        mins = [x[2] for x in s if x[2] is not None]
        out = {"elapsed_h": round((time.monotonic() - self.t0) / 3600.0, 2), **self.n,
               "boot": self.base_boot, "reset_reason": self.base_reason,
               "command_failure_kinds": self.errors, "events": self.events,
               "wifi_drops": self.events.get("wifi:down", 0),
               "tg_offline": self.events.get("tg:offline", 0)}
        if heaps:
            q = max(1, len(heaps) // 4)
            run = best = 0
            for p, q2 in zip(heaps, heaps[1:]):
                run = run + 1 if q2 < p else 0
                best = max(best, run)
            out["heap_free"] = {"min": min(heaps), "max": max(heaps),
                                "first_quarter_median": statistics.median(heaps[:q]),
                                "last_quarter_median": statistics.median(heaps[-q:]),
                                "longest_decline_run": best, "samples": len(heaps)}
        if mins:
            out["heap_min"] = {"first": mins[0], "last": mins[-1], "lowest": min(mins)}
        ld = [x[4] for x in s if x[4] is not None]
        if ld:
            out["log_dropped"] = {"first": ld[0], "last": ld[-1]}
        ups = [x[3] for x in s if x[3] is not None]
        if ups:
            out["uptime_s"] = {"first": ups[0], "last": ups[-1]}
        return out


class SerialLost(Exception):
    pass


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-p", "--port")
    ap.add_argument("--hours", type=float, default=24.0)
    ap.add_argument("--log", required=True)
    ap.add_argument("--period", type=float, default=180.0, metavar="S")
    ap.add_argument("--closed-every", type=float, default=1800.0, metavar="S")
    ap.add_argument("--closed-for", type=float, default=600.0, metavar="S")
    ap.add_argument("--allow-side-effects", action="store_true",
                    help="soak a board with an owner or pending messages (they get acked)")
    a = ap.parse_args()
    if os.path.exists(a.log) and os.path.getsize(a.log) > 0:
        print("%s exists: give each run its own log" % a.log, file=sys.stderr)
        return 2
    soak = Soak(a)

    def on_signal(signum, frame):
        soak.stop = True
    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)
    rc = 1
    try:
        rc = soak.run()
    finally:
        soak.close()
        if soak.refused == "start":
            # nothing ran: leave no log, so the retry (--allow-side-effects) may use it
            soak.log.close()
            os.remove(a.log)
        else:
            summ = soak.summary()
            soak.rec(summary=summ)
            soak.log.close()
            with open(a.log + ".summary.json", "w", encoding="utf-8") as f:
                json.dump(summ, f, indent=1)
            print(json.dumps(summ))
    return rc


if __name__ == "__main__":
    sys.exit(main())
