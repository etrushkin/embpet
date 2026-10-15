"""Telegram bridge commands (README 2.1, 3.3, 3.4) that need no message to leave the board.

Nothing here sends to Telegram: the send tests only cover the checks that fail before the
network is touched (bad_args, too_long, not_configured). The tests that really send to the
paired owner are in test_tg_owner.py (marker `owner`); the Wi-Fi-down paths are in
test_tg_offline.py.
"""
import json
import re

import pytest

import hil_util as U
import tg_from_env as T

MASKED = re.compile(r"^\d+:.{2}….{3}$")


@pytest.fixture()
def no_owner(board):
    """Tests that would send if an owner were set run only while there is none."""
    if U.owner_chat(board) is not None:
        pytest.skip("an owner is paired: tg send would reach the real chat")


# ---- status ----

def test_status_has_the_documented_fields(board):
    res = board.cmd("tg status")
    assert res["ok"] is True
    assert res["state"] in ("unconfigured", "offline", "online", "auth_error")
    assert isinstance(res["detail"], str)
    assert isinstance(res["bot"], str)
    assert res["owner"] is None or isinstance(res["owner"], int)
    assert isinstance(res["token"], str)
    assert isinstance(res["pending"], int) and res["pending"] >= 0


def test_status_shows_only_a_masked_token(board, env):
    res = board.cmd("tg status")
    if res["token"] == "":
        pytest.skip("no bot token stored")
    assert MASKED.match(res["token"]), "token is not in the masked form"
    token = env.get("TG_TOKEN", "")
    leaked = bool(token) and any(s in json.dumps(res) for s in T.secrets_of(token))
    assert not leaked


def test_status_masked_token_matches_the_one_in_env_local(board, env):
    if not env.get("TG_TOKEN"):
        pytest.skip("TG_TOKEN missing in .env.local")
    stored = board.cmd("tg status")["token"]
    if stored == "":
        pytest.skip("no bot token stored")
    assert stored == T.masked(env["TG_TOKEN"])


def test_bridge_goes_online_with_the_bot_name_when_wifi_and_token_are_set(board, wifi_up):
    if board.cmd("tg status")["token"] == "":
        pytest.skip("no bot token stored")
    st = U.wait_for(lambda: (lambda r: r if r["state"] == "online" else None)(
        board.cmd("tg status")), 60.0, 2.0)
    assert st, "bridge did not come online"
    assert st["bot"] != ""
    assert st["detail"] == "@" + st["bot"]


# ---- pending / ack ----

def test_pending_returns_a_list_of_items(board):
    res = board.cmd("tg pending")
    assert res["ok"] is True
    assert isinstance(res["items"], list)
    for item in res["items"]:
        assert set(item) >= {"msg", "actor", "age_s"}


def test_pending_count_in_status_matches_the_list(board):
    status = board.cmd("tg status")
    items = board.cmd("tg pending")["items"]
    assert status["pending"] == len(items)


def test_pending_for_an_actor_without_messages_is_empty(board):
    res = board.cmd("tg pending", actor="hil-nobody")
    assert res["ok"] is True
    assert res["items"] == []
    assert res["actor"] == "hil-nobody"


def test_ack_of_an_unknown_message_acks_nothing(board):
    res = board.cmd("tg ack", "-m", "999999999")
    assert res["ok"] is True
    assert res["acked"] == 0


@pytest.mark.parametrize("args", [
    (),                                   # neither option
    ("--older", "5", "-m", "7"),          # both options
    ("--older", "x"),
    ("--older", "4294967296"),
    ("-m", "0"),
    ("-m", "abc"),
])
def test_ack_rejects_bad_arguments(board, args):
    res = board.cmd("tg ack", *args)
    assert res["ok"] is False
    assert res["err"] == "bad_args"


# ---- send: the checks that fail before Telegram is contacted ----

def test_send_without_actor_is_bad_args(board, no_owner):
    res = board.cmd("tg send", text=["hil: must never be sent"])
    assert res["ok"] is False
    assert res["err"] == "bad_args"


def test_send_with_a_bad_reply_id_is_bad_args(board, no_owner):
    res = board.cmd("tg send", "-r", "abc", text=["hil: must never be sent"], actor="hil")
    assert res["ok"] is False
    assert res["err"] == "bad_args"


def test_send_text_over_4096_bytes_is_too_long(board, no_owner):
    res = board.cmd("tg send", text=["x" * 4100], actor="hil")
    assert res["ok"] is False
    assert res["err"] == "too_long"


def test_send_text_that_only_the_tag_pushes_over_the_limit_is_too_long(board, no_owner):
    # 4093 + "[hil] " (6) = 4099 UTF-16 units > 4096, while the text alone is under 4096 bytes
    res = board.cmd("tg send", text=["x" * 4093], actor="hil")
    assert res["ok"] is False
    assert res["err"] == "too_long"


def test_send_two_byte_characters_over_the_byte_limit_is_too_long(board, no_owner):
    # 2100 x 2 bytes = 4200 bytes > 4096, but only 2100 UTF-16 units
    res = board.cmd("tg send", text=["Ж" * 2100], actor="hil")
    assert res["ok"] is False
    assert res["err"] == "too_long"


def test_send_without_an_owner_is_not_configured(board, no_owner):
    res = board.cmd("tg send", text=["hil: must never be sent"], actor="hil")
    assert res["ok"] is False
    assert res["err"] == "not_configured"


def test_send_without_an_owner_leaves_pending_unchanged(board, no_owner):
    before = board.cmd("tg status")["pending"]
    board.cmd("tg send", text=["hil: must never be sent"], actor="hil")
    assert board.cmd("tg status")["pending"] == before


# ---- token ----

def test_token_that_is_not_a_bot_token_is_bad_args_and_the_old_one_stays(board):
    before = board.cmd("tg status")["token"]
    res = board.cmd("tg token", text=["not-a-token"])
    assert res["ok"] is False
    assert res["err"] == "bad_args"
    assert board.cmd("tg status")["token"] == before


# ---- owner ----

def test_owner_without_arguments_shows_the_current_owner(board):
    res = board.cmd("tg owner")
    assert res["ok"] is True
    assert res["chat_id"] == board.cmd("tg status")["owner"]


@pytest.mark.parametrize("text", [["0"], ["abc"], ["1.5"]])
def test_owner_rejects_an_invalid_chat_id(board, text):
    res = board.cmd("tg owner", text=text)
    assert res["ok"] is False
    assert res["err"] == "bad_args"


def test_owner_rejects_a_chat_id_together_with_clear(board):
    res = board.cmd("tg owner", "--clear", text=["5"])
    assert res["ok"] is False
    assert res["err"] == "bad_args"
