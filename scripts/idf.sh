#!/bin/bash
# Run one command inside the project's ESP-IDF environment without sourcing
# export.sh into the calling shell:
#
#   scripts/idf.sh idf.py build
#   scripts/idf.sh python tools/embpet_cli.py sys info
#
# Same root resolution as scripts/export.sh ($EMBPET_IDF_ROOT, then
# ~/code/esp-idf_5.5.2). The export banner goes to stderr.
set -euo pipefail

if [ $# -eq 0 ]; then
    echo "usage: scripts/idf.sh <command> [args...]" >&2
    exit 2
fi

cd "$(dirname "${BASH_SOURCE[0]}")/.."
cmd=("$@")
set --   # a sourced script sees the caller's "$@" as its own arguments
# shellcheck source=export.sh
. scripts/export.sh >&2
exec "${cmd[@]}"
