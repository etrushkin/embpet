"""Input events through OUTn -> opto -> IN loopback wiring (README 2.3). Marker: loopback.

Skipped unless asked for:

    scripts/hw.sh python tools/hil.py -m loopback          (or EMBPET_HIL_LOOPBACK=1)

Wiring: output OUTn drives the field side of the input's opto, so that `io set n on` makes the
input active (README 1.3). The pairs are OUT:IN in $EMBPET_LOOPBACK, default "0:10,1:20,2:21"
(OUT0 -> IN10, OUT1 -> IN20, OUT2 -> IN21; physically GPIO4 -> IN10, GPIO3 -> IN20,
GPIO2 -> IN21, see the README 1.2 pin map). Each pair is tested separately. Output levels and
debounce times are restored afterwards.
"""
import os
import time

import pytest

pytestmark = [pytest.mark.loopback, pytest.mark.usefixtures("restore_io_state")]


def pairs():
    spec = os.environ.get("EMBPET_LOOPBACK", "0:10,1:20,2:21")
    return [tuple(int(x) for x in p.split(":")) for p in spec.split(",") if p]


PAIRS = pairs()


@pytest.fixture(params=PAIRS, ids=lambda p: "OUT%d-IN%d" % p)
def pair(request, board):
    out, pin = request.param
    board.cmd("io set", str(out), "off")
    board.cmd("io debounce", str(pin), "20")
    time.sleep(0.3)
    board.flush_input()
    return out, pin


def next_input(board, pin, seconds=1.5):
    """First `input` event for PIN within SECONDS (queued ones first), else None."""
    for e in board.events(seconds):
        if e.get("ev") == "input" and e.get("pin") == pin:
            return e
    return None


def test_output_on_makes_the_input_active_and_off_releases_it(board, pair):
    out, pin = pair
    board.cmd("io set", str(out), "on")
    press = next_input(board, pin)
    assert press is not None
    assert (press["active"], press["extra"]) == (1, 0)
    board.cmd("io set", str(out), "off")
    release = next_input(board, pin)
    assert release is not None
    assert (release["active"], release["extra"]) == (0, 0)


def test_io_in_reports_the_input_state_after_an_event(board, pair):
    out, pin = pair
    board.cmd("io set", str(out), "on")
    assert next_input(board, pin) is not None
    state = next(i for i in board.cmd("io in")["in"] if i["pin"] == pin)
    assert (state["active"], state["level"], state["extra"]) == (1, 0, 0)


def test_input_event_has_the_common_frame_fields(board, pair):
    out, pin = pair
    board.cmd("io set", str(out), "on")
    e = next_input(board, pin)
    assert e is not None
    assert e["t"] == "evt" and isinstance(e["seq"], int) and len(e["boot"]) == 8


def test_a_pulse_gives_two_events_about_the_pulse_width_apart(board, pair):
    out, pin = pair
    t0 = time.monotonic()
    board.cmd("io pulse", str(out), "-d", "400")
    first = next_input(board, pin)
    second = next_input(board, pin)
    t2 = time.monotonic()
    assert first is not None and second is not None
    assert (first["active"], second["active"]) == (1, 0)
    assert 0.4 < t2 - t0 < 0.7


def test_a_pulse_shorter_than_the_debounce_time_gives_no_event_but_counts_in_extra(board, pair):
    out, pin = pair
    board.cmd("io debounce", str(pin), "200")
    board.cmd("io pulse", str(out), "-d", "40")
    assert next_input(board, pin, 1.0) is None
    board.cmd("io set", str(out), "on")
    e = next_input(board, pin, 1.5)
    assert e is not None
    assert (e["active"], e["extra"]) == (1, 2)


def test_debounce_zero_reports_a_clean_press_at_once(board, pair):
    out, pin = pair
    board.cmd("io debounce", str(pin), "0")
    board.cmd("io set", str(out), "on")
    e = next_input(board, pin)
    assert e is not None and e["active"] == 1


def test_evt_since_reports_truncated_after_the_ring_overflows(board, pair):
    out, pin = pair
    seq0 = board.cmd("sys info")["seq"]
    for _ in range(18):                                 # 36 input events > the 32 kept
        board.cmd("io set", str(out), "on")
        time.sleep(0.1)
        board.cmd("io set", str(out), "off")
        time.sleep(0.1)
    list(board.events(0.5))     # drain live events in transit; events(0) only returns queued ones
    res = board.cmd("evt since", str(seq0))
    replayed = list(board.events(0))
    assert res["truncated"] is True
    assert res["count"] == len(replayed) == 32
    assert all(e["seq"] > seq0 for e in replayed)
