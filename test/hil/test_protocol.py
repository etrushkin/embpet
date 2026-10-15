"""Serial protocol (README 3.2 - 3.4): frame format, rid/actor echo, errors, `evt since`.
Production firmware, no network needed."""
import json
import re

import pytest

import hil_util as U
from embpet import CmdTimeout, FrameChecker


def frames_of(lines):
    out = []
    for text in lines:
        m = U.FRAME_RE.match(text)
        if m:
            out.append(json.loads(m.group(1)))
    return out


# ---- frame format ----

def test_response_is_one_line_starting_with_at_at(board):
    lines, res = U.raw_exchange(board, "sys info -i fmt1", rid="fmt1")
    frame_lines = [ln for ln in lines if ln.startswith("@@")]
    assert res is not None
    assert len(frame_lines) >= 1
    assert U.FRAME_RE.match(frame_lines[-1])


def test_response_has_common_fields(board):
    res = board.cmd("sys info")
    assert res["t"] == "res"
    assert type(res["seq"]) is int and res["seq"] > 0
    assert re.fullmatch(r"[0-9a-f]{8}", res["boot"])
    assert res["cmd"] == "sys info"
    assert res["ok"] is True


def test_seq_increases_and_boot_id_is_stable_between_responses(board):
    a = board.cmd("sys info")
    b = board.cmd("sys info")
    assert b["seq"] > a["seq"]
    assert b["boot"] == a["boot"]


def test_sys_info_reports_the_documented_fields(board):
    res = board.cmd("sys info")
    for key in ("fw", "idf", "uptime_s", "heap_free", "heap_min", "mac", "reset_reason",
                "log", "log_dropped"):
        assert key in res, key
    assert re.fullmatch(r"([0-9a-f]{2}:){5}[0-9a-f]{2}", res["mac"])
    assert res["idf"].startswith("v5.5")


def test_frames_stay_intact_with_debug_logging_on(board):
    checker = FrameChecker()
    seen = []
    board.on_line = lambda text: (checker.feed(text), seen.append(text))
    try:
        board.cmd("sys log", "debug")
        for _ in range(4):
            for command in ("sys info", "wifi status", "tg status"):
                try:
                    board.cmd(command, timeout=10.0)
                except CmdTimeout:
                    pytest.fail("no response to %s; last lines: %r" % (command, seen[-10:]))
    finally:
        board.on_line = None
        board.cmd("sys log", "warn")
    assert checker.bad == 0, checker.problems
    assert checker.frames >= 10


def test_blank_line_gets_no_response(board):
    board.write_line("")
    assert [f for f in board.frames(0.6) if f.get("t") == "res"] == []
    assert board.cmd("sys info")["ok"] is True


def test_two_pipelined_commands_are_matched_by_rid(board):
    board.write_line("sys info -i pipa")
    board.write_line("wifi status -i pipb")
    got = {}
    for f in board.frames(10.0):
        if f.get("t") == "res":
            got[f["rid"]] = f
        if len(got) == 2:
            break
    assert set(got) == {"pipa", "pipb"}
    assert got["pipa"]["cmd"] == "sys info"
    assert got["pipb"]["cmd"] == "wifi status"


def test_help_lists_every_command_of_the_reference(board):
    res = board.cmd("help")
    assert res["ok"] is True
    expected = {"tg send", "tg pending", "tg ack", "tg token", "tg pair", "tg owner",
                "tg status", "wifi set", "wifi status", "wifi txpower", "io in", "io debounce",
                "io set", "io pulse", "io get", "io boot", "evt since", "sys info", "sys log",
                "sys reboot", "help"}
    assert expected <= set(res["cmds"])


# ---- rid / actor echo ----

def test_rid_of_32_characters_is_echoed(board):
    rid = "r" * 16 + "-" * 8 + "9" * 8
    assert board.cmd("sys info", rid=rid)["rid"] == rid


