#!/usr/bin/env python3
"""Offline test of FrameChecker (embpet_cli.py --check-frames). No board needed.
Run: scripts/idf.sh python tools/test_embpet_cli.py"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from embpet_cli import FrameChecker  # noqa: E402


def frame(seq, boot="aaaa0001"):
    return '@@{"t":"evt","seq":%d,"boot":"%s","ev":"stress"}' % (seq, boot)


def run(lines):
    c = FrameChecker()
    for ln in lines:
        c.feed(ln)
    return c


class FrameCheckerTest(unittest.TestCase):
    def test_good_sequence(self):
        c = run(["boot noise", frame(1), frame(2), "", frame(3)])
        self.assertEqual((c.frames, c.bad, c.gaps, c.embedded_text), (3, 0, 0, 0))
        self.assertTrue(c.ok())

    def test_log_text_with_at_at_is_ignored(self):
        c = run([frame(1),
                 'W (3081) stress: task 0 spam 0 with "@@{quotes}" and padding xxx',
                 frame(2)])
        self.assertEqual((c.frames, c.bad, c.embedded_text), (2, 0, 1))
        self.assertTrue(c.ok())

    def test_glued_frame_is_bad(self):
        c = run([frame(1), "W (5) x: partial " + frame(2)])
        self.assertEqual(c.bad, 1)
        self.assertEqual(c.embedded_text, 0)
        self.assertFalse(c.ok())

    def test_glued_after_embedded_text(self):
        c = run(['W (5) x: "@@{q}" then ' + frame(1)])
        self.assertEqual(c.bad, 1)

    def test_gap_fails(self):
        c = run([frame(1), frame(3)])
        self.assertEqual((c.bad, c.gaps), (0, 1))
        self.assertFalse(c.ok())

    def test_duplicate_is_bad(self):
        c = run([frame(1), frame(2), frame(2)])
        self.assertEqual(c.bad, 1)
        self.assertFalse(c.ok())

    def test_truncated_json_at_line_start_is_bad(self):
        c = run(['@@{"t":"evt","seq":1,"boo'])
        self.assertEqual(c.bad, 1)

    def test_new_boot_restarts_sequence(self):
        c = run([frame(5, "aaaa0001"), frame(1, "bbbb0002"), frame(2, "bbbb0002")])
        self.assertEqual((c.bad, c.gaps, c.frames), (0, 0, 3))

    def test_no_frames_is_not_ok(self):
        self.assertFalse(run(["hello"]).ok())


if __name__ == "__main__":
    unittest.main()
