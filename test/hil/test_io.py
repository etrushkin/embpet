"""GPIO outputs and input settings (README 2.3, 2.4, 3.4) on the production firmware.

Pulse timing is checked through `io get` remaining_ms and the time the host observes the
pulse end (the firmware has no edge recorder in production). Output levels, boot levels and
debounce times are restored after every test by the restore_io_state fixture.

Tests drive OUT6 and OUT7 (JTAG pins, unused by the USB console) so a pulse never touches
the strapping pins.
"""
import time

import pytest

import hil_util as U

pytestmark = pytest.mark.usefixtures("restore_io_state")

P = 6        # pin driven by most tests
Q = 7


def get_pin(board, pin):
    res = board.cmd("io get")
    return next(o for o in res["out"] if o["pin"] == pin)


def other(level):
    return "off" if level == "on" else "on"


@pytest.fixture()
def pin_off(board):
    """OUT6 and OUT7 off and not pulsing at the start of the test."""
    for p in (P, Q):
        board.cmd("io set", str(p), "off")


# ---- io get / set ----

def test_io_get_lists_the_eight_outputs_with_their_fields(board):
    res = board.cmd("io get")
    assert res["ok"] is True
    assert [o["pin"] for o in res["out"]] == list(range(8))
    for o in res["out"]:
        assert o["level"] in ("on", "off")
        assert isinstance(o["pulsing"], bool)
        assert isinstance(o["remaining_ms"], int)


def test_io_set_on_answers_with_the_resulting_level(board, pin_off):
    res = board.cmd("io set", str(P), "on")
    assert res["ok"] is True
    assert (res["pin"], res["level"]) == (P, "on")


def test_io_get_reflects_set(board, pin_off):
    board.cmd("io set", str(P), "on")
    assert get_pin(board, P)["level"] == "on"
    board.cmd("io set", str(P), "off")
    assert get_pin(board, P)["level"] == "off"


def test_io_set_toggle_flips_the_level_each_time(board, pin_off):
    assert board.cmd("io set", str(P), "toggle")["level"] == "on"
    assert board.cmd("io set", str(P), "toggle")["level"] == "off"


def test_io_set_only_changes_the_named_pin(board, pin_off):
    before = U.out_levels(board)
    board.cmd("io set", str(P), "on")
    after = U.out_levels(board)
    assert {p: l for p, l in after.items() if p != P} == {p: l for p, l in before.items() if p != P}


@pytest.mark.parametrize("args,err", [
    (("8", "on"), "bad_pin"),
    (("9", "on"), "bad_pin"),
    (("10", "on"), "bad_pin"),
    (("1", "maybe"), "bad_args"),
    (("1",), "bad_args"),
    (("x", "on"), "bad_args"),
])
def test_io_set_rejects_bad_input(board, args, err):
    res = board.cmd("io set", *args)
    assert res["ok"] is False
    assert res["err"] == err


def test_io_set_with_negative_pin_is_bad_args(board):
    res = board.cmd("io set", "-1", "on")
    assert res["ok"] is False
    assert res["err"] == "bad_args"


# ---- io pulse ----

def test_pulse_inverts_the_level_and_reports_remaining_ms(board, pin_off):
    t0 = time.monotonic()
    res = board.cmd("io pulse", str(P), "-d", "3000")
    assert res["ok"] is True
    assert (res["pin"], res["level"]) == (P, "on")
    assert 2700 < res["remaining_ms"] <= 3000
    entry = get_pin(board, P)
    assert entry["pulsing"] is True and entry["level"] == "on"
    elapsed_ms = (time.monotonic() - t0) * 1000
    assert entry["remaining_ms"] <= res["remaining_ms"]
    assert res["remaining_ms"] - entry["remaining_ms"] <= elapsed_ms + 250


def test_pulse_default_duration_is_100_ms(board, pin_off):
    res = board.cmd("io pulse", str(P))
    assert res["ok"] is True
    assert 0 < res["remaining_ms"] <= 100