def test_actor_of_16_characters_is_echoed(board):
    assert board.cmd("sys info", actor="A_b-9" + "z" * 11)["actor"] == "A_b-9" + "z" * 11


def test_response_without_options_has_no_rid_or_actor_but_cmd(board):
    lines, res = U.raw_exchange(board, "sys info", timeout=10.0)
    assert res is not None
    assert "actor" not in res
    assert res["cmd"] == "sys info"


def test_rid_and_actor_are_echoed_on_a_failed_command(board):
    res = board.cmd("io set", "9", "on", rid="echo1", actor="who")
    assert res["ok"] is False
    assert res["rid"] == "echo1"
    assert res["actor"] == "who"


def test_actor_over_16_characters_is_rejected_and_not_echoed(board):
    _, res = U.raw_exchange(board, "sys info -i act17 -a " + "a" * 17, timeout=10.0)
    assert res["ok"] is False
    assert res["err"] == "bad_args"
    assert "actor" not in res


def test_actor_with_invalid_character_is_rejected(board):
    _, res = U.raw_exchange(board, "sys info -a bad!actor", timeout=10.0)
    assert res["ok"] is False
    assert res["err"] == "bad_args"


def test_rid_over_32_characters_is_rejected_and_not_echoed(board):
    _, res = U.raw_exchange(board, "sys info -i " + "r" * 33, timeout=10.0)
    assert res["ok"] is False
    assert res["err"] == "bad_args"
    assert "rid" not in res


# ---- errors ----

def test_unknown_command_gives_bad_args_with_the_typed_words(board):
    res = board.cmd("sys nope", rid="unk1")
    assert res["ok"] is False
    assert res["err"] == "bad_args"
    assert res["cmd"] == "sys nope"
    assert res["rid"] == "unk1"
    assert isinstance(res["msg"], str) and res["msg"]


def test_unknown_single_word_command_gives_bad_args(board):
    res = board.cmd("frobnicate")
    assert res["ok"] is False
    assert res["err"] == "bad_args"


def test_missing_required_argument_gives_bad_args(board):
    assert board.cmd("evt since")["err"] == "bad_args"


def test_unknown_option_gives_bad_args(board):
    assert board.cmd("sys info", "--bogus")["err"] == "bad_args"


def test_tg_send_without_actor_gives_bad_args_before_anything_else(board):
    res = board.cmd("tg send", text=["hil protocol test"])
    assert res["ok"] is False
    assert res["err"] == "bad_args"


def test_error_response_has_human_readable_msg(board):
    res = board.cmd("io set", "9", "on")
    assert res["err"] == "bad_pin"
    assert isinstance(res["msg"], str) and res["msg"]


@pytest.mark.parametrize("line", [
    'sys log "\\q"',                  # unknown escape
    'sys log "warn',                  # missing closing quote
    'sys log "warn"x',                # text glued to the closing quote
    'sys log "warn\\',                # backslash at the end of the line
])
def test_bad_quoting_gives_bad_args(board, line):
    _, res = U.raw_exchange(board, line, timeout=10.0)
    assert res["ok"] is False
    assert res["err"] == "bad_args"


def test_bad_quoting_leaves_the_log_level_unchanged(board):
    before = board.cmd("sys info")["log"]
    U.raw_exchange(board, 'sys log "debug', timeout=10.0)
    assert board.cmd("sys info")["log"] == before


def test_line_over_4400_bytes_gives_too_long_and_console_recovers(board):
    _, res = U.raw_exchange(board, 'sys log "' + "x" * 4500 + '"', timeout=15.0)
    assert res["ok"] is False
    assert res["err"] == "too_long"
    assert board.cmd("sys info")["ok"] is True


def test_non_ascii_argument_gives_a_normal_error_frame(board):
    res = board.cmd("sys log", "привет")
    assert res["ok"] is False
    assert res["err"] == "bad_args"


# ---- sys log ----

