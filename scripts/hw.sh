#!/bin/bash
# Run one hardware command (flash, CLI, HIL) inside ESP-IDF while holding the
# board lock, so only one agent uses the serial port at a time:
#
#   scripts/hw.sh idf.py -p /dev/cu.usbmodem11101 flash
#   scripts/hw.sh python tools/embpet_cli.py sys info
#
# Waits while another holder has the lock. Needs flock(1) (brew install flock).
set -euo pipefail

if [ $# -eq 0 ]; then
    echo "usage: scripts/hw.sh <command> [args...]" >&2
    exit 2
fi

root="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
exec flock /tmp/embpet-hw.lock "$root/scripts/idf.sh" "$@"
