"""Serial port discovery and the reset-free open (README 3.1)."""
import glob
import time

import serial  # pyserial (ESP-IDF python env or the project .venv)


def list_ports():
    return sorted(glob.glob("/dev/cu.usbmodem*")) + sorted(glob.glob("/dev/ttyACM*"))


def find_port():
    """First USB-Serial/JTAG port, or None when no board is attached."""
    ports = list_ports()
    return ports[0] if ports else None


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