def test_remaining_ms_counts_down_in_real_time(board, pin_off):
    board.cmd("io pulse", str(P), "-d", "4000")
    t1 = time.monotonic()
    r1 = get_pin(board, P)["remaining_ms"]
    time.sleep(1.0)
    t2 = time.monotonic()
    r2 = get_pin(board, P)["remaining_ms"]
    elapsed_ms = (t2 - t1) * 1000
    assert abs((r1 - r2) - elapsed_ms) <= 250


def test_pulse_ends_after_its_duration_and_restores_off(board, pin_off):
    t0 = time.monotonic()
    board.cmd("io pulse", str(P), "-d", "1500")
    end = None
    while time.monotonic() - t0 < 4.0:
        if not get_pin(board, P)["pulsing"]:
            end = time.monotonic() - t0
            break
        time.sleep(0.05)
    assert end is not None
    assert 1.4 <= end <= 2.2
    entry = get_pin(board, P)
    assert entry["level"] == "off" and entry["remaining_ms"] == 0


def test_pulse_from_the_on_level_goes_off_then_back_on(board, pin_off):
    board.cmd("io set", str(P), "on")
    res = board.cmd("io pulse", str(P), "-d", "800")
    assert res["level"] == "off"
    assert get_pin(board, P)["level"] == "off"
    time.sleep(1.5)
    entry = get_pin(board, P)
    assert entry["level"] == "on" and entry["pulsing"] is False


def test_pulse_of_one_millisecond_is_accepted_and_ends(board, pin_off):
    res = board.cmd("io pulse", str(P), "-d", "1")
    assert res["ok"] is True
    time.sleep(0.3)
    entry = get_pin(board, P)
    assert entry["pulsing"] is False and entry["level"] == "off"


def test_re_pulse_restarts_the_timer(board, pin_off):
    board.cmd("io pulse", str(P), "-d", "3000")
    time.sleep(1.5)
    before = get_pin(board, P)["remaining_ms"]
    res = board.cmd("io pulse", str(P), "-d", "3000")
    assert before < 1700
    assert res["level"] == "on"
    assert res["remaining_ms"] > 2700


def test_re_pulse_still_restores_the_original_level(board, pin_off):
    board.cmd("io pulse", str(P), "-d", "1000")
    time.sleep(0.4)
    board.cmd("io pulse", str(P), "-d", "1000")
    time.sleep(1.8)
    entry = get_pin(board, P)
    assert entry["level"] == "off" and entry["pulsing"] is False


def test_io_set_cancels_a_running_pulse(board, pin_off):
    board.cmd("io pulse", str(P), "-d", "1200")
    board.cmd("io set", str(P), "on")
    time.sleep(1.8)
    entry = get_pin(board, P)
    assert entry["level"] == "on" and entry["pulsing"] is False


def test_toggle_cancels_a_running_pulse(board, pin_off):
    board.cmd("io pulse", str(P), "-d", "1200")
    res = board.cmd("io set", str(P), "toggle")
    assert res["level"] == "off"
    time.sleep(1.8)
    entry = get_pin(board, P)
    assert entry["level"] == "off" and entry["pulsing"] is False


def test_two_pins_pulse_independently(board, pin_off):
    board.cmd("io pulse", str(P), "-d", "800")
    board.cmd("io pulse", str(Q), "-d", "2500")
    time.sleep(1.4)
    p, q = get_pin(board, P), get_pin(board, Q)
    assert p["pulsing"] is False and p["level"] == "off"
    assert q["pulsing"] is True and q["level"] == "on"


@pytest.mark.parametrize("args,err", [
    (("10",), "bad_pin"),
    (("20",), "bad_pin"),
    (("8",), "bad_pin"),
    ((str(P), "-d", "0"), "bad_args"),
    ((str(P), "-d", "600001"), "bad_args"),
    ((str(P), "-d", "x"), "bad_args"),
])
def test_io_pulse_rejects_bad_input(board, args, err):
    res = board.cmd("io pulse", *args)
    assert res["ok"] is False
    assert res["err"] == err


