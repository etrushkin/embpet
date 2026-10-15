"""Fixtures for the hardware-in-the-loop tests.

Run through the board lock:  scripts/hw.sh python tools/hil.py [pytest args]
The port is $EMBPET_PORT, else the first /dev/cu.usbmodem* (or /dev/ttyACM*). Every test
that uses the `board` fixture is skipped when no board is found or the port cannot be
opened, except that an explicit $EMBPET_PORT (or EMBPET_REQUIRE_BOARD=1, set by
`tools/hil.py --require-board`) that is missing or unopenable fails instead.
"""
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
from embpet import Board, find_port  # noqa: E402

import serial  # noqa: E402


@pytest.fixture(scope="session")
def port():
    explicit = os.environ.get("EMBPET_PORT")
    p = explicit or find_port()
    if not p:
        msg = "no board attached (set EMBPET_PORT or plug in /dev/cu.usbmodem*)"
        if os.environ.get("EMBPET_REQUIRE_BOARD"):
            pytest.fail(msg)
        pytest.skip(msg)
    return p


@pytest.fixture(scope="session")
def board(port):
    """One open connection for the whole session. The port is opened without resetting the
    chip; wait_ready() covers a board that was reset just before (its first data can take
    about 2.5 s)."""
    try:
        b = Board(port)
    except serial.SerialException as e:
        msg = "cannot open %s: %s" % (port, e)
        # A port named on purpose (or a required board) that fails is a failure, not a skip.
        if os.environ.get("EMBPET_PORT") or os.environ.get("EMBPET_REQUIRE_BOARD"):
            pytest.fail(msg)
        pytest.skip(msg)
    with b:
        b.wait_ready()
        b.flush_input()
        yield b
