"""Helpers shared by the HIL tests: waiting, raw console access, reboot, secrets from
.env.local (never printed), owner prompts."""
import os
import re
import sys
import time

import pytest

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools")
sys.path.insert(0, TOOLS)
import wifi_from_env as W  # noqa: E402

ROOT = W.ROOT
ENV_FILE = os.path.join(ROOT, ".env.local")
OUT_PINS = tuple(range(8))
IN_PINS = (10, 20, 21)
FRAME_RE = re.compile(r"^@@(\{.*\})\r?$")


def load_env():
    """The values of .env.local, or {} when it is missing."""
    try:
        return W.load_env(ENV_FILE)
    except OSError:
        return {}


def wait_for(fn, timeout, interval=1.0):
    """Call FN until it returns something truthy; return that, or the last (falsy) value
    after TIMEOUT seconds."""
    deadline = time.monotonic() + timeout
    while True:
        val = fn()
        if val or time.monotonic() >= deadline:
            return val
        time.sleep(interval)


def wifi_status(board):
    return board.cmd("wifi status", timeout=10.0)


def wait_wifi(board, state, timeout):
    """Poll `wifi status` until its state is STATE; returns the last status res."""
    last = {}

    def check():
        nonlocal last
        last = wifi_status(board)
        return last.get("state") == state
    wait_for(check, timeout, 1.5)
    return last


def set_wifi_from_env(board, env):
    """`wifi set -- ssid pass` in-process (values never in argv)."""
    words = [env["WIFI_SSID"]] + ([env["WIFI_PASS"]] if env.get("WIFI_PASS") else [])
    return board.cmd("wifi set", text=words, timeout=20.0)


def wrong_password(password):
    """A password that differs from PASSWORD (which must not be empty) in its last character."""
    assert password, "an open network has no wrong password"
    return password[:-1] + ("a" if password[-1] != "a" else "b")


def ensure_wifi_up(board, env, timeout=60.0):
    """Wi-Fi up with the credentials from .env.local; re-provisions them when needed.
    Returns the last status."""
    st = wait_wifi(board, "up", 12.0)
    if st.get("state") == "up" and (not env.get("WIFI_SSID") or st.get("ssid") == env["WIFI_SSID"]):
        return st
    if env.get("WIFI_SSID"):
        set_wifi_from_env(board, env)
    return wait_wifi(board, "up", timeout)


def out_levels(board):
    """{pin: 'on'|'off'} from `io get`."""
    res = board.cmd("io get")
    return {o["pin"]: o["level"] for o in res["out"]}


def raw_exchange(board, line, timeout=15.0, rid=None):
    """Write one console LINE as is and return (all lines read until the res with RID or
    until TIMEOUT, the parsed res or None). For tests that need the unparsed lines or a
    line the Board.cmd builder refuses."""
    import json
    board.write_line(line)
    lines = []
    res = None
    for text in board.lines(timeout):
        lines.append(text)
        m = FRAME_RE.match(text)
        if m:
            try:
                f = json.loads(m.group(1))
            except ValueError:
                continue
            if f.get("t") == "res" and (rid is None or f.get("rid") == rid):
                res = f
                break
    return lines, res


def reboot(board, timeout=40.0):
    """`sys reboot`, reconnect, and wait until the new boot answers. Returns that `sys info`
    res (its `boot` differs from the old one)."""
    old = board.cmd("sys info")["boot"]
    res = board.cmd("sys reboot")
    assert res["ok"] is True
    board.reconnect()
    info = board.wait_ready(timeout)
    assert info["boot"] != old
    board.flush_input()
    return info


def owner_chat(board):
    """The paired owner chat id, or None."""
    return board.cmd("tg status")["owner"]


def ask_owner(request, text):
    """Interactive prompt for the human at the terminal (output capture is suspended while
    it waits). Skips the test when there is no terminal. Returns the line the human typed."""
    capman = request.config.pluginmanager.getplugin("capturemanager")
    with capman.global_and_fixture_disabled():
        # pytest's capture replaces sys.stdin, so ask the real file descriptor
        if not os.isatty(0):
            answer = None
        else:
            print("\n>>> OWNER ACTION: %s" % text)
            answer = input(">>> press Enter when done (or type 'skip'): ").strip()
    if answer is None:
        pytest.skip("owner step needs a terminal: " + text)
    return answer
