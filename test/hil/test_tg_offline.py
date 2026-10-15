"""Telegram bridge paths that need Wi-Fi to be down (README 2.1, 3.3).

A module fixture takes the link down once (the .env.local SSID with a wrong password) and
puts the .env.local credentials back afterwards, so the board is online again when the next
module starts. While the link is down nothing can reach Telegram, which is what makes it safe
to use a placeholder owner chat id here; it is cleared again after each test.
"""
import pytest

import hil_util as U

PLACEHOLDER_OWNER = "1"
FAKE_TOKEN = "123456:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"      # well-formed, not a real bot

pytestmark = pytest.mark.slow


@pytest.fixture(scope="module")
def wifi_down(board, env, wifi_secured):
    st = U.ensure_wifi_up(board, env)
    assert st["state"] == "up", "wifi not up before the outage"
    bad = U.wrong_password(env["WIFI_PASS"])
    try:
        assert board.cmd("wifi set", text=[env["WIFI_SSID"], bad])["ok"]
        assert U.wait_wifi(board, "down", 45.0)["state"] == "down"
        yield
    finally:
        U.set_wifi_from_env(board, env)
        up = U.wait_wifi(board, "up", 90.0)
        has_token = board.cmd("tg status")["token"] != ""
        online = (not has_token) or U.wait_for(
            lambda: board.cmd("tg status")["state"] == "online", 60.0, 2.0)
        assert up["state"] == "up", "wifi did not recover"
        assert online, "bridge did not come back online"


@pytest.fixture()
def placeholder_owner(board, wifi_down):
    """An owner is set for the send test; an existing (real) owner is left alone."""
    existing = U.owner_chat(board)
    if existing is None:
        assert board.cmd("tg owner", text=[PLACEHOLDER_OWNER])["ok"] is True
    try:
        yield existing if existing is not None else int(PLACEHOLDER_OWNER)
    finally:
        if existing is None:
            board.cmd("tg owner", "--clear")


@pytest.fixture()
def no_owner(board, wifi_down):
    if U.owner_chat(board) is not None:
        pytest.skip("an owner is paired")


def test_bridge_is_offline_while_wifi_is_down(board, wifi_down):
    res = board.cmd("tg status")
    if res["token"] == "":
        pytest.skip("no bot token stored")
    assert res["state"] == "offline"


def test_send_while_wifi_is_down_is_offline(board, placeholder_owner):
    before = board.cmd("tg status")["pending"]
    res = board.cmd("tg send", text=["hil: must never be sent"], actor="hil")
    assert res["ok"] is False
    assert res["err"] == "offline"
    assert board.cmd("tg status")["pending"] == before


def test_send_without_owner_while_wifi_is_down_is_not_configured_first(board, no_owner):
    res = board.cmd("tg send", text=["hil: must never be sent"], actor="hil")
    assert res["ok"] is False
    assert res["err"] == "not_configured"


def test_too_long_is_reported_before_offline(board, placeholder_owner):
    if placeholder_owner != int(PLACEHOLDER_OWNER):
        pytest.skip("a real owner is paired: an over-limit send must not risk reaching it")
    res = board.cmd("tg send", text=["x" * 4100], actor="hil")
    assert res["err"] == "too_long"


def test_token_cannot_be_checked_while_wifi_is_down(board, wifi_down):
    before = board.cmd("tg status")["token"]
    res = board.cmd("tg token", text=[FAKE_TOKEN])
    assert res["ok"] is False
    assert res["err"] == "offline"
    assert board.cmd("tg status")["token"] == before


def test_owner_set_show_and_clear(board, no_owner):
    res = board.cmd("tg owner", text=[PLACEHOLDER_OWNER])
    try:
        assert res["ok"] is True
        assert res["chat_id"] == 1
        assert board.cmd("tg status")["owner"] == 1
    finally:
        cleared = board.cmd("tg owner", "--clear")
    assert cleared["chat_id"] is None
    assert board.cmd("tg status")["owner"] is None


def test_owner_accepts_a_negative_group_chat_id(board, no_owner):
    try:
        res = board.cmd("tg owner", text=["-1000000000001"])
        assert res["ok"] is True
        assert res["chat_id"] == -1000000000001
    finally:
        board.cmd("tg owner", "--clear")
    assert board.cmd("tg owner")["chat_id"] is None
