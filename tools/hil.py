#!/usr/bin/env python3
"""Run the hardware-in-the-loop pytest suite (test/hil) with the project .venv.

  scripts/hw.sh python tools/hil.py [pytest args]     e.g. -k smoke

Running it through scripts/hw.sh keeps the board lock for the whole test run. Create the
venv first with scripts/dev-venv.sh. Set EMBPET_PORT to pick a port (a port that cannot be
opened fails). Without a board the tests are skipped, unless --require-board is given,
which turns that into a failure.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VENV_PYTHON = os.path.join(ROOT, ".venv", "bin", "python")


def main():
    if not os.path.exists(VENV_PYTHON):
        sys.exit("no .venv: run scripts/dev-venv.sh first")
    args = sys.argv[1:]
    env = dict(os.environ)
    if "--require-board" in args:
        args.remove("--require-board")
        env["EMBPET_REQUIRE_BOARD"] = "1"
    # -rs shows why tests were skipped, so a missing board is not mistaken for a pass.
    cmd = [VENV_PYTHON, "-m", "pytest", "-rs", os.path.join(ROOT, "test", "hil")] + args
    return subprocess.call(cmd, cwd=ROOT, env=env)


if __name__ == "__main__":
    sys.exit(main())
