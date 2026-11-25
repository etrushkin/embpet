"""Serial port discovery (SPEC.md 2) and the reset-free open (SPEC.md 3.2)."""
import glob
import time

import serial  # pyserial (ESP-IDF python env or the project .venv)


class NoBoard(RuntimeError):
    """No serial port found."""


class AmbiguousPort(NoBoard):
    """Several ports match and the caller gave nothing to tell them apart."""


def list_ports():
    return sorted(glob.glob("/dev/cu.usbmodem*")) + sorted(glob.glob("/dev/ttyACM*"))


def is_url(port):
    """True for a pyserial URL (rfc2217://, socket://, loop://, ...), false for a device path."""
    return "://" in str(port)


def port_serial(device):
    """USB serial number of DEVICE (SPEC.md 2: the chip's MAC, unique per board), or None."""
    try:
        from serial.tools import list_ports as lp
        for p in lp.comports():
            if p.device == device:
                return p.serial_number
    except Exception:
        pass
    return None


def list_candidates():
    """[(device, usb serial number or None)] for every port that may be an embpet board."""
    return [(d, port_serial(d)) for d in list_ports()]


def _norm(sn):
    return sn.strip().upper()


def find_port(serial_number=None, candidates=None):
    """The USB-Serial/JTAG port to open, or None when no board matches (SPEC.md 2).

    With SERIAL_NUMBER, only the port with that USB serial number (case-insensitive) is
    taken. Every ESP32-C3/S3 shows the same VID:PID, so without it the choice is made only
    when exactly one port exists. When several match, raises AmbiguousPort listing them
    instead of guessing. CANDIDATES is [(device, serial)] (default: list_candidates())."""
    found = list_candidates() if candidates is None else list(candidates)
    if serial_number is not None:
        want = _norm(serial_number)
        found = [c for c in found if c[1] and _norm(c[1]) == want]
    if not found:
        return None
    if len(found) > 1:
        what = "serial number %s" % serial_number if serial_number is not None else "no serial number given"
        raise AmbiguousPort("several ports match (%s): %s; pass a port or a serial number"
                            % (what, ", ".join("%s (%s)" % (d, sn or "no serial number")
                                               for d, sn in found)))
    return found[0][0]


def open_port(port, serial_cls=None, for_url=None):
    """Open the port (a device path or a pyserial URL) without resetting the chip.

    A PORT containing "://" (rfc2217://host:port, socket://, loop://) is created with
    serial.serial_for_url(port, do_not_open=True) (FOR_URL replaces that function in tests)
    and gets the same line sequence as a local port. SERIAL_CLS, when given, wins over both.
    On rfc2217:// pyserial's open() skips the DTR update (dsrdtr) and sends only RTS=0, then
    DTR=0 follows after the open: the RFC 2217 server sees RTS first, then DTR, the same
    (1,0) -> (0,0) order as locally. The initial state is the server's own: the lines are not
    pulsed on the client's behalf, but whether the server resets the chip on its own open is
    up to the server.

    The USB-Serial-JTAG resets the chip while RTS=1 and DTR=0. macOS asserts both lines
    when the port opens, and a plain pyserial open() then clears DTR before RTS, so the
    chip sees (DTR,RTS) = (1,1) -> (0,1) -> (0,0) and resets at the middle step
    (embpet-s2j.1). With dsrdtr=True, open() skips its DTR update (on POSIX dsrdtr has no
    other effect) and clears only RTS; DTR is cleared afterwards:
    (1,1) -> (1,0) -> (0,0). DTR=1/RTS=0 only selects the boot mode for a later reset.
    """
    if serial_cls is None and is_url(port):
        s = (for_url or serial.serial_for_url)(port, do_not_open=True)
    else:
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


def reset_chip(s):
    """Reset the chip: same as esptool's HardReset(uses_usb=True), RTS high holds EN low."""
    s.rts = True
    time.sleep(0.2)
    s.rts = False
    time.sleep(0.2)
