#!/usr/bin/env python3
"""Heap across repeated `tg token` re-sets (embpet-jk5.2). Needs a CONFIG_EMBPET_TG_TEST build
(e.g. components/tgbot/bridgetest.defaults: `tg t_stat`), Wi-Fi up and TG_TOKEN in
.env.local. Nothing is sent to Telegram. Run under the board lock:

  scripts/hw.sh python tools/token_heap.py [-p PORT] [--rounds 50] [--control 10]
      [--settle 15] [--log FILE.jsonl]

First --control idle samples (no token change, --settle + about 5 s apart: the same spacing
as the rounds) measure the heap noise of plain long polling. Then --rounds times: the same
valid token from .env.local is set with `tg token` (checked with getMe, stored, poller
restarted), the bridge must be online again within 30 s, the console is read for --settle
seconds, and `tg t_stat` gives heap_free, heap_min and heap_largest. One JSON line per sample
goes to stdout (and to --log); the last line is {"summary": ...} with, per phase, the
first/last/min/max of each figure, the least-squares slope (bytes per sample), the medians of
the quarters, and the longest run of consecutive heap_free declines.

A `tg token` that fails with `offline`, `timeout` or `busy` (getMe hit a transient network
error, no answer reached the host, or the previous attempt is still running) is retried once after the bridge is online again; the summary
counts these as token_retries. A round that still fails is logged with "ok": false and left
out of the summary statistics. The token is never printed (the res frame is masked as
tools/tg_from_env.py does).
Exit 0 = every check passed, 1 = any check failed (a round, a reset during the run, the
masked token in tg status), 2 = setup error (.env.local, board, no `tg t_stat`, not online).
"""
import argparse
import json
import os
import statistics
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from embpet import CmdTimeout, NoBoard, find_port  # noqa: E402
import bridge_test as BT  # noqa: E402
import robust_test as R  # noqa: E402
import tg_from_env as T  # noqa: E402
import wifi_from_env as W  # noqa: E402
from wifi_test import Checks  # noqa: E402

FIGS = ("heap_free", "heap_min", "heap_largest")


def slope(ys):
    """Least-squares slope of ys against 0..n-1 (bytes per sample)."""
    n = len(ys)
    if n < 2:
        return 0.0
    mx = (n - 1) / 2.0
    my = sum(ys) / float(n)
    num = sum((i - mx) * (y - my) for i, y in enumerate(ys))
    den = sum((i - mx) ** 2 for i in range(n))
    return num / den


def stats(samples):
    out = {"samples": len(samples)}
    if not samples:
        return out
    for k in FIGS:
        ys = [s[k] for s in samples if s.get(k) is not None]
        if not ys:
            continue
        q = max(1, len(ys) // 4)
        quarters = [ys[i * q:(i + 1) * q] for i in range(3)] + [ys[3 * q:]]
        out[k] = {"first": ys[0], "last": ys[-1], "min": min(ys), "max": max(ys),
                  "slope_per_sample": round(slope(ys), 1),
                  "quarter_medians": [statistics.median(p or ys[-q:]) for p in quarters]}
    run = best = 0
    hf = [s["heap_free"] for s in samples if s.get("heap_free") is not None]
    for p, q2 in zip(hf, hf[1:]):
        run = run + 1 if q2 < p else 0
        best = max(best, run)
    out["longest_heap_free_decline_run"] = best
    return out


def sample(x, phase, i, log, ok=True):
    st = x.cmd("tg t_stat")
    s = {"phase": phase, "i": i, "ok": ok, "t": round(time.monotonic(), 1),
         "tg": st.get("tg"), **{k: st.get(k) for k in FIGS}}
    line = json.dumps(s)
    print(line, flush=True)
    if log:
        log.write(line + "\n")
    return s


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-p", "--port")
    ap.add_argument("--rounds", type=int, default=50)
    ap.add_argument("--control", type=int, default=10, metavar="N")
    ap.add_argument("--settle", type=float, default=15.0, metavar="S")
    ap.add_argument("--log")
    a = ap.parse_args()
    try:
        env = W.load_env(os.path.join(W.ROOT, ".env.local"))
    except OSError:
        print("cannot read .env.local", file=sys.stderr)
        return 2
    token, ssid = env.get("TG_TOKEN", ""), env.get("WIFI_SSID", "")
    if not token or not ssid or "WIFI_PASS" not in env:
        print("TG_TOKEN / WIFI_SSID / WIFI_PASS missing in .env.local", file=sys.stderr)
        return 2
    checks = Checks(T.secrets_of(token) + [env["WIFI_PASS"], ssid])
    x = BT.Ctx(a.port or find_port(), token, ssid, env["WIFI_PASS"], 0, checks)
    try:
        x.open()
    except (CmdTimeout, NoBoard, OSError) as e:
        print("board not ready: %s" % type(e).__name__, file=sys.stderr)
        x.close()
        return 2
    log = open(a.log, "w", encoding="utf-8") if a.log else None
    ctrl, rounds = [], []
    retries = 0
    try:
        st = x.cmd("tg t_stat")
        if not st:
            print("tg t_stat did not answer (timeout)", file=sys.stderr)
            return 2
        if "heap_largest" not in st:
            print("no `tg t_stat`: flash a CONFIG_EMBPET_TG_TEST build", file=sys.stderr)
            return 2
        if not checks.check("setup: tg online", R.wait_tg(x, "online", 60)):
            return 2
        boot0 = R.sys_info(x).get("boot")
        for i in range(a.control):
            x.pump(a.settle + 5.0)
            ctrl.append(sample(x, "control", i, log))
        for i in range(a.rounds):
            code, res, err = T.set_token(token, board=x.b)
            why = res.get("err") if res else err.split(":", 1)[0]
            if code != 0 and why in ("offline", "timeout", "busy"):
                # getMe hit a transient network error (board or host timeout; busy: the
                # previous attempt is still running): not what this measures, retry once
                retries += 1
                print("INFO round %d: tg token %s, retrying once" % (i + 1, why), flush=True)
                R.wait_tg(x, "online", 30)
                code, res, err = T.set_token(token, board=x.b)
            ok = checks.check("round %d: same token accepted" % (i + 1), code == 0,
                              T.W.mask(res, T.secrets_of(token)) if res else err)
            ok = checks.check("round %d: online again" % (i + 1),
                              R.wait_tg(x, "online", 30)) and ok
            x.pump(a.settle)
            s = sample(x, "token", i, log, ok)
            if ok:
                rounds.append(s)            # failed rounds are logged, kept out of the stats
        checks.check("same boot throughout (no reset)",
                     boot0 is not None and R.sys_info(x).get("boot") == boot0, boot0)
        checks.check("token still masked in tg status",
                     x.cmd("tg status").get("token") == T.masked(token))
    finally:
        summ = {"control": stats(ctrl), "token": stats(rounds), "settle_s": a.settle,
                "token_retries": retries}
        line = json.dumps({"summary": summ})
        print(line, flush=True)
        if log:
            log.write(line + "\n")
            log.close()
        x.close()
    return 0 if checks.ok() else 1


if __name__ == "__main__":
    sys.exit(main())
