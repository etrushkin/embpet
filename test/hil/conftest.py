"""Fixtures for the hardware-in-the-loop tests.

Run through the board lock:  scripts/hw.sh python tools/hil.py [pytest args]
The port is $EMBPET_PORT, else the first /dev/cu.usbmodem* (or /dev/ttyACM*). Every test
that uses the `board` fixture is skipped when no board is found or the port cannot be
opened, except that an explicit $EMBPET_PORT (or EMBPET_REQUIRE_BOARD=1, set by
`tools/hil.py --require-board`) that is missing or unopenable fails instead.

The suites target the production firmware. See test/hil/README.md for the markers, the
owner and loopback selections, and how the board state is restored.
"""
import os
import re
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
from embpet import Board, find_port  # noqa: E402

import serial  # noqa: E402

import hil_util as U  # noqa: E402
import tg_from_env as T  # noqa: E402


def _selected(config, marker, env_var):
    """True when tests with MARKER were asked for: -m names it without `not`, or ENV_VAR=1."""
    if os.environ.get(env_var) == "1":
        return True
    expr = config.getoption("markexpr") or ""
    return re.search(r"(?<!not )\b%s\b" % marker, expr) is not None


def pytest_collection_modifyitems(config, items):
    """owner and loopback tests are skipped unless asked for (-m owner / EMBPET_HIL_OWNER=1,
    -m loopback / EMBPET_HIL_LOOPBACK=1)."""
    skips = {
        "owner": (_selected(config, "owner", "EMBPET_HIL_OWNER"),
                  "needs the paired Telegram owner: run with -m owner (or EMBPET_HIL_OWNER=1)"),
        "loopback": (_selected(config, "loopback", "EMBPET_HIL_LOOPBACK"),
                     "needs OUTn->opto->IN wiring: run with -m loopback "
                     "(or EMBPET_HIL_LOOPBACK=1)"),
    }
    for item in items:
        for name, (wanted, reason) in skips.items():
            if name in item.keywords and not wanted:
                item.add_marker(pytest.mark.skip(reason=reason))


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


@pytest.fixture(scope="session")
def env():
    """The values of .env.local ({} if missing). Secrets in it are never printed."""
    return U.load_env()


def snapshot(board):
    """The settings the suites may change, as the board reports them now."""
    st = board.cmd("tg status")
    wifi = U.wifi_status(board)
    return {
        "txpower": wifi["txpower"],
        "log": board.cmd("sys info")["log"],
        "outs": U.out_levels(board),
        "boot": board.cmd("io boot")["levels"],
        "debounce": board.cmd("io debounce")["debounce"],
        "owner": st["owner"],
        "token": st["token"],
        "ssid": wifi.get("ssid", ""),
    }


def restore_io(board, snap):
    """Boot levels, debounce times and output levels back to SNAP (errors are collected)."""
    errors = []
    try:
        now_boot = board.cmd("io boot")["levels"]
        now_deb = board.cmd("io debounce")["debounce"]
        now_out = board.cmd("io get")["out"]
    except Exception as e:
        return ["cannot read the io state: %r" % e]
    todo = [("io boot", pin, level) for pin, level in snap["boot"].items()
            if now_boot.get(pin) != level]
    todo += [("io debounce", pin, str(ms)) for pin, ms in snap["debounce"].items()
             if now_deb.get(pin) != ms]
    # io set also cancels a running pulse, so pulsing pins are set again even at their level
    todo += [("io set", str(o["pin"]), snap["outs"][o["pin"]]) for o in now_out
             if o["level"] != snap["outs"][o["pin"]] or o["pulsing"]]
    for cmd, pin, value in todo:
        try:
            board.cmd(cmd, pin, value)
        except Exception as e:      # keep restoring the rest
            errors.append("%s %s: %r" % (cmd, pin, e))
    return errors


def restore_owner(board, snap):
    if board.cmd("tg status")["owner"] == snap["owner"]:
        return
    if snap["owner"] is None:
        res = board.cmd("tg owner", "--clear")
    else:
        res = board.cmd("tg owner", text=[str(snap["owner"])])
    assert res["ok"], res


