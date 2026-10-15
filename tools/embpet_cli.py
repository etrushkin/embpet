#!/usr/bin/env python3
"""Send one CLI command to an embpet board and print its @@ response frame.

Usage:
  embpet_cli.py [-p PORT] [-t SEC] [-a ACTOR] [--show-events] [--raw] CMD [ARG ...] [-- TEXT ...]

Each ARG and TEXT is quoted and escaped for the embpet console (README 3.2), so values
with spaces or quotes are safe:  embpet_cli.py wifi set -- "my ssid" 'p"ss'
Words after a bare `--` are positional text and may start with '-' (a Wi-Fi password
like "-secret", a message like "-1 degrees"); without `--` such a word would be taken
as an option by the firmware's argument parser.
CLI flags (-p, -t, -a, --show-events, ...) must come BEFORE the command words. Everything
from the command on is passed to the firmware, so its own options work:
  embpet_cli.py -t 5 tg ack --older 30      embpet_cli.py io pulse 3 -d 100

Prints the response JSON on stdout. Exit codes: 0 = ok:true, 1 = ok:false,
2 = timeout / serial error. See README.md section 3 for the protocol.

Passthrough mode (no command is sent):
  embpet_cli.py [-p PORT] --monitor SEC [--reset]

Prints the raw serial output for SEC seconds and exits 0. With --reset the chip
is reset first (RTS pulse on USB-Serial-JTAG), so the 2nd-stage bootloader log
and the app banner are captured. Use it where `idf.py monitor` has no TTY.

Listen mode (no command is sent):
  embpet_cli.py [-p PORT] --listen SEC [--reset]

Prints every evt frame as one JSON line on stdout for SEC seconds (0 = until Ctrl-C).

Frame check (no command is sent):
  embpet_cli.py [-p PORT] --check-frames SEC [--reset]

Reads SEC seconds, parses every `@@` line as JSON and checks that seq is strictly
increasing per boot id. A mid-line '@@' is bad only if the rest of the line parses as a
frame (t and seq); other '@@' text is counted in embedded_text. Prints a JSON summary
and up to 5 bad lines to stderr; exit 0 iff bad == 0, gaps == 0 and frames > 0.

The reusable parts live in the tools/embpet package (Board.cmd, Board.events).
Offline test: scripts/idf.sh python tools/test_embpet_cli.py
"""
import argparse
import json
import sys
import time

try:
    import serial  # pyserial (bundled with the ESP-IDF Python env)
except ImportError:
    sys.exit("pyserial missing: run with the ESP-IDF python (scripts/idf.sh python ...) "
             "or the project .venv (scripts/dev-venv.sh)")

from embpet import (FRAME, Board, CmdTimeout, FrameChecker, NoBoard,  # noqa: E402,F401
                    build_line, find_port, new_rid, open_port, parse_frame, quote,
                    reset_chip, split_command)


def _jprint(frame, file=None):
    print(json.dumps(frame, ensure_ascii=False), file=file or sys.stdout, flush=True)


def _serial_error(e):
    print("serial error: %s" % e, file=sys.stderr)
    return 2


def monitor(port, seconds, reset):
    try:
        board = Board(port, reset=reset)
    except (serial.SerialException, NoBoard) as e:
        return _serial_error(e)
    try:
        with board:
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                data = board.read_raw()
                if data:
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()
    except serial.SerialException as e:
        return _serial_error(e)
    except KeyboardInterrupt:
        return 0
    return 0


def check_frames(port, seconds, reset):
    """Read SEC seconds and run every line through FrameChecker.
    Exit 0 = ok, 1 = bad frames / gaps / no frames, 2 = serial error."""
    try:
        board = Board(port, reset=reset)
    except (serial.SerialException, NoBoard) as e:
        return _serial_error(e)
    chk = FrameChecker()
    try:
        with board:
            for text in board.lines(seconds):
                chk.feed(text)
    except serial.SerialException as e:
        return _serial_error(e)
    except KeyboardInterrupt:
        pass
    for p in chk.problems[:5]:
        print(p, file=sys.stderr)
    print(json.dumps(chk.summary()))
    return 0 if chk.ok() else 1


