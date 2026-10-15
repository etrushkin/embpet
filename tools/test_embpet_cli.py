#!/usr/bin/env python3
"""Offline tests of embpet_cli.py: FrameChecker (--check-frames) and the DTR/RTS order of
open_port. No board needed.
Run: scripts/idf.sh python tools/test_embpet_cli.py"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from embpet_cli import FrameChecker, open_port  # noqa: E402
from serial.serialutil import SerialBase  # noqa: E402


class FakePosixSerial(SerialBase):
    """Models the modem-line handling of pyserial's serialposix.Serial.open() on macOS:
    the OS asserts DTR and RTS when the port opens, then open() applies DTR (unless
    dsrdtr) and then RTS (unless rtscts). Setting dtr/rts on an open port applies it at
    once. Records every (dtr, rts) state the device sees."""

    def open(self):
        self.lines = [(1, 1)]
        self.is_open = True
        if not self._dsrdtr:
            self._update_dtr_state()
        if not self._rtscts:
            self._update_rts_state()

    def close(self):
        self.is_open = False

    def _reconfigure_port(self, force_update=False):
        pass

    def _set(self, dtr=None, rts=None):
        d, r = self.lines[-1]
        self.lines.append((d if dtr is None else int(dtr), r if rts is None else int(rts)))

    def _update_dtr_state(self):
        self._set(dtr=self._dtr_state)

    def _update_rts_state(self):
        self._set(rts=self._rts_state)


class OpenPortTest(unittest.TestCase):
    def test_plain_pyserial_order_resets(self):
        # The bug (embpet-s2j.1): dtr=rts=False before open() passes through (0,1).
        s = FakePosixSerial()
        s.dtr = False
        s.rts = False
        s.open()
        self.assertIn((0, 1), s.lines)

    def test_open_port_never_presents_reset_state(self):
        s = open_port("/dev/null", serial_cls=FakePosixSerial)
        self.assertNotIn((0, 1), s.lines)           # RTS=1 with DTR=0 resets the chip
        self.assertEqual(s.lines[-1], (0, 0))
        self.assertFalse(s.dsrdtr)

    def test_open_port_closes_on_line_setup_error(self):
        class Failing(FakePosixSerial):
            def _update_dtr_state(self):
                raise OSError(5, "ioctl failed")

        made = []

        def factory():
            made.append(Failing())
            return made[-1]

        with self.assertRaises(OSError):
            open_port("/dev/null", serial_cls=factory)
        self.assertFalse(made[0].is_open)


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
