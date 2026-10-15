#!/usr/bin/env python3
"""Bind the owner Telegram chat to the board (embpet-z5d, README 2.1). The human step:

  scripts/hw.sh python tools/tg_pair.py [-p PORT] [--wait S]

Runs `tg pair`, prints the 6-digit code and what to do with it, then waits for the `paired`
event (default: as long as the code is valid, 5 min). The owner opens the bot in Telegram
and sends `/start <code>` from the chat that should receive the messages. The bot answers
there with a confirmation. A wrong code is ignored; after 5 wrong codes pairing is cancelled
and this tool has to be run again. The board needs a stored bot token and Wi-Fi
(tools/tg_from_env.py). The chat id is not printed. Anyone who knows the bot can send wrong
codes and cancel the pairing; run the tool again, or use `tg owner <chat_id>`. Exit 0 = paired, 1 = timed out or the
board refused `tg pair`, 2 = no board.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from embpet import Board, CmdTimeout, NoBoard  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-p", "--port")
    ap.add_argument("--wait", type=float, metavar="S",
                    help="give up after S seconds (default: the code's validity)")
    ap.add_argument("--test-ttl", type=int, metavar="S",
                    help="test build only: make the code valid for S seconds")
    a = ap.parse_args()
    try:
        with Board(a.port) as b:
            b.wait_ready(20)
            if a.test_ttl is not None:
                r = b.cmd("tg t_pairttl", str(a.test_ttl))
                if not r.get("ok"):
                    print("--test-ttl needs the test build; this firmware refused tg t_pairttl "
                          "(%s)" % r.get("err"), file=sys.stderr)
                    return 1
            owner_before = b.cmd("tg owner").get("chat_id")
            res = b.cmd("tg pair", timeout=20.0)
            if not res.get("ok"):
                print("tg pair refused: %s %s" % (res.get("err"), res.get("msg") or ""),
                      file=sys.stderr)
                return 1
            code, ttl = res["code"], res["expires_s"]
            st = b.cmd("tg status")
            bot = st.get("bot") or "your bot"
            print("Pairing code: %s (valid %d s)\n"
                  "In Telegram open @%s and send:  /start %s\n"
                  "Waiting for the board to confirm ..." % (code, ttl, bot.lstrip("@"), code),
                  flush=True)
            deadline = time.monotonic() + (a.wait if a.wait is not None else ttl + 2)
            while time.monotonic() < deadline:
                for e in b.events(min(1.0, max(0.1, deadline - time.monotonic()))):
                    if e.get("ev") == "paired":
                        print("Paired: the owner chat is bound; the bot confirmed in Telegram.")
                        return 0
            if b.cmd("tg owner").get("chat_id") != owner_before:
                print("The owner changed although no `paired` event was seen (check "
                      "`tg owner`); pairing probably succeeded.")
                return 0
    except CmdTimeout:
        print("no response from the board", file=sys.stderr)
        return 2
    except (NoBoard, OSError) as e:
        print("serial error: %s" % type(e).__name__, file=sys.stderr)
        return 2
    print("Not paired: no valid /start <code> arrived in time, or 5 wrong codes cancelled "
          "pairing (the board cannot tell which). Run this tool again (`tg pair` restarts it).")
    return 1


if __name__ == "__main__":
    sys.exit(main())
