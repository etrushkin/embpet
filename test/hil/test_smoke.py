"""Smoke tests: only console commands every firmware build has, so whatever firmware is on
the board works. Run: scripts/hw.sh python tools/hil.py -k smoke"""
import pytest

pytestmark = pytest.mark.smoke


def test_smoke_sys_info_returns_ok(board):
    res = board.cmd("sys info")
    assert res["t"] == "res"
    assert res["ok"] is True


def test_smoke_unknown_command_returns_error_res(board):
    res = board.cmd("nosuchcmd")
    assert res["t"] == "res"
    assert res["ok"] is False
    assert res["err"] == "bad_args"


def test_smoke_rid_is_echoed(board):
    res = board.cmd("sys info", rid="smokerid42")
    assert res["rid"] == "smokerid42"


def test_smoke_actor_is_echoed(board):
    res = board.cmd("sys info", actor="smoke")
    assert res["actor"] == "smoke"