def test_pulse_of_the_maximum_duration_is_accepted(board, pin_off):
    res = board.cmd("io pulse", str(P), "-d", "600000")
    assert res["ok"] is True
    assert 599000 < res["remaining_ms"] <= 600000
    board.cmd("io set", str(P), "off")          # cancel; the fixture would do it too


# ---- io boot ----

def test_io_boot_lists_eight_levels(board):
    res = board.cmd("io boot")
    assert res["ok"] is True
    assert sorted(res["levels"], key=int) == [str(p) for p in range(8)]
    assert set(res["levels"].values()) <= {"on", "off"}


def test_io_boot_set_answers_with_the_updated_map(board, board_guard):
    old = board_guard["boot"][str(P)]
    res = board.cmd("io boot", str(P), other(old))
    assert res["ok"] is True
    assert res["levels"][str(P)] == other(old)
    assert board.cmd("io boot")["levels"][str(P)] == other(old)


@pytest.mark.parametrize("args,err", [
    (("8", "on"), "bad_pin"),
    (("21", "on"), "bad_pin"),
    (("1",), "bad_args"),
    (("1", "toggle"), "bad_args"),
])
def test_io_boot_rejects_bad_input(board, args, err):
    res = board.cmd("io boot", *args)
    assert res["ok"] is False
    assert res["err"] == err


@pytest.mark.slow
def test_boot_levels_apply_after_reboot_and_runtime_levels_do_not(board, board_guard):
    boot = board_guard["boot"]
    board.cmd("io boot", str(P), other(boot[str(P)]))       # persisted: must apply
    board.cmd("io set", str(Q), other(boot[str(Q)]))        # runtime only: must be lost
    expect = dict(boot)
    expect[str(P)] = other(boot[str(P)])
    U.reboot(board)
    assert board.cmd("io boot")["levels"] == expect
    levels = U.out_levels(board)
    assert {str(p): l for p, l in levels.items()} == expect


# ---- inputs (settings only; edges need the loopback suite) ----

def test_io_in_lists_the_three_inputs_with_their_fields(board):
    res = board.cmd("io in")
    assert res["ok"] is True
    assert [i["pin"] for i in res["in"]] == [10, 20, 21]
    for i in res["in"]:
        assert i["active"] in (0, 1)
        assert i["level"] in (0, 1)
        assert i["extra"] >= 0


def test_io_debounce_lists_the_three_times(board):
    res = board.cmd("io debounce")
    assert res["ok"] is True
    assert sorted(res["debounce"], key=int) == ["10", "20", "21"]
    assert all(0 <= ms <= 1000 for ms in res["debounce"].values())


def test_io_debounce_set_is_reported_and_only_changes_that_pin(board, board_guard):
    before = board_guard["debounce"]
    res = board.cmd("io debounce", "20", "123")
    assert res["ok"] is True
    after = board.cmd("io debounce")["debounce"]
    assert after["20"] == 123
    assert {k: v for k, v in after.items() if k != "20"} == {k: v for k, v in before.items()
                                                             if k != "20"}


@pytest.mark.parametrize("ms", ["0", "1000"])
def test_io_debounce_accepts_the_range_limits(board, ms):
    assert board.cmd("io debounce", "10", ms)["ok"] is True
    assert board.cmd("io debounce")["debounce"]["10"] == int(ms)


@pytest.mark.parametrize("args,err", [
    (("9", "5"), "bad_pin"),
    (("0", "5"), "bad_pin"),
    (("22", "5"), "bad_pin"),
    (("10",), "bad_args"),
    (("10", "1001"), "bad_args"),
    (("10", "x"), "bad_args"),
])
def test_io_debounce_rejects_bad_input(board, args, err):
    res = board.cmd("io debounce", *args)
    assert res["ok"] is False
    assert res["err"] == err


@pytest.mark.slow
def test_debounce_survives_a_reboot(board, board_guard):
    board.cmd("io debounce", "21", "77")
    U.reboot(board)
    assert board.cmd("io debounce")["debounce"]["21"] == 77
