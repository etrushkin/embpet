#!/usr/bin/env python3
"""Send one CLI command to an embpet board and print its @@ response frame.

Usage:
  embpet_cli.py [-p PORT] [-t SEC] [-a ACTOR] [--show-events] [--raw] CMD [ARG ...]

Each ARG is quoted and escaped for the esp_console parser, so values with
spaces or quotes are safe:  embpet_cli.py wifi set "my ssid" 'p"ss'

Prints the response JSON on stdout. Exit codes: 0 = ok:true, 1 = ok:false,
2 = timeout / serial error. See README.md section 3 for the protocol.

Passthrough mode (no command is sent):
  embpet_cli.py [-p PORT] --monitor SEC [--reset]

Prints the raw serial output for SEC seconds and exits 0. With --reset the chip
is reset first (RTS pulse on USB-Serial-JTAG), so the 2nd-stage bootloader log
and the app banner are captured. Use it where `idf.py monitor` has no TTY.
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
    """Quote one argument for esp_console_split_argv; newlines become \\n."""
    esc = arg.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
    return '"%s"' % esc


def open_port(port):
    # Set DTR/RTS low *before* opening so the USB-Serial-JTAG does not reset.
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()
    return s


def monitor(port, seconds, reset):
    try:
        s = open_port(port or find_port())
    except serial.SerialException as e:
        print("serial error: %s" % e, file=sys.stderr)
        return 2
    try:
        with s:
            if reset:
                # Same as esptool's HardReset(uses_usb=True): RTS high holds EN low.
                s.rts = True
                time.sleep(0.2)
                s.rts = False
                time.sleep(0.2)
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
    ap.add_argument("--reset", action="store_true",
                    help="with --monitor: reset the chip first")
    ap.add_argument("cmd", nargs="*", help="command words, then arguments")
    a = ap.parse_args()

    if a.reset and a.monitor is None:
        ap.error("--reset requires --monitor")
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
                idx = text.find(FRAME + "{")
                if idx < 0:
                    continue
                try:
                    frame = json.loads(text[idx + len(FRAME):])
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
