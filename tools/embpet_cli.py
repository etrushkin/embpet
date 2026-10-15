#!/usr/bin/env python3
"""Send one CLI command to an embpet board and print its @@ response frame.

Usage:
  embpet_cli.py [-p PORT] [-t SEC] [-a ACTOR] [--show-events] [--raw] CMD [ARG ...]

Each ARG is quoted and escaped for the embpet console (README 3.2), so values with
spaces or quotes are safe:  embpet_cli.py wifi set "my ssid" 'p"ss'

Prints the response JSON on stdout. Exit codes: 0 = ok:true, 1 = ok:false,
2 = timeout / serial error. See README.md section 3 for the protocol.

Passthrough mode (no command is sent):
  embpet_cli.py [-p PORT] --monitor SEC [--reset]

Prints the raw serial output for SEC seconds and exits 0. With --reset the chip
is reset first (RTS pulse on USB-Serial-JTAG), so the 2nd-stage bootloader log
and the app banner are captured. Use it where `idf.py monitor` has no TTY.

Frame check (no command is sent):
  embpet_cli.py [-p PORT] --check-frames SEC [--reset]

Reads SEC seconds, parses every `@@` line as JSON and checks that seq is strictly
increasing per boot id. A mid-line '@@' is bad only if the rest of the line parses as a
frame (t and seq); other '@@' text is counted in embedded_text. Prints a JSON summary
and up to 5 bad lines to stderr; exit 0 iff bad == 0, gaps == 0 and frames > 0.
Offline test: scripts/idf.sh python tools/test_embpet_cli.py
"""
import argparse
import glob
import json
import random
import string
import sys
import time

try:
    import serial  # pyserial (bundled with the ESP-IDF Python env)
except ImportError:
    sys.exit("pyserial missing: run with the ESP-IDF python (after export.sh) "
             "or `pip install pyserial`")

FRAME = "@@"


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*")) + sorted(glob.glob("/dev/ttyACM*"))
    if not ports:
        sys.exit("no board found (/dev/cu.usbmodem*); pass -p PORT")
    return ports[0]


def quote(arg):
    """Quote one argument for the embpet console (README 3.2); newlines become \\n."""
    esc = arg.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
    return '"%s"' % esc


def open_port(port, serial_cls=None):
    """Open the port without resetting the chip.

    The USB-Serial-JTAG resets the chip while RTS=1 and DTR=0. macOS asserts both lines
    when the port opens, and a plain pyserial open() then clears DTR before RTS, so the
    chip sees (DTR,RTS) = (1,1) -> (0,1) -> (0,0) and resets at the middle step
    (embpet-s2j.1). With dsrdtr=True, open() skips its DTR update (on POSIX dsrdtr has no
    other effect) and clears only RTS; DTR is cleared afterwards:
    (1,1) -> (1,0) -> (0,0). DTR=1/RTS=0 only selects the boot mode for a later reset.
    """
    s = (serial_cls or serial.Serial)()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.dsrdtr = True
    s.open()
    try:
        s.dtr = False   # applied now that the port is open: RTS first, then DTR
        s.dsrdtr = False
    except BaseException:
        s.close()
        raise
    return s


def _reset(s):
    """Reset the chip: same as esptool's HardReset(uses_usb=True), RTS high holds EN low."""
    s.rts = True
    time.sleep(0.2)
    s.rts = False
    time.sleep(0.2)


