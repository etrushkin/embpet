"""Telegram bridge against the real, paired owner chat (README 2.1, 3.3). Marker: owner.

These tests SEND short messages tagged [hil-e3d] to the owner chat, and some ask the human at
the terminal to reply in Telegram, so they only run on request. A message the owner must
answer says so in its own text ("HIL step N: use Reply on THIS message and send: ..."); all
others say "please ignore":

    scripts/hw.sh python tools/hil.py -m owner         (or EMBPET_HIL_OWNER=1)

Needs: Wi-Fi up, a stored token, a paired owner (tg status shows `owner`; tools/tg_pair.py
pairs one) and, for the reply steps, a terminal. Steps that need the human print a prompt;
typing `skip` skips that test. Every message a test sends is acknowledged in its teardown, so
the pending table is left as found. The reboot test also makes the board post its
"embpet online" notice to the owner. Telethon automation of the replies is not implemented
(bead embpet-vgj).
"""
import pytest

import hil_util as U

pytestmark = [pytest.mark.owner, pytest.mark.slow]

ACTOR = "hil-e3d"
TEXT = "embpet HIL test, please ignore"


@pytest.fixture()
def online(board, owner_paired, wifi_up):
    if board.cmd("tg status")["token"] == "":
        pytest.skip("no bot token stored")
    st = U.wait_for(lambda: board.cmd("tg status")["state"] == "online", 60.0, 2.0)
    assert st, "bridge is not online"


@pytest.fixture()
def sent(board, online):
    """Send one message; yields its Telegram message id; acknowledges it afterwards."""
    list(board.events(0))
    res = board.cmd("tg send", text=[TEXT], actor=ACTOR, timeout=20.0)
    assert res["ok"] is True, res
    try:
        yield res["msg"]
    finally:
        board.cmd("tg ack", "-m", str(res["msg"]))


def pending_of(board, actor=ACTOR):
    return board.cmd("tg pending", actor=actor)["items"]


def wait_event(board, name, seconds, **match):
    """First evt frame with ev == name (and the given fields) within SECONDS, else None."""
    for e in board.events(seconds):
        if e.get("ev") == name and all(e.get(k) == v for k, v in match.items()):
            return e
    return None


def prompt(request, text):
    answer = U.ask_owner(request, text)
    if answer.lower() == "skip":
        pytest.skip("skipped at the prompt")


def drain(board):
    """Drop the events seen so far, including those still in transit."""
    list(board.events(0.5))


class Step:
    """A message the owner must answer. Its own text says what to reply, so the owner never
    has to pick it out among the other [hil-e3d] messages (embpet-7eu)."""

    def __init__(self, request, board):
        self.request, self.board, self.sent = request, board, []
        self.text = self.answer = None

    def send(self, n, answer):
        self.answer = answer
        self.text = "HIL step %s: use Reply on THIS message and send: %s" % (n, answer)
        res = self.board.cmd("tg send", text=[self.text], actor=ACTOR, timeout=20.0)
        assert res["ok"] is True, res
        self.sent.append(res["msg"])
        drain(self.board)                   # this send's own pending event

    def ask(self, extra=""):
        prompt(self.request, '%sIn Telegram, use Reply on the bot message "[%s] %s" and send: %s'
               % (extra, ACTOR, self.text, self.answer))


@pytest.fixture()
def step(request, board, online):
    s = Step(request, board)
    yield s
    for msg in s.sent:
        board.cmd("tg ack", "-m", str(msg))


# ---- send / pending / ack: no human reply needed ----

def test_send_returns_a_message_id_and_the_message_is_pending(board, sent):
    assert isinstance(sent, int) and sent > 0
    items = pending_of(board)
    assert [i["msg"] for i in items] == [sent]
    assert items[0]["actor"] == ACTOR
    assert 0 <= items[0]["age_s"] < 60


def test_send_emits_a_pending_event_before_the_response(board, online):
    list(board.events(0))
    res = board.cmd("tg send", text=[TEXT], actor=ACTOR, timeout=20.0)
    try:
        events = list(board.events(0))
        pend = [e for e in events if e["ev"] == "pending"]
        assert pend and pend[-1]["count"] >= 1
        assert pend[-1]["seq"] < res["seq"]
    finally:
        board.cmd("tg ack", "-m", str(res["msg"]))


