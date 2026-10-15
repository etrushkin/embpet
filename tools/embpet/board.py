"""Board: one open serial connection to an embpet board."""
import collections
import random
import string
import time

from .frames import build_line, parse_frame
from .serialport import find_port, open_port, reset_chip

EVENT_BACKLOG = 1000
SESSION_START = b"\x15\n"


class CmdTimeout(TimeoutError):
    """No matching res frame arrived in time."""


class NoBoard(RuntimeError):
    """No serial port found."""


def new_rid():
    return "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(8))


class Board:
    """Opens the port without resetting the chip (README 3.1).

    cmd() sends one command and returns its res frame (matched by rid). evt frames seen
    meanwhile are kept (the last EVENT_BACKLOG) and are yielded first by events(). The
    optional on_line(text) / on_event(frame) callbacks see every console line / evt frame
    as it arrives."""

    def __init__(self, port=None, serial_cls=None, reset=False, session_start=True):
        port = port or find_port()
        if not port:
            raise NoBoard("no board found (/dev/cu.usbmodem*); pass a port")
        self.port = port
        self.on_line = None
        self.on_event = None
        self._events = collections.deque(maxlen=EVENT_BACKLOG)
        self._buf = b""
        self._serial_cls = serial_cls
        self._session_start = session_start
        self._s = open_port(port, serial_cls=serial_cls)
        try:
            if reset:
                reset_chip(self._s)     # a reset clears the line, and the bytes would race boot
            elif session_start:
                self._start_session()
        except BaseException:
            self._s.close()
            raise

    def _start_session(self):
        """Ctrl-U then newline (README 3.2): discards a partial line a previous session left
        in the firmware's line buffer or USB ring; the newline ends the now empty line, which
        gets no response."""
        self._s.write(SESSION_START)
        self._s.flush()

    def close(self):
        self._s.close()

    def reconnect(self, delay=3.0, timeout=30.0):
        """Reopen the port after a chip reset that re-enumerates the USB device (e.g.
        `sys reboot`): wait DELAY seconds, then retry the open until TIMEOUT. Buffered input
        and queued events are dropped. Raises NoBoard when the port does not come back."""
        try:
            self._s.close()
        except OSError:
            pass
        time.sleep(delay)
        deadline = time.monotonic() + timeout
        while True:
            try:
                # serial.SerialException is an OSError subclass, so this covers it
                self._s = open_port(self.port, serial_cls=self._serial_cls)
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise NoBoard("port %s did not come back" % self.port)
                time.sleep(0.5)
        self._buf = b""
        self._events.clear()
        if self._session_start:
            self._start_session()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def flush_input(self):
        """Drop buffered input, including queued events."""
        self._s.reset_input_buffer()
        self._buf = b""
        self._events.clear()

    def read_raw(self, size=4096):
        """Raw bytes from the port (b"" after the 0.2 s read timeout); bypasses line parsing."""
        return self._s.read(size)

    def write_line(self, line):
        self._s.write((line + "\n").encode("utf-8"))
        self._s.flush()

    def lines(self, seconds):
        """Yield decoded console lines until SECONDS have passed (None = forever)."""
        deadline = None if seconds is None else time.monotonic() + seconds
        while deadline is None or time.monotonic() < deadline:
            # read(4096) would wait out the whole 0.2 s timeout; read what has arrived, or
            # block for one byte, so a response is seen as soon as it is complete.
            waiting = getattr(self._s, "in_waiting", 0)
            self._buf += self._s.read(waiting if isinstance(waiting, int) and waiting > 0 else 1)
            while b"\n" in self._buf:
                if deadline is not None and time.monotonic() >= deadline:
                    return          # log spam must not stretch the deadline; keep the rest
                raw, self._buf = self._buf.split(b"\n", 1)
                text = raw.decode("utf-8", "replace").rstrip("\r")
                if self.on_line:
                    self.on_line(text)
                yield text

    def frames(self, seconds):
        """Yield parsed frames (res and evt) until SECONDS have passed (None = forever)."""
        for text in self.lines(seconds):
            f = parse_frame(text)
            if f is not None:
                yield f

    def cmd(self, command, *args, text=(), actor=None, timeout=15.0, rid=None):
        """Send one command and return its res frame as a dict (ok may be false).

        COMMAND is the command words as one string ("wifi set"); ARGS are quoted
        arguments that argtable still parses as options ("--older", "30"); TEXT words
        follow a bare `--` and are always positional. Raises CmdTimeout."""
        rid = rid or new_rid()
        self.write_line(build_line(command.split(), args, text, rid, actor))
        for f in self.frames(timeout):
            t = f.get("t")
            if t == "evt":
                self._push_event(f)
            elif t == "res" and f.get("rid") == rid:
                return f
        raise CmdTimeout("no response to rid=%s after %.0fs" % (rid, timeout))

    def wait_ready(self, timeout=15.0):
        """Poll `sys info` until the firmware answers; returns that res. Use it after a
        chip reset (Board(reset=True)), when the app is still booting and the first open
        can deliver data only after about 2.5 s. Raises CmdTimeout."""
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise CmdTimeout("board not ready after %.0fs" % timeout)
            try:
                return self.cmd("sys info", timeout=min(1.0, left))
            except CmdTimeout:
                pass

    def _push_event(self, f):
        if self.on_event:
            self.on_event(f)
        self._events.append(f)

    def events(self, seconds=None):
        """Yield evt frames: queued ones first, then new ones until SECONDS have passed
        (None = forever). Each event is yielded once."""
        while self._events:
            yield self._events.popleft()
        for f in self.frames(seconds):
            if f.get("t") == "evt":
                if self.on_event:
                    self.on_event(f)
                yield f