def monitor(port, seconds, reset):
    try:
        s = open_port(port or find_port())
    except serial.SerialException as e:
        print("serial error: %s" % e, file=sys.stderr)
        return 2
    try:
        with s:
            if reset:
                _reset(s)
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                data = s.read(4096)
                if data:
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()
    except serial.SerialException as e:
        print("serial error: %s" % e, file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        return 0
    return 0


def _frame_obj(s):
    """Parse s as a JSON object holding t and seq, else None."""
    try:
        f = json.loads(s)
    except ValueError:
        return None
    if isinstance(f, dict) and "t" in f and "seq" in f:
        return f
    return None


class FrameChecker:
    """Line-by-line checker for --check-frames.

    A line starting with '@@' must be a frame (JSON object with int seq and a boot id).
    An '@@' later in a line is bad only if the rest parses as a frame (a real frame glued
    onto another line); otherwise it is log text and only counted in embedded_text.
    seq must be strictly increasing per boot id; any gap is counted and fails the check.
    Meant for plain streams: an `evt since` replay re-emits old seqs and would be
    reported as bad.
    """

    def __init__(self):
        self.frames = self.bad = self.gaps = self.embedded_text = 0
        self.problems = []
        self.last = {}          # boot id -> last seq

    def _bad(self, msg, text):
        self.bad += 1
        self.problems.append("%s: %r" % (msg, text[:120]))

    def feed(self, text):
        if "@@" not in text:
            return
        if not text.startswith("@@"):
            idx = text.find("@@")
            while idx >= 0:
                if _frame_obj(text[idx + 2:]) is not None:
                    self._bad("frame glued onto another line", text)
                    return
                idx = text.find("@@", idx + 2)
            self.embedded_text += 1
            return
        f = _frame_obj(text[2:])
        if f is None or type(f["seq"]) is not int or "boot" not in f:
            self._bad("bad frame", text)
            return
        self.frames += 1
        seq, boot = f["seq"], f["boot"]
        prev = self.last.get(boot)
        if prev is not None:
            if seq <= prev:
                self._bad("seq %d after %d (boot %s)" % (seq, prev, boot), text)
            elif seq > prev + 1:
                self.gaps += 1
        self.last[boot] = seq if prev is None else max(seq, prev)

    def ok(self):
        return self.bad == 0 and self.gaps == 0 and self.frames > 0

    def summary(self):
        return {"frames": self.frames, "bad": self.bad, "gaps": self.gaps,
                "embedded_text": self.embedded_text, "boots": len(self.last),
                "last_seq": self.last}


def check_frames(port, seconds, reset):
    """Read SEC seconds and run every line through FrameChecker.
    Exit 0 = ok, 1 = bad frames / gaps / no frames, 2 = serial error."""
    try:
        s = open_port(port or find_port())
    except serial.SerialException as e:
        print("serial error: %s" % e, file=sys.stderr)
        return 2
    chk = FrameChecker()
    try:
        with s:
            if reset:
                _reset(s)
            deadline = time.monotonic() + seconds
            buf = b""
            while time.monotonic() < deadline:
                buf += s.read(4096)
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    text = raw.decode("utf-8", "replace").rstrip("\r")
                    chk.feed(text)
    except serial.SerialException as e:
        print("serial error: %s" % e, file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        pass
    for p in chk.problems[:5]:
        print(p, file=sys.stderr)
    print(json.dumps(chk.summary()))
    return 0 if chk.ok() else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-p", "--port")
    ap.add_argument("-t", "--timeout", type=float, default=15.0)
    ap.add_argument("-a", "--actor", help="actor id added as -a")
    ap.add_argument("--show-events", action="store_true",
                    help="also print evt frames seen while waiting (stderr)")
    ap.add_argument("--raw", action="store_true",
                    help="print every console line to stderr")
    ap.add_argument("--monitor", type=float, metavar="SEC",
                    help="print raw serial output for SEC seconds, then exit")
    ap.add_argument("--check-frames", type=float, metavar="SEC",
                    help="read SEC seconds and verify every @@ line is JSON with strictly "
                         "increasing seq; exit 0 (ok) / 1 (bad) / 2 (serial error)")
    ap.add_argument("--reset", action="store_true",
                    help="with --monitor or --check-frames: reset the chip first")
    ap.add_argument("cmd", nargs="*", help="command words, then arguments")
    a = ap.parse_args()

    if a.reset and a.monitor is None and a.check_frames is None:
        ap.error("--reset requires --monitor or --check-frames")
    if a.check_frames is not None:
        if a.cmd or a.monitor is not None:
            ap.error("--check-frames cannot be combined with a command or --monitor")
        if a.check_frames <= 0:
            ap.error("--check-frames SEC must be greater than 0")
        return check_frames(a.port, a.check_frames, a.reset)
    if a.monitor is not None:
        if a.cmd:
            ap.error("--monitor cannot be combined with a command")
        if a.monitor <= 0:
            ap.error("--monitor SEC must be greater than 0")
        return monitor(a.port, a.monitor, a.reset)
    if not a.cmd:
        ap.error("a command is required (or --monitor SEC)")

    # The first two words are the command ("wifi set"); others are quoted args.
    words = a.cmd[:2] if len(a.cmd) >= 2 and a.cmd[0] in ("tg", "wifi", "io", "evt", "sys") else a.cmd[:1]
    args = a.cmd[len(words):]
    rid = "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(8))
    opts = ["-i", rid] + (["-a", a.actor] if a.actor else [])
    line = " ".join(words + opts + [quote(x) for x in args])

    try:
        s = open_port(a.port or find_port())
    except serial.SerialException as e:
        print("serial error: %s" % e, file=sys.stderr)
        return 2

    with s:
        s.reset_input_buffer()
        s.write((line + "\n").encode("utf-8"))
        s.flush()
        deadline = time.monotonic() + a.timeout
        buf = b""
        while time.monotonic() < deadline:
            buf += s.read(4096)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = raw.decode("utf-8", "replace").rstrip("\r")
                if a.raw:
                    print(text, file=sys.stderr)
                # Only lines that start with "@@{" are frames (README 3.3); the echoed
                # command line or log text may contain "@@{" further in.
                if not text.startswith(FRAME + "{"):
                    continue
                try:
                    frame = json.loads(text[len(FRAME):])
                except ValueError:
                    continue
                if frame.get("t") == "evt":
                    if a.show_events:
                        print(json.dumps(frame, ensure_ascii=False), file=sys.stderr)
                    continue
                if frame.get("t") == "res" and frame.get("rid") == rid:
                    print(json.dumps(frame, ensure_ascii=False))
                    return 0 if frame.get("ok") else 1
    print("timeout: no response to rid=%s after %.0fs" % (rid, a.timeout), file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
