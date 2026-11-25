"""Board: one open serial connection to an embpet board."""
import collections
import random
import string
import time

from .frames import build_line, parse_frame
from .serialport import NoBoard, find_port, is_url, open_port, port_serial, reset_chip

EVENT_BACKLOG = 1000
SESSION_START = b"\x15\n"


class CmdTimeout(TimeoutError):
    """No matching res frame arrived in time."""


def new_rid():
    return "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(8))


class Board:
    """Opens the port without resetting the chip (SPEC.md 3.2).

    cmd() sends one command and returns its res frame (matched by rid). evt frames seen
    meanwhile are kept (the last EVENT_BACKLOG) and are yielded first by events(). The
    optional on_line(text) / on_event(frame) callbacks see every console line / evt frame
    as it arrives."""

    def __init__(self, port=None, serial_cls=None, reset=False, session_start=True,
                 serial_number=None):
        port = port or find_port(serial_number)
        if not port:
            if serial_number:
                raise NoBoard("no USB serial device with serial number %s; "
                              "pass -p PORT_OR_URL" % serial_number)
            raise NoBoard("no board found (/dev/cu.usbmodem*, /dev/ttyACM*); pass -p PORT "
                          "or -p URL (rfc2217://host:port), or --serial SN")
        self.port = port
        # USB serial number (SPEC.md 2): reconnect() finds the board again by it, because
        # the device node can change or another board can take the old one.
        self.serial_number = serial_number or port_serial(port)
        self._last = None               # (boot, seq) of the newest frame seen (SPEC.md 7.5)
        self._seen = collections.OrderedDict()   # (boot, seq) of events already delivered
        self.on_line = None
        self.on_event = None
        self.last_replay = None         # the res of the last `evt since` replay
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
        """Ctrl-U then newline (SPEC.md 3.2): discards a partial line a previous session left
        in the firmware's line buffer or USB ring; the newline ends the now empty line, which
        gets no response."""
        self._s.write(SESSION_START)
        self._s.flush()

    def close(self):
        self._s.close()

    def reconnect(self, delay=None, timeout=30.0):
        """Reopen the port after a chip reset that re-enumerates the USB device (e.g.
        `sys reboot`): wait DELAY seconds (default 3 s for a device path, 0 for a URL, which
        is reopened as it is), then retry until TIMEOUT. The board is found
        again by its USB serial number (the device node may change; with several boards
        another one may hold the old path), or by the old path (or URL) when the number is
        unknown. Buffered partial input is dropped; queued events are kept. With a session start,
        the events missed meanwhile are replayed with `evt since <last seq>` (SPEC.md 7.5:
        `evt since 0` after a new boot) and deduplicated on (boot, seq). Raises NoBoard
        when the port does not come back, AmbiguousPort when several boards match."""
        if delay is None:
            delay = 0.0 if is_url(self.port) else 3.0
        try:
            self._s.close()
        except OSError:
            pass
        time.sleep(delay)
        deadline = time.monotonic() + timeout
        prev = self._last
        while True:
            try:
                port = find_port(self.serial_number) if self.serial_number else self.port
                if port:
                    # serial.SerialException is an OSError subclass, so this covers it
                    self._s = open_port(port, serial_cls=self._serial_cls)
                    self.port = port
                    break
            except OSError:
                pass
            if time.monotonic() >= deadline:
                raise NoBoard("board %s did not come back" % (self.serial_number or self.port))
            time.sleep(0.5)
        self._buf = b""
        if self._session_start:
            self._start_session()
            if prev is not None:
                self._replay(prev)

    def _replay(self, prev):
        """After a reopen: `evt since` the last seq seen, or 0 when the boot id changed."""
        info = self.wait_ready()
        since = prev[1] if prev[0] == info.get("boot") else 0
        self.last_replay = self.cmd("evt since", str(since))

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
                self._track(f)
                yield f

    def _track(self, f):
        """Remember the newest (boot, seq) for the replay after a reconnect."""
        boot, seq = f.get("boot"), f.get("seq")
        if isinstance(boot, str) and isinstance(seq, int) and not isinstance(seq, bool):
            if self._last is None or self._last[0] != boot or seq > self._last[1]:
                self._last = (boot, seq)

    def _is_new(self, f):
        """False for an evt already delivered (same boot and seq), e.g. in a replay."""
        key = (f.get("boot"), f.get("seq"))
        if key[0] is None or key[1] is None:
            return True
        if key in self._seen:
            return False
        self._seen[key] = None
        if len(self._seen) > 4096:
            self._seen.popitem(last=False)
        return True

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
        if not self._is_new(f):
            return
        if self.on_event:
            self.on_event(f)
        self._events.append(f)

    def events(self, seconds=None):
        """Yield evt frames: queued ones first, then new ones until SECONDS have passed
        (None = forever). Each event is yielded once."""
        while self._events:
            yield self._events.popleft()
        for f in self.frames(seconds):
            if f.get("t") == "evt" and self._is_new(f):
                if self.on_event:
                    self.on_event(f)
                yield f
