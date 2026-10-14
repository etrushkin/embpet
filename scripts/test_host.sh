#!/usr/bin/env bash
# Build and run the host (native) unit tests in build-host/. Works from any cwd.
# Exits non-zero if configuring, building or any test fails.
# Extra arguments go to ctest, e.g. `scripts/test_host.sh -R selftest`.
# If configure fails after a generator change: rm -rf build-host
set -euo pipefail

root="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
args=("$@")
set --   # a sourced script sees the caller's "$@" as its own arguments

# shellcheck source=/dev/null
. "$root/scripts/export.sh" >/dev/null

cmake -S "$root/test/host" -B "$root/build-host" -G Ninja >/dev/null
cmake --build "$root/build-host"
ctest --test-dir "$root/build-host" --output-on-failure "${args[@]+"${args[@]}"}"
