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


def reset_chip(s):
    """Reset the chip: same as esptool's HardReset(uses_usb=True), RTS high holds EN low."""
    s.rts = True
    time.sleep(0.2)
    s.rts = False
    time.sleep(0.2)
