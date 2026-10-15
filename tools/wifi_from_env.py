#!/usr/bin/env python3
"""Provision the board's Wi-Fi from the git-ignored .env.local without ever printing the
credentials (embpet-y90). Agents use this instead of sourcing .env.local.

  scripts/hw.sh python tools/wifi_from_env.py [-p PORT] [--env FILE] [--wait SEC]

Reads WIFI_SSID and WIFI_PASS from .env.local itself (KEY=VALUE lines; `export ` prefix,
quotes and # comments are handled; WIFI_PASS must be present, empty = open network), sends
`wifi set -- <ssid> <pass>` in-process with the tools/embpet package, then polls
`wifi status` for up to --wait seconds (default 30). Prints one JSON line:
  {"set": {...res...}, "status": {...res...}}
The SSID and password are replaced by *** anywhere they would appear in that output.
Exit 0 = credentials stored and the board is up with an IP, 1 = stored but not up (or a
command failed), 2 = .env.local unreadable/incomplete or no response from the board.

The values are never put in argv (not visible in `ps`).
"""
import argparse
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from embpet import Board, CmdTimeout, NoBoard  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLI = os.path.join(ROOT, "tools", "embpet_cli.py")


def load_env(path):
    """Parse KEY=VALUE lines. No shell semantics: no expansion, no command substitution."""
    env = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("export "):
                line = line[7:].lstrip()
            key, sep, val = line.partition("=")
            if not sep:
                continue
            val = val.strip()
            if len(val) >= 2 and val[0] == val[-1] and val[0] in "\"'":
                val = val[1:-1]
            elif " #" in val:
                val = val.split(" #", 1)[0].rstrip()
            env[key.strip()] = val
    return env


def mask(obj, secrets):
    """Replace every secret by *** in a string, or recursively in a JSON-like object
    (longest secret first, so a secret that contains another is fully masked)."""
    if isinstance(obj, str):
        for s in sorted((x for x in secrets if x), key=len, reverse=True):
            obj = obj.replace(s, "***")
        return obj
    if isinstance(obj, dict):
        return {mask(k, secrets): mask(v, secrets) for k, v in obj.items()}
    if isinstance(obj, list):
        return [mask(v, secrets) for v in obj]
    return obj


def run_cli(args, port=None, timeout=20.0, extra=()):
    """Run embpet_cli.py; returns (exit code, parsed res frame or None, stderr text)."""
    cmd = [sys.executable, CLI, "-t", str(timeout)]
    if port:
        cmd += ["-p", port]
    cmd += list(extra) + list(args)
    r = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out = r.stdout.decode("utf-8", "replace").strip()
    res = None
    for line in out.splitlines():
        try:
            res = json.loads(line)
        except ValueError:
            pass
    return r.returncode, res, r.stderr.decode("utf-8", "replace")


def wifi_set(ssid, password, port=None):
    """`wifi set -- ssid pass` sent in-process, so the values never reach argv / ps; `--`
    keeps a value that starts with '-' from being an option. Same return shape as run_cli:
    (exit code, res frame or None, error text)."""
    words = [ssid] + ([password] if password else [])
    try:
        with Board(port) as b:
            res = b.cmd("wifi set", text=words, timeout=20.0)
    except CmdTimeout:
        return 2, None, "timeout: no response to wifi set"
    except (NoBoard, OSError) as e:
        return 2, None, "serial error: %s" % type(e).__name__
    return (0 if res.get("ok") else 1), res, ""


def wait_up(port=None, seconds=30.0):
    """Poll `wifi status` until state is up; returns the last status res (or None)."""
    deadline = time.monotonic() + seconds
    res = None
    while True:
        _, res, _ = run_cli(["wifi", "status"], port, timeout=10.0)
        if (res and res.get("ok") and res.get("state") == "up") or time.monotonic() >= deadline:
            return res
        time.sleep(2.0)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-p", "--port")
    ap.add_argument("--env", default=os.path.join(ROOT, ".env.local"))
    ap.add_argument("--wait", type=float, default=30.0, metavar="SEC")
    a = ap.parse_args()

    try:
        env = load_env(a.env)
    except OSError as e:
        print("cannot read %s: %s" % (os.path.basename(a.env), e.strerror), file=sys.stderr)
        return 2
    ssid = env.get("WIFI_SSID", "")
    if not ssid:
        print("WIFI_SSID is empty or missing in %s" % os.path.basename(a.env), file=sys.stderr)
        return 2
    if "WIFI_PASS" not in env:      # an open network needs an explicit WIFI_PASS=
        print("WIFI_PASS is missing in %s (set it to an empty value for an open network)" %
              os.path.basename(a.env), file=sys.stderr)
        return 2
    password = env["WIFI_PASS"]
    secrets = [ssid, password]

    code, set_res, err = wifi_set(ssid, password, a.port)
    if set_res is None:
        print("no response to wifi set (exit %d): %s" % (code, mask(err.strip(), secrets)),
              file=sys.stderr)
        return 2
    status = wait_up(a.port, a.wait) if set_res.get("ok") else None
    print(json.dumps(mask({"set": set_res, "status": status}, secrets), ensure_ascii=False))
    up = bool(status and status.get("ok") and status.get("state") == "up")
    return 0 if up else 1


if __name__ == "__main__":
    sys.exit(main())