def listen(port, seconds, reset):
    """Print every evt frame as a JSON line. SECONDS == 0 runs until Ctrl-C."""
    try:
        board = Board(port, reset=reset)
    except (serial.SerialException, NoBoard) as e:
        return _serial_error(e)
    try:
        with board:
            for f in board.events(seconds or None):
                _jprint(f)
    except serial.SerialException as e:
        return _serial_error(e)
    except KeyboardInterrupt:
        pass
    return 0


def run_command(a, words, args, text):
    try:
        board = Board(a.port)
    except (serial.SerialException, NoBoard) as e:
        return _serial_error(e)
    if a.raw:
        board.on_line = lambda t: print(t, file=sys.stderr)
    if a.show_events:
        board.on_event = lambda f: _jprint(f, sys.stderr)
    try:
        with board:
            board.flush_input()
            res = board.cmd(" ".join(words), *args, text=text, actor=a.actor,
                            timeout=a.timeout)
    except ValueError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2
    except CmdTimeout as e:
        print("timeout: %s" % e, file=sys.stderr)
        return 2
    except serial.SerialException as e:
        return _serial_error(e)
    except KeyboardInterrupt:
        return 130
    _jprint(res)
    return 0 if res.get("ok") else 1


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    # Everything after the first bare `--` is positional text for the command; argparse
    # must not see it (it would drop the `--` and parse words like "-x" as flags).
    text = []
    if "--" in argv:
        i = argv.index("--")
        argv, text = argv[:i], argv[i + 1:]

    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0], allow_abbrev=False)
    ap.add_argument("-p", "--port")
    ap.add_argument("-t", "--timeout", type=float, default=15.0)
    ap.add_argument("-a", "--actor", help="actor id added as -a")
    ap.add_argument("--show-events", action="store_true",
                    help="also print evt frames seen while waiting (stderr)")
    ap.add_argument("--raw", action="store_true",
                    help="print every console line to stderr")
    ap.add_argument("--monitor", type=float, metavar="SEC",
                    help="print raw serial output for SEC seconds, then exit")
    ap.add_argument("--listen", type=float, metavar="SEC",
                    help="print evt frames as JSON lines for SEC seconds (0 = until "
                         "Ctrl-C), then exit")
    ap.add_argument("--check-frames", type=float, metavar="SEC",
                    help="read SEC seconds and verify every @@ line is JSON with strictly "
                         "increasing seq; exit 0 (ok) / 1 (bad) / 2 (serial error)")
    ap.add_argument("--reset", action="store_true",
                    help="with --monitor, --listen or --check-frames: reset the chip first")
    ap.add_argument("cmd", nargs=argparse.REMAINDER,
                    help="command words, then firmware arguments, all passed on as "
                    "given (CLI flags go before them; put text that may start with '-' "
                    "after a bare --)")
    a = ap.parse_args(argv)

    modes = [m for m in (a.monitor, a.listen, a.check_frames) if m is not None]
    if a.reset and not modes:
        ap.error("--reset requires --monitor, --listen or --check-frames")
    if len(modes) > 1:
        ap.error("--monitor, --listen and --check-frames are mutually exclusive")
    if modes:
        if a.cmd or text:
            ap.error("--monitor, --listen and --check-frames cannot be combined "
                     "with a command")
        if a.check_frames is not None:
            if a.check_frames <= 0:
                ap.error("--check-frames SEC must be greater than 0")
            return check_frames(a.port, a.check_frames, a.reset)
        if a.listen is not None:
            if a.listen < 0:
                ap.error("--listen SEC must be 0 or greater")
            return listen(a.port, a.listen, a.reset)
        if a.monitor <= 0:
            ap.error("--monitor SEC must be greater than 0")
        return monitor(a.port, a.monitor, a.reset)
    if not a.cmd:
        ap.error("a command is required (or --monitor SEC)")

    words, args = split_command(a.cmd)
    return run_command(a, words, args, text)


if __name__ == "__main__":
    sys.exit(main())
