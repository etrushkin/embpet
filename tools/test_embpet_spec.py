#!/usr/bin/env python3
"""Offline tests that tools/embpet follows SPEC.md: port selection by USB serial number (2),
line checks (4.2, 4.3) and Board.reconnect() with `evt since` replay (7.5). Fake serial, no board.
Run: python3 -m pytest tools/test_embpet_spec.py"""
import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from embpet import AmbiguousPort, Board, NoBoard, build_line, find_port  # noqa: E402
from embpet import board as board_mod  # noqa: E402
from test_embpet_cli import FakeBoardSerial, rid_of  # noqa: E402

A, B = "E8:3D:C1:8B:8E:D4", "E8:F6:0A:E7:5B:C6"


class FindPortTest(unittest.TestCase):
    def test_none_without_candidates(self):
        self.assertIsNone(find_port(candidates=[]))

    def test_single_candidate_without_serial(self):
        self.assertEqual(find_port(candidates=[("/dev/ttyACM0", A)]), "/dev/ttyACM0")

    def test_selects_by_serial_not_first(self):
        c = [("/dev/ttyACM0", A), ("/dev/ttyACM1", B)]
        self.assertEqual(find_port(B, candidates=c), "/dev/ttyACM1")
        self.assertEqual(find_port(B.lower(), candidates=c), "/dev/ttyACM1")

    def test_serial_without_match_is_none(self):
        self.assertIsNone(find_port("00:00", candidates=[("/dev/ttyACM0", A)]))

    def test_several_without_serial_refuses_and_lists_them(self):
        c = [("/dev/ttyACM0", A), ("/dev/ttyACM1", B)]
        with self.assertRaises(AmbiguousPort) as cm:
            find_port(candidates=c)
        msg = str(cm.exception)
        self.assertIn("/dev/ttyACM0", msg)
        self.assertIn(B, msg)
        self.assertIsInstance(cm.exception, NoBoard)

    def test_same_serial_twice_refuses(self):
        with self.assertRaises(AmbiguousPort):
            find_port(A, candidates=[("/dev/ttyACM0", A), ("/dev/ttyACM1", A)])


class BuildLineChecksTest(unittest.TestCase):
    def test_rid_format(self):
        for bad in ("", "a b", "x" * 33, 'a"b', "é"):
            with self.assertRaises(ValueError, msg=repr(bad)):
                build_line(["sys", "info"], rid=bad)
        self.assertIn("-i " + "x" * 32, build_line(["sys", "info"], rid="x" * 32))

    def test_actor_format(self):
        for bad in ("", "a b", "x" * 17, "a;b"):
            with self.assertRaises(ValueError, msg=repr(bad)):
                build_line(["tg", "send"], rid="r", actor=bad)
        self.assertIn("-a " + "A_-" + "x" * 13, build_line(["tg", "send"], actor="A_-" + "x" * 13))

    def test_token_limit(self):
        words = ["io", "set"]
        ok = build_line(words, ["1"] * 29)          # 2 + 29 = 31 tokens
        self.assertEqual(len(ok.split()), 31)
        with self.assertRaises(ValueError):
            build_line(words, ["1"] * 30)

    def test_token_limit_counts_rid_actor_and_separator(self):
        # 2 words + -i r + -a a (4) + "--" (1) + 24 texts = 31
        build_line(["tg", "send"], rid="r", actor="a", text=["x"] * 24)
        with self.assertRaises(ValueError):
            build_line(["tg", "send"], rid="r", actor="a", text=["x"] * 25)


def make_replaying(since_log, boots):
    """Serial whose board has the events of `boots['evts']` and answers sys info/evt since."""
    def replies(line):
        rid = rid_of(line)
        words = line.split()
        if line.startswith("sys info"):
            return ['@@{"t":"res","seq":50,"boot":"%s","rid":"%s","ok":true}' % (boots["boot"], rid)]
        if line.startswith("evt since"):
            since = int(words[-1].strip('"'))
            since_log.append(since)
            out = ['@@{"t":"evt","seq":%d,"boot":"%s","ev":"input"}' % (q, boots["boot"])
                   for q in boots["evts"] if q > since]
            return out + ['@@{"t":"res","seq":60,"boot":"%s","rid":"%s","ok":true,"count":%d}'
                          % (boots["boot"], rid, len(out))]
        return []
    return type("S", (FakeBoardSerial,), {"replies": staticmethod(replies)})