def test_pending_count_in_status_includes_the_sent_message(board, online):
    before = board.cmd("tg status")["pending"]
    res = board.cmd("tg send", text=[TEXT], actor=ACTOR, timeout=20.0)
    try:
        assert board.cmd("tg status")["pending"] == before + 1
    finally:
        board.cmd("tg ack", "-m", str(res["msg"]))


def test_ack_by_message_id_removes_it_from_pending(board, sent):
    res = board.cmd("tg ack", "-m", str(sent))
    assert res["acked"] == 1
    assert pending_of(board) == []


def test_ack_by_message_id_twice_acks_nothing_the_second_time(board, sent):
    assert board.cmd("tg ack", "-m", str(sent))["acked"] == 1
    assert board.cmd("tg ack", "-m", str(sent))["acked"] == 0


def test_ack_emits_a_pending_event_with_the_new_count(board, sent):
    list(board.events(0))
    board.cmd("tg ack", "-m", str(sent))
    e = wait_event(board, "pending", 2.0)
    assert e is not None
    assert e["count"] == board.cmd("tg status")["pending"]


def test_pending_for_another_actor_does_not_list_the_message(board, sent):
    assert pending_of(board, actor="hil-other") == []


def test_send_as_a_reply_to_an_earlier_message_succeeds(board, sent):
    res = board.cmd("tg send", "-r", str(sent), text=["embpet HIL test, reply thread"],
                    actor=ACTOR, timeout=20.0)
    try:
        assert res["ok"] is True
        assert res["msg"] != sent
    finally:
        board.cmd("tg ack", "-m", str(res["msg"]))


def test_send_accepts_non_ascii_text_and_emoji(board, online):
    res = board.cmd("tg send", text=["Привет ✅ \U0001F600"],
                    actor=ACTOR, timeout=20.0)
    try:
        assert res["ok"] is True
    finally:
        board.cmd("tg ack", "-m", str(res["msg"]))


def test_longest_allowed_message_is_sent_and_one_more_character_is_too_long(board, online):
    actor = "a" * 16                    # "[" + 16 + "] " = 19 units, so 4077 characters fit
    over = board.cmd("tg send", text=["x" * 4078], actor=actor, timeout=20.0)
    assert over["err"] == "too_long"
    res = board.cmd("tg send", text=["x" * 4077], actor=actor, timeout=20.0)
    try:
        assert res["ok"] is True
    finally:
        board.cmd("tg ack", "-m", str(res["msg"]))


def test_status_is_online_with_the_bot_name(board, online):
    st = board.cmd("tg status")
    assert st["state"] == "online"
    assert st["detail"] == "@" + st["bot"]


# ---- the human replies ----

def test_owner_reply_is_routed_to_the_actor(board, step):
    step.send(1, "ok")
    step.ask()
    e = wait_event(board, "reply", 60.0, actor=ACTOR)
    assert e is not None, "no reply event within 60 s"
    assert e["to"] == step.sent[0], "the Reply went to another message"
    assert e["text"] == "ok"
    assert e["src"] == "table"
    assert isinstance(e["msg"], int) and isinstance(e["date"], int)
    assert pending_of(board) == []


def test_reply_event_comes_before_the_pending_event_of_the_message_it_answers(board, step):
    step.send(2, "second")
    step.ask()
    seqs = {}
    for e in board.events(60.0):
        if e.get("ev") == "reply" and e.get("to") == step.sent[0]:
            seqs.setdefault("reply", e["seq"])
        elif e.get("ev") == "pending":
            seqs.setdefault("pending", e["seq"])
        if len(seqs) == 2:
            break
    assert set(seqs) == {"reply", "pending"}
    assert seqs["reply"] < seqs["pending"]


def test_message_without_reply_gets_a_hint_and_no_event(request, board, sent):
    prompt(request, "HIL step 3: in Telegram, write any new message to the bot WITHOUT using "
                    "Reply (the bot should answer with a hint to use Reply)")
    assert wait_event(board, "reply", 8.0) is None
    assert pending_of(board)[0]["msg"] == sent


def test_reply_after_a_reboot_is_routed_by_the_actor_prefix(board, step):
    step.send(4, "after reboot")
    U.reboot(board)
    step.ask(extra="The board just rebooted (its pending table is empty). ")
    e = wait_event(board, "reply", 60.0, actor=ACTOR)
    assert e is not None, "no reply event within 60 s"
    assert e["to"] == step.sent[0], "the Reply went to another message"
    assert e["text"] == "after reboot"
    assert e["src"] == "prefix"
