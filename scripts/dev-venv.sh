#!/bin/bash
# Create the project-local dev Python env .venv with pyserial and pytest. It is built
# from the ESP-IDF interpreter, but the ESP-IDF venv itself is never touched. .venv is
# git-ignored. Does nothing (and needs no network) when .venv already has both packages.
#
#   scripts/dev-venv.sh                            # create / repair .venv
#   scripts/hw.sh python tools/hil.py -k smoke     # then run the board tests with it
set -euo pipefail

root="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

check='import serial, pytest; assert serial.__version__ == "3.5" and int(pytest.__version__.split(".")[0]) >= 8'

if [ ! -x .venv/bin/python ]; then
    scripts/idf.sh python -m venv .venv
fi
if ! scripts/idf.sh .venv/bin/python -c "$check" 2>/dev/null; then
    scripts/idf.sh .venv/bin/python -m pip install --quiet "pyserial==3.5" "pytest>=8"
fi
scripts/idf.sh .venv/bin/python -c "import serial, pytest; print('dev venv ok: pyserial', serial.__version__, 'pytest', pytest.__version__)"