class ReconnectTest(unittest.TestCase):
    def board(self, boots, serial_number=A, ports=None):
        since = []
        cls = make_replaying(since, boots)
        b = Board("/dev/ttyACM0", serial_cls=cls, serial_number=serial_number)
        return b, since

    def test_finds_board_again_by_serial_number(self):
        b, _ = self.board({"boot": "b1", "evts": []})
        opened = []
        orig = board_mod.open_port

        def spy(port, serial_cls=None):
            opened.append(port)
            return orig(port, serial_cls=serial_cls)

        cands = [("/dev/ttyACM0", B), ("/dev/ttyACM1", A)]    # another board took the old path
        with mock.patch("embpet.board.time.sleep"), mock.patch.object(board_mod, "open_port", spy), \
                mock.patch("embpet.serialport.list_candidates", lambda: cands):
            b.reconnect(delay=0, timeout=5)
        self.assertEqual(opened, ["/dev/ttyACM1"])
        self.assertEqual(b.port, "/dev/ttyACM1")

    def test_waits_until_the_serial_number_reappears(self):
        b, _ = self.board({"boot": "b1", "evts": []})
        seq = iter([[], [("/dev/ttyACM0", B)], [("/dev/ttyACM2", A)]])
        with mock.patch("embpet.board.time.sleep"), \
                mock.patch("embpet.serialport.list_candidates", lambda: next(seq)):
            b.reconnect(delay=0, timeout=30)
        self.assertEqual(b.port, "/dev/ttyACM2")

    def test_times_out_when_only_other_boards_are_there(self):
        b, _ = self.board({"boot": "b1", "evts": []})
        ticks = iter(range(1000))
        with mock.patch("embpet.board.time.sleep"), \
                mock.patch("embpet.board.time.monotonic", lambda: next(ticks)), \
                mock.patch("embpet.serialport.list_candidates", lambda: [("/dev/ttyACM0", B)]):
            with self.assertRaises(NoBoard):
                b.reconnect(delay=0, timeout=3)

    def test_replays_missed_events_same_boot_without_duplicates(self):
        boots = {"boot": "b1", "evts": [1, 2, 3, 4]}
        b, since = self.board(boots)
        # events 1, 2 were seen before the reset; 3, 4 are missed
        for q in (1, 2):
            b._push_event({"t": "evt", "seq": q, "boot": "b1"})
            b._track({"t": "evt", "seq": q, "boot": "b1"})
        boots["evts"] = [1, 2, 3, 4]
        with mock.patch("embpet.board.time.sleep"), \
                mock.patch("embpet.serialport.list_candidates", lambda: [("/dev/ttyACM0", A)]):
            b.reconnect(delay=0, timeout=5)
        self.assertEqual(since, [2])
        self.assertEqual([e["seq"] for e in b.events(0)], [1, 2, 3, 4])
        self.assertEqual(b.last_replay["count"], 2)

    def test_new_boot_replays_from_zero(self):
        boots = {"boot": "b2", "evts": [1, 2]}
        b, since = self.board(boots)
        b._push_event({"t": "evt", "seq": 7, "boot": "b1"})
        b._track({"t": "evt", "seq": 7, "boot": "b1"})
        with mock.patch("embpet.board.time.sleep"), \
                mock.patch("embpet.serialport.list_candidates", lambda: [("/dev/ttyACM0", A)]):
            b.reconnect(delay=0, timeout=5)
        self.assertEqual(since, [0])
        self.assertEqual([(e["boot"], e["seq"]) for e in b.events(0)],
                         [("b1", 7), ("b2", 1), ("b2", 2)])

    def test_duplicate_event_frames_are_delivered_once(self):
        b, _ = self.board({"boot": "b1", "evts": []})
        for _ in range(2):
            b._push_event({"t": "evt", "seq": 5, "boot": "b1"})
        self.assertEqual(len(list(b.events(0))), 1)


if __name__ == "__main__":
    unittest.main()
