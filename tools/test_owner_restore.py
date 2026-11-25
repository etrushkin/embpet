#!/usr/bin/env python3
"""Offline tests: bridge_test.py and pair_test.py write the owner found at setup back on
exit (embpet-5lg.1). A fake board records every command; the suites are replaced by stubs that
change the owner and then end normally, fail a check, raise or get Ctrl-C. Never touches
hardware. Run: python3 -m pytest tools/test_owner_restore.py (or python3 tools/test_owner_restore.py).
tg_test.py only reads the owner chat from --chat/TG_OWNER_CHAT and never writes it."""
import contextlib
import io
import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bridge_test as B  # noqa: E402
import pair_test as P  # noqa: E402

ENV = {"TG_TOKEN": "123456789:AAFakeFakeFakeFakeFakeFakeFakeFake1", "WIFI_SSID": "net",
       "WIFI_PASS": "pw"}


class FakeBoard:
    """Stateful stand-in for embpet.Board: only the owner is modelled."""
    owner = None
    log = []
    refuse_restore = False

    def __init__(self, port):
        self._s = mock.Mock()
        self.on_event = None

    def wait_ready(self, t):
        pass

    def close(self):
        pass

    def events(self, seconds):
        return iter(())

    def cmd(self, command, *args, text=(), actor=None, timeout=15.0):
        FakeBoard.log.append((command, args, tuple(text)))
        if command == "tg owner":
            if "--clear" in args:
                FakeBoard.owner = None
            elif text:
                if not FakeBoard.refuse_restore:
                    FakeBoard.owner = int(text[0])
            return {"ok": True, "chat_id": FakeBoard.owner}
        if command == "tg t_owner":
            FakeBoard.owner = int(args[0]) or None
        if command == "wifi status":
            return {"state": "up", "ssid": "net"}
        if command == "tg status":
            return {"state": "online", "owner": FakeBoard.owner}
        return {"ok": True}


def writes():
    return [e for e in FakeBoard.log if e[0] in ("tg owner", "tg t_owner") and
            (e[1] or e[2])]


def stub(mode):
    """A suite replacement: it changes the owner, then ends as MODE says."""
    def run(x, *a):
        x.cmd("tg owner", text=["424242"])
        x.cmd("tg t_owner", "0")
        x.cmd("tg owner", text=["-1001777000333"])
        if mode == "fail":
            x.c.check("stub check", False, "boom")
        elif mode == "raise":
            raise RuntimeError("boom")
        elif mode == "ctrlc":
            raise KeyboardInterrupt
    return run


def run_main(module, argv, mode, owner, refuse=False):
    FakeBoard.owner, FakeBoard.log, FakeBoard.refuse_restore = owner, [], refuse
    patches = [mock.patch.object(B, "Board", FakeBoard),
               mock.patch.object(module, "find_port", lambda: "fake"),
               mock.patch.object(B.W, "load_env", lambda p: dict(ENV)),
               mock.patch.object(B.T, "set_token", lambda *a, **k: (0, {"bot": "b"}, "")),
               mock.patch.object(sys, "argv", ["x"] + argv)]
    if module is B:
        patches.append(mock.patch.object(B, "suite_auto", stub(mode)))
    else:
        patches += [mock.patch.object(P, n, stub(mode)) for n in
                    ("step_owner", "step_token", "step_pairing", "step_persist_and_notice",
                     "step_leak")]
    out = io.StringIO()
    with contextlib.ExitStack() as st, contextlib.redirect_stdout(out):
        for p in patches:
            st.enter_context(p)
        rc = module.main()
    return rc, out.getvalue()


class OwnerRestore(unittest.TestCase):
    OWNERS = (None, 123456, -1001234567890)

    def check_run(self, module, argv, mode, owner):
        raised = None
        try:
            rc, out = run_main(module, argv, mode, owner)
        except KeyboardInterrupt as e:
            raised, rc, out = e, None, ""
        except RuntimeError as e:
            raised, rc, out = e, None, ""
        self.assertEqual(FakeBoard.owner, owner)            # stored owner equals the original
        w = writes()
        if owner is None:
            self.assertEqual(w[-1], ("tg owner", ("--clear",), ()))
        else:
            self.assertEqual(w[-1], ("tg owner", (), (str(owner),)))
        return rc, out, raised

    def test_all(self):
        cases = [("ok", 0), ("fail", 1), ("raise", None), ("ctrlc", None)]
        for module, argv in ((B, ["auto"]), (B, ["--chat", "99", "auto"]),
                             (P, [])):
            for owner in self.OWNERS:
                for mode, want_rc in cases:
                    with self.subTest(module=module.__name__, argv=argv, owner=owner,
                                      mode=mode):
                        rc, out, raised = self.check_run(module, argv, mode, owner)
                        self.assertEqual(rc, want_rc)
                        if mode == "raise":
                            self.assertIsInstance(raised, RuntimeError)
                        if mode == "ctrlc":
                            self.assertIsInstance(raised, KeyboardInterrupt)
                        if rc is not None:
                            self.assertIn("INFO owner restored", out)
                            self.assertIn("PASS teardown: owner restored", out)

    def test_restore_mismatch_fails_a_check(self):
        rc, out = run_main(B, ["auto"], "ok", 123456, refuse=True)
        self.assertEqual(rc, 1)
        self.assertIn("FAIL teardown: owner restored", out)

    def test_unreadable_owner_is_not_overwritten(self):
        FakeBoard.owner, FakeBoard.log = 5, []
        with mock.patch.object(FakeBoard, "cmd", lambda *a, **k: {}), \
                contextlib.redirect_stdout(io.StringIO()):
            c = B.Checks([])
            x = B.Ctx("p", "t", "s", "w", 0, c)
            x.b = FakeBoard("p")
            self.assertFalse(x.save_owner())
            x.restore_owner()
        self.assertFalse(c.ok())
        self.assertEqual(FakeBoard.log, [])


if __name__ == "__main__":
    unittest.main()
