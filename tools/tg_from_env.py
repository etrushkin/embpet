#!/usr/bin/env python3
"""Store the bot token from the git-ignored .env.local on the board without ever printing it
(embpet-q9h). Agents use this instead of sourcing .env.local.

  scripts/hw.sh python tools/tg_from_env.py [-p PORT] [--env FILE] [--cmd "tg t_token"]

Reads TG_TOKEN from .env.local itself (same parser as tools/wifi_from_env.py) and sends
`<cmd> -- <token>` in-process with the tools/embpet package, so the token is never in argv
(not visible in `ps`). --cmd defaults to the test-build command `tg t_token`
(CONFIG_EMBPET_TG_TEST); pass "tg token" once the production command exists. Prints one
JSON line, the res frame with the token masked as cfg does (123456:AA…xyz) and any other
occurrence replaced by ***. Exit 0 = stored, 1 = the board refused it, 2 = .env.local
unreadable / TG_TOKEN missing / no response.
"""
import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from embpet import Board, CmdTimeout, NoBoard  # noqa: E402
import wifi_from_env as W  # noqa: E402

TOKEN_RE = re.compile(r"^(\d{1,12}):([A-Za-z0-9_-]{16,})$")


def load_token(path):
    """TG_TOKEN from .env.local, or None if missing."""
    tok = W.load_env(path).get("TG_TOKEN", "")
    return tok or None


def masked(token):
    """cfg_mask_token's format: <id>:<first 2>…<last 3> of the secret, else ***."""
    m = TOKEN_RE.match(token)
    if not m:
        return "***"
    return "%s:%s…%s" % (m.group(1), m.group(2)[:2], m.group(2)[-3:])


def secrets_of(token):
    """Strings that must never appear in output: the token, its secret part, and a middle
    slice of the secret (catches partial leaks)."""
    m = TOKEN_RE.match(token)
    out = [token]
    if m:
        s = m.group(2)
        out += [s, s[len(s) // 2 - 4:len(s) // 2 + 4]]
    return out


def set_token(token, port=None, cmd="tg t_token", board=None):
    """Send `<cmd> -- <token>` in-process. Returns (exit code, res frame or None, error text);
    the error text never contains the token."""
    try:
        if board is not None:
            res = board.cmd(cmd, text=[token], timeout=20.0)
        else:
            with Board(port) as b:
                res = b.cmd(cmd, text=[token], timeout=20.0)
    except CmdTimeout:
        return 2, None, "timeout: no response to %s" % cmd
    except (NoBoard, OSError) as e:
        return 2, None, "serial error: %s" % type(e).__name__
    return (0 if res.get("ok") else 1), res, ""


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-p", "--port")
    ap.add_argument("--env", default=os.path.join(W.ROOT, ".env.local"))
    ap.add_argument("--cmd", default="tg t_token")
    a = ap.parse_args()
    try:
        token = load_token(a.env)
    except OSError as e:
        print("cannot read %s: %s" % (os.path.basename(a.env), e.strerror), file=sys.stderr)
        return 2
    if not token:
        print("TG_TOKEN is empty or missing in %s" % os.path.basename(a.env), file=sys.stderr)
        return 2
    code, res, err = set_token(token, a.port, a.cmd)
    if res is None:
        print(err, file=sys.stderr)
        return 2
    print(json.dumps(W.mask(res, secrets_of(token)), ensure_ascii=False))
    return code


if __name__ == "__main__":
    sys.exit(main())
