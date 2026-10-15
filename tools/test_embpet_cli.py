#!/usr/bin/env python3
"""Offline tests of the tools/embpet package and embpet_cli.py: console line building,
frame parsing, Board.cmd/events against a scripted serial port, FrameChecker
(--check-frames) and the DTR/RTS order of open_port. No board needed.
Run: scripts/idf.sh python tools/test_embpet_cli.py  (or pytest tools/test_embpet_cli.py)"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from embpet import (Board, CmdTimeout, FrameChecker, build_line, open_port,  # noqa: E402
                    parse_frame, quote, split_command)
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


class QuoteTest(unittest.TestCase):
    def test_escapes_quote_backslash_and_newline(self):
        self.assertEqual(quote('a"b\\c\nd'), '"a\\"b\\\\c\\nd"')

    def test_utf8_passes_through(self):
        self.assertEqual(quote("héllo"), '"héllo"')


class QuoteControlCharTest(unittest.TestCase):
    def test_carriage_return_is_rejected(self):
        with self.assertRaises(ValueError):
            quote("a\rb")

    def test_backspace_and_del_are_rejected(self):
        for ch in ("\x08", "\x7f", "\x1b", "\x00"):
            with self.assertRaises(ValueError, msg=repr(ch)):
                quote("a" + ch)

    def test_tab_is_allowed(self):
        self.assertEqual(quote("a\tb"), '"a\tb"')


class LineLengthTest(unittest.TestCase):
    def test_line_over_firmware_limit_is_rejected(self):
        with self.assertRaises(ValueError):
            build_line(["wifi", "set"], text=["x" * 4400])

    def test_line_at_firmware_limit_is_accepted(self):
        base = len(build_line(["wifi", "set"], text=[""]))
        line = build_line(["wifi", "set"], text=["x" * (4400 - base)])
        self.assertEqual(len(line), 4400)

    def test_limit_counts_utf8_bytes(self):
        with self.assertRaises(ValueError):
            build_line(["wifi", "set"], text=["é" * 2200])

    def test_cmd_does_not_write_a_rejected_line(self):
        b = make_board(lambda ln: [])
        with self.assertRaises(ValueError):
            b.cmd("tg send", text=["a\rsys reboot"])
        self.assertEqual(b._s.written, [])


class SplitCommandTest(unittest.TestCase):
    def test_group_takes_two_words(self):
        self.assertEqual(split_command(["wifi", "set", "a", "b"]), (["wifi", "set"], ["a", "b"]))

    def test_non_group_takes_one_word(self):
        self.assertEqual(split_command(["help", "x"]), (["help"], ["x"]))


class BuildLineTest(unittest.TestCase):
    def test_ids_follow_command_words(self):
        self.assertEqual(build_line(["sys", "info"], rid="r1", actor="me"),
                         "sys info -i r1 -a me")

    def test_args_are_quoted_without_double_dash(self):
        self.assertEqual(build_line(["tg", "ack"], ["--older", "30"], rid="r1"),
                         'tg ack -i r1 "--older" "30"')

    def test_text_follows_double_dash(self):
        self.assertEqual(build_line(["tg", "send"], text=["-1 deg"], rid="r1", actor="a"),
                         'tg send -i r1 -a a -- "-1 deg"')

    def test_no_double_dash_without_text(self):
        self.assertNotIn("--", build_line(["wifi", "status"], rid="r1"))

    def test_text_is_escaped(self):
        self.assertEqual(build_line(["wifi", "set"], text=['p"s', "x\ny"]),
                         'wifi set -- "p\\"s" "x\\ny"')


class ParseFrameTest(unittest.TestCase):
    def test_frame_at_line_start(self):
        self.assertEqual(parse_frame('@@{"t":"res","ok":true}'), {"t": "res", "ok": True})

    def test_echoed_command_containing_at_at_is_not_a_frame(self):
        self.assertIsNone(parse_frame('embpet> x @@{"t":"res"}'))

    def test_truncated_json_is_none(self):
        self.assertIsNone(parse_frame('@@{"t":"res'))

    def test_non_object_is_none(self):
        self.assertIsNone(parse_frame("@@{}x"))


class FakeBoardSerial(FakePosixSerial):
    """Scripted board: every written line is recorded; `replies(line)` returns the lines the
    board answers with, which read() hands out one chunk at a time."""
    replies = staticmethod(lambda line: [])

    def open(self):
        super().open()
        self.written = []
        self.pending = []

    def write(self, data):
        line = data.decode().rstrip("\n")
        self.written.append(line)
        self.pending += [(x + "\n").encode() for x in type(self).replies(line)]
        return len(data)

    def flush(self):
        pass

    def read(self, size=1):
        return self.pending.pop(0) if self.pending else b""

    def reset_input_buffer(self):
        self.pending = []


def rid_of(line):
    words = line.split()
    return words[words.index("-i") + 1]


def make_board(replies):
    cls = type("S", (FakeBoardSerial,), {"replies": staticmethod(replies)})
    return Board("/dev/null", serial_cls=cls)


class BoardCmdTest(unittest.TestCase):
    def test_returns_res_matching_rid(self):
        b = make_board(lambda ln: ['@@{"t":"res","rid":"other","ok":false}',
                                   '@@{"t":"res","rid":"%s","ok":true,"v":1}' % rid_of(ln)])
        self.assertEqual(b.cmd("sys info")["v"], 1)

    def test_sends_command_with_rid_and_actor(self):
        b = make_board(lambda ln: ['@@{"t":"res","rid":"%s","ok":true}' % rid_of(ln)])
        b.cmd("tg send", text=["-hi"], actor="me")
        self.assertRegex(b._s.written[0], r'^tg send -i \w+ -a me -- "-hi"$')

    def test_ignores_echo_and_log_lines(self):
        b = make_board(lambda ln: ["embpet> " + ln, "W (1) x: log",
                                   '@@{"t":"res","rid":"%s","ok":true}' % rid_of(ln)])
        self.assertTrue(b.cmd("sys info")["ok"])

    def test_timeout_raises(self):
        with self.assertRaises(CmdTimeout):
            make_board(lambda ln: []).cmd("sys info", timeout=0.3)

    def test_events_seen_during_cmd_are_replayed_by_events(self):
        b = make_board(lambda ln: ['@@{"t":"evt","seq":1,"ev":"in"}',
                                   '@@{"t":"res","rid":"%s","ok":true}' % rid_of(ln)])
        b.cmd("sys info")
        self.assertEqual([e["seq"] for e in b.events(0.2)], [1])

    def test_events_are_yielded_once(self):
        b = make_board(lambda ln: ['@@{"t":"evt","seq":1}', '@@{"t":"res","rid":"%s"}' % rid_of(ln)])
        b.cmd("sys info")
        list(b.events(0.1))
        self.assertEqual(list(b.events(0.1)), [])

    def test_events_reads_new_evt_frames_and_skips_res(self):
        b = make_board(lambda ln: [])
        b._s.pending += [b'@@{"t":"res","rid":"x"}\n', b'noise\n', b'@@{"t":"evt","seq":7}\n']
        self.assertEqual([e["seq"] for e in b.events(0.2)], [7])

    def test_on_event_callback_sees_events_during_cmd(self):
        seen = []
        b = make_board(lambda ln: ['@@{"t":"evt","seq":3}', '@@{"t":"res","rid":"%s"}' % rid_of(ln)])
        b.on_event = seen.append
        b.cmd("sys info")
        self.assertEqual([e["seq"] for e in seen], [3])

    def test_board_does_not_present_reset_state_on_open(self):
        b = make_board(lambda ln: [])
        self.assertNotIn((0, 1), b._s.lines)


class BoardRobustnessTest(unittest.TestCase):
    def test_event_backlog_keeps_only_the_newest(self):
        from embpet.board import EVENT_BACKLOG
        n = EVENT_BACKLOG + 5
        b = make_board(lambda ln: ['@@{"t":"evt","seq":%d}' % i for i in range(n)]
                       + ['@@{"t":"res","rid":"%s"}' % rid_of(ln)])
        b.cmd("sys info")
        seqs = [e["seq"] for e in b.events(0.1)]
        self.assertEqual((len(seqs), seqs[0], seqs[-1]), (EVENT_BACKLOG, 5, n - 1))

    def test_cmd_deadline_holds_under_log_spam(self):
        import time
        b = make_board(lambda ln: [])
        b._s.read = lambda size=1: b"W (1) spam\n" * 50
        t0 = time.monotonic()
        with self.assertRaises(CmdTimeout):
            b.cmd("sys info", timeout=0.3)
        self.assertLess(time.monotonic() - t0, 1.0)

    def test_init_closes_port_when_reset_fails(self):
        made = []

        class Failing(FakePosixSerial):
            def _update_rts_state(self):
                if getattr(self, "is_open", False) and len(self.lines) > 3:
                    raise OSError(5, "ioctl failed")
                super()._update_rts_state()

        def factory():
            made.append(Failing())
            return made[-1]

        with self.assertRaises(OSError):
            Board("/dev/null", serial_cls=factory, reset=True)
        self.assertFalse(made[0].is_open)

    def test_wait_ready_retries_until_the_firmware_answers(self):
        calls = []

        def replies(ln):
            calls.append(ln)
            return ['@@{"t":"res","rid":"%s","ok":true}' % rid_of(ln)] if len(calls) >= 2 else []

        res = make_board(replies).wait_ready(timeout=5)
        self.assertTrue(res["ok"])
        self.assertEqual(len(calls), 2)

    def test_wait_ready_raises_when_silent(self):
        with self.assertRaises(CmdTimeout):
            make_board(lambda ln: []).wait_ready(timeout=0.5)


class CliMainTest(unittest.TestCase):
    def run_main(self, argv):
        import embpet_cli
        calls = []
        orig = embpet_cli.run_command
        embpet_cli.run_command = lambda a, words, args, text: calls.append((words, args, text)) or 0
        try:
            rc = embpet_cli.main(argv)
        finally:
            embpet_cli.run_command = orig
        return rc, calls

    def test_words_after_double_dash_are_text_even_with_leading_dash(self):
        rc, calls = self.run_main(["tg", "send", "--", "-hello", "--x"])
        self.assertEqual((rc, calls), (0, [(["tg", "send"], [], ["-hello", "--x"])]))

    def test_words_before_double_dash_are_args(self):
        _, calls = self.run_main(["-t", "5", "wifi", "set", "arg", "--", "pw"])
        self.assertEqual(calls, [(["wifi", "set"], ["arg"], ["pw"])])

    def test_without_double_dash_there_is_no_text(self):
        _, calls = self.run_main(["wifi", "set", "ssid", "pw"])
        self.assertEqual(calls, [(["wifi", "set"], ["ssid", "pw"], [])])

    def test_second_double_dash_stays_in_text(self):
        _, calls = self.run_main(["tg", "send", "--", "a", "--", "b"])
        self.assertEqual(calls[0][2], ["a", "--", "b"])

    def test_firmware_option_before_double_dash_is_an_arg(self):
        _, calls = self.run_main(["tg", "ack", "--older", "30"])
        self.assertEqual(calls, [(["tg", "ack"], ["--older", "30"], [])])

    def test_firmware_short_option_is_an_arg(self):
        _, calls = self.run_main(["io", "pulse", "3", "-d", "100"])
        self.assertEqual(calls, [(["io", "pulse"], ["3", "-d", "100"], [])])

    def test_cli_flag_before_command_is_consumed(self):
        import embpet_cli
        seen = []
        orig = embpet_cli.run_command
        embpet_cli.run_command = lambda a, w, ar, t: seen.append((a.timeout, w, ar)) or 0
        try:
            embpet_cli.main(["-t", "5", "io", "debounce"])
        finally:
            embpet_cli.run_command = orig
        self.assertEqual(seen, [(5.0, ["io", "debounce"], [])])

    def test_cli_flag_after_command_goes_to_firmware(self):
        import embpet_cli
        seen = []
        orig = embpet_cli.run_command
        embpet_cli.run_command = lambda a, w, ar, t: seen.append((a.timeout, ar)) or 0
        try:
            embpet_cli.main(["io", "pulse", "-t", "9"])
        finally:
            embpet_cli.run_command = orig
        self.assertEqual(seen, [(15.0, ["-t", "9"])])

    def test_option_args_and_text_together(self):
        _, calls = self.run_main(["tg", "ack", "--older", "30", "--", "-x"])
        self.assertEqual(calls, [(["tg", "ack"], ["--older", "30"], ["-x"])])

    def test_reset_after_command_words_is_not_a_cli_flag(self):
        _, calls = self.run_main(["sys", "info", "--reset"])
        self.assertEqual(calls, [(["sys", "info"], ["--reset"], [])])

    def test_listen_rejects_a_command(self):
        with self.assertRaises(SystemExit):
            self.run_main(["--listen", "1", "sys", "info"])


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
