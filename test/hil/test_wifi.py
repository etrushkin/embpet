"""Wi-Fi station (README 2.5, 3.4): status, TX power, reconnect, outage and recovery.

The credentials come from the git-ignored .env.local and are only sent in-process; no assertion
here compares or prints the password. The tests that break the link use the wifi_restore
fixture, which sets the .env.local credentials again and waits for the link afterwards.
"""
import json
import re

import pytest

import hil_util as U

IPV4 = re.compile(r"^\d{1,3}(\.\d{1,3}){3}$")


@pytest.fixture()
def txpower_restore(board, board_guard):
    yield
    board.cmd("wifi txpower", str(board_guard["txpower"]))


# ---- status ----

def test_status_reports_state_ssid_and_txpower(board):
    res = U.wifi_status(board)
    assert res["ok"] is True
    assert res["state"] in ("up", "down", "unconfigured")
    assert isinstance(res["ssid"], str)
    assert isinstance(res["txpower"], (int, float))


def test_status_never_shows_a_password(board, wifi_creds):
    res = U.wifi_status(board)
    text = json.dumps(res)
    leaked = bool(wifi_creds["WIFI_PASS"]) and wifi_creds["WIFI_PASS"] in text
    assert not leaked
    assert not [k for k in res if "pass" in k.lower()]


def test_status_when_up_has_ssid_ipv4_and_negative_rssi(board, wifi_up, wifi_creds):
    res = U.wifi_status(board)
    assert res["state"] == "up"
    assert res["ssid"] == wifi_creds["WIFI_SSID"]
    assert IPV4.match(res["ip"])
    assert isinstance(res["rssi"], int) and -100 < res["rssi"] < 0


# ---- txpower ----

def test_txpower_set_is_reported_and_read_back(board, txpower_restore):
    res = board.cmd("wifi txpower", "10")
    assert res["ok"] is True
    assert res["txpower"] == 10
    assert U.wifi_status(board)["txpower"] == 10


def test_txpower_accepts_quarter_dbm_steps(board, txpower_restore):
    assert board.cmd("wifi txpower", "11.25")["txpower"] == 11.25


@pytest.mark.parametrize("value", ["1.75", "20.25", "8.3", "abc", "21"])
def test_txpower_rejects_bad_values(board, value):
    res = board.cmd("wifi txpower", value)
    assert res["ok"] is False
    assert res["err"] == "bad_args"


def test_txpower_rejects_a_negative_value(board):
    res = board.cmd("wifi txpower", "--", "-5")
    assert res["ok"] is False
    assert res["err"] == "bad_args"


# ---- set / reconnect ----

def test_set_without_arguments_is_bad_args(board):
    res = board.cmd("wifi set")
    assert res["ok"] is False
    assert res["err"] == "bad_args"


@pytest.mark.slow
def test_set_with_the_same_credentials_reconnects(board, wifi_up, wifi_creds):
    res = U.set_wifi_from_env(board, wifi_creds)
    assert res["ok"] is True
    assert "ssid" not in res and "pass" not in json.dumps(res).lower()
    st = U.wait_wifi(board, "up", 45.0)
    assert st["state"] == "up"
    assert IPV4.match(st["ip"])


@pytest.mark.slow
def test_wifi_set_output_has_no_password_at_debug_log_level(board, wifi_up, wifi_creds):
    pw = wifi_creds["WIFI_PASS"]
    if len(pw) < 8:
        pytest.skip("password too short for a meaningful partial-leak check")
    mid = pw[len(pw) // 2 - 4:len(pw) // 2 + 4]
    seen = []
    board.on_line = seen.append
    try:
        board.cmd("sys log", "debug")
        U.set_wifi_from_env(board, wifi_creds)
        for _ in board.frames(12.0):
            pass
    finally:
        board.on_line = None
        board.cmd("sys log", "warn")
    # the console echoes the typed command (which holds the password); only the firmware's
    # own output is checked
    text = "\n".join(ln for ln in seen if "wifi set" not in ln)
    leaked = pw in text or mid in text
    assert not leaked
    assert U.wait_wifi(board, "up", 45.0)["state"] == "up"


@pytest.mark.slow
def test_wrong_password_takes_the_link_down_and_the_console_keeps_answering(
        board, wifi_up, wifi_secured, wifi_creds, wifi_restore):
    bad = U.wrong_password(wifi_creds["WIFI_PASS"])
    res = board.cmd("wifi set", text=[wifi_creds["WIFI_SSID"], bad])
    assert res["ok"] is True
    st = U.wait_wifi(board, "down", 45.0)
    assert st["state"] == "down"
    assert st["ssid"] == wifi_creds["WIFI_SSID"]
    assert "ip" not in st
    for _ in range(5):                      # the station is retrying in the background
        assert board.cmd("sys info", timeout=10.0)["ok"] is True
        list(board.frames(1.0))


@pytest.mark.slow
def test_recovery_after_an_outage_reaches_up_with_an_ip(board, wifi_up, wifi_secured, wifi_creds,
                                                        wifi_restore):
    bad = U.wrong_password(wifi_creds["WIFI_PASS"])
    board.cmd("wifi set", text=[wifi_creds["WIFI_SSID"], bad])
    assert U.wait_wifi(board, "down", 45.0)["state"] == "down"
    assert U.set_wifi_from_env(board, wifi_creds)["ok"] is True
    st = U.wait_wifi(board, "up", 60.0)
    assert st["state"] == "up" and IPV4.match(st["ip"])


@pytest.mark.slow
def test_credentials_survive_a_reboot(board, wifi_up, wifi_creds):
    U.reboot(board)
    st = U.wait_wifi(board, "up", 60.0)
    assert st["state"] == "up"
    assert st["ssid"] == wifi_creds["WIFI_SSID"]