def test_sys_log_sets_and_reports_the_level(board):
    try:
        for level in ("info", "debug", "error"):
            res = board.cmd("sys log", level)
            assert res["ok"] is True
            assert res["level"] == level
            assert board.cmd("sys info")["log"] == level
    finally:
        board.cmd("sys log", "warn")


def test_sys_log_rejects_an_unknown_level(board):
    assert board.cmd("sys log", "loud")["err"] == "bad_args"


# ---- evt since ----

def replay(board, since):
    """Run `evt since SINCE`; returns (res, replayed evt frames)."""
    list(board.events(0))                   # drop queued events
    res = board.cmd("evt since", str(since))
    return res, list(board.events(0))


def test_evt_since_zero_replays_buffered_events_in_seq_order(board):
    res, evts = replay(board, 0)
    assert res["ok"] is True
    assert res["count"] == len(evts)
    seqs = [e["seq"] for e in evts]
    assert seqs == sorted(seqs) and len(set(seqs)) == len(seqs)
    assert all(e["t"] == "evt" and "ev" in e for e in evts)
    assert all(e["boot"] == res["boot"] for e in evts)
    assert all(s < res["seq"] for s in seqs)


def test_evt_since_replays_only_newer_events(board):
    _, all_evts = replay(board, 0)
    assert len(all_evts) >= 2, "need at least two buffered events (the boot event and one more)"
    mid = all_evts[len(all_evts) // 2]["seq"]
    res, evts = replay(board, mid)
    assert len(evts) < len(all_evts)
    assert all(e["seq"] > mid for e in evts)
    assert res["count"] == len(evts)


def test_evt_since_replay_is_verbatim(board):
    _, first = replay(board, 0)
    _, second = replay(board, 0)
    by_seq = {e["seq"]: e for e in second}
    common = [e for e in first if e["seq"] in by_seq]
    assert common, "no common events between two replays"
    assert all(by_seq[e["seq"]] == e for e in common)


def test_evt_since_beyond_the_last_event_returns_nothing(board):
    res, evts = replay(board, 4294967295)
    assert res["ok"] is True
    assert res["count"] == 0 and evts == []
    assert res["truncated"] is False


@pytest.mark.parametrize("arg", ["abc", "-1", "4294967296", "1.5"])
def test_evt_since_rejects_a_bad_seq(board, arg):
    res = board.cmd("evt since", text=[arg])
    assert res["ok"] is False
    assert res["err"] == "bad_args"


# ---- after a reboot (one reboot shared by these tests) ----

@pytest.fixture(scope="module")
def rebooted(board):
    """(sys info before, sys info after) around one `sys log info` + `sys reboot`."""
    board.cmd("sys log", "info")
    before = board.cmd("sys info")
    return before, U.reboot(board)


@pytest.mark.slow
def test_reboot_gives_a_new_boot_id_and_a_small_uptime(rebooted):
    before, after = rebooted
    assert after["boot"] != before["boot"]
    assert after["uptime_s"] < 30


@pytest.mark.slow
def test_reboot_resets_the_log_level_to_warn(rebooted):
    assert rebooted[0]["log"] == "info"
    assert rebooted[1]["log"] == "warn"


@pytest.mark.slow
def test_boot_event_after_reboot_has_fw_idf_and_reset_reason(board, rebooted):
    _, evts = replay(board, 0)
    boots = [e for e in evts if e["ev"] == "boot"]
    assert len(boots) == 1
    assert boots[0]["fw"] == rebooted[1]["fw"]
    assert boots[0]["idf"] == rebooted[1]["idf"]
    assert boots[0]["reset_reason"] == "software"


@pytest.mark.slow
def test_evt_since_is_not_truncated_while_the_ring_has_not_overflowed(board, rebooted):
    res, evts = replay(board, 0)
    assert res["truncated"] is False
    assert res["count"] == len(evts) >= 1