@pytest.fixture(scope="session", autouse=True)
def board_guard(board, env):
    """Snapshot of the board settings taken before the first test; everything is put back
    after the last one, even when tests failed: boot levels, debounce times, output levels,
    log level, owner, Wi-Fi (credentials from .env.local) and the bot token."""
    snap = snapshot(board)
    yield snap
    errors = restore_io(board, snap)
    for what, fn in (
            ("log", lambda: board.cmd("sys log", snap["log"])),
            ("txpower", lambda: U.wifi_status(board)["txpower"] == snap["txpower"] or
             board.cmd("wifi txpower", str(snap["txpower"]))),
            ("owner", lambda: restore_owner(board, snap))):
        try:
            fn()
        except Exception as e:
            errors.append("%s: %r" % (what, e))
    if env.get("WIFI_SSID") and snap["ssid"] == env["WIFI_SSID"]:
        st = U.ensure_wifi_up(board, env)
        if st.get("state") != "up":
            errors.append("wifi did not come back up")
    try:
        now = board.cmd("tg status")["token"]
        if now != snap["token"]:
            # the board never reveals the token: it can be put back only if the original
            # was the one in .env.local (compared by its masked form)
            if snap["token"] == "":
                errors.append("the bot token changed and was empty at the start: cannot restore")
            elif env.get("TG_TOKEN") and snap["token"] == T.masked(env["TG_TOKEN"]):
                code, res, err = T.set_token(env["TG_TOKEN"], board=board)
                if code != 0:
                    errors.append("token restore failed (exit %d)" % code)
            else:
                errors.append("the bot token changed and the original is not the .env.local "
                              "one: cannot restore")
    except Exception as e:
        errors.append("token: %r" % e)
    assert not errors, "board state not fully restored: %s" % "; ".join(errors)


@pytest.fixture()
def restore_io_state(board, board_guard):
    """Use in tests that change outputs, boot levels or debounce times."""
    yield
    errors = restore_io(board, board_guard)
    assert not errors, errors


@pytest.fixture(scope="session")
def wifi_creds(env, board_guard):
    """The .env.local Wi-Fi values, for tests that set the credentials. Skips when the
    board starts on a different network (its password is unknown, so it could not be put
    back). A board with no SSID ends up on the .env.local network."""
    if not env.get("WIFI_SSID") or "WIFI_PASS" not in env:
        pytest.skip("WIFI_SSID / WIFI_PASS missing in .env.local")
    if board_guard["ssid"] not in ("", env["WIFI_SSID"]):
        pytest.skip("the board is on another Wi-Fi network than .env.local: not changed")
    return env


@pytest.fixture(scope="session")
def wifi_secured(wifi_creds):
    """Like wifi_creds, for tests that need a wrong password: skips for an open network."""
    if not wifi_creds["WIFI_PASS"]:
        pytest.skip("WIFI_PASS is empty (open network): no wrong password to use")
    return wifi_creds


@pytest.fixture()
def wifi_up(board, wifi_creds):
    """Wi-Fi up with the .env.local credentials (provisioned if needed)."""
    st = U.ensure_wifi_up(board, wifi_creds)
    assert st.get("state") == "up", "wifi not up: %r" % {k: v for k, v in st.items() if k != "ssid"}
    return st


@pytest.fixture()
def wifi_restore(board, wifi_creds):
    """Use in tests that break Wi-Fi: the .env.local credentials are set again and the
    link waited for afterwards, even when the test failed."""
    yield
    st = U.set_wifi_from_env(board, wifi_creds)
    assert st["ok"] is True
    st = U.wait_wifi(board, "up", 90.0)
    assert st.get("state") == "up", "wifi did not recover"


@pytest.fixture()
def owner_paired(board):
    """The owner chat id; skips when nobody is paired (tools/tg_pair.py pairs one)."""
    owner = U.owner_chat(board)
    if owner is None:
        pytest.skip("no owner paired (run tools/tg_pair.py); nothing may be sent")
    if owner == 1:
        pytest.skip("owner is the placeholder chat 1, left by an interrupted test: clear it "
                    "with `tg owner --clear`, then pair the real owner")
    return owner
