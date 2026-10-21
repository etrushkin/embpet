# Bring a bootstrapped ESP-IDF (see scripts/bootstrap.sh) into the current shell.
# Must be sourced (bash or zsh):
#
#   . scripts/export.sh [<root>]
#
# <root> defaults to $EMBPET_IDF_ROOT, then ~/code/esp-idf_5.5.2.
# No `set -e` here: this runs inside the caller's interactive shell.

_embpet_root="${1:-${EMBPET_IDF_ROOT:-$HOME/code/esp-idf_5.5.2}}"

if [ ! -f "$_embpet_root/env.sh" ] || [ ! -f "$_embpet_root/esp-idf/export.sh" ]; then
    echo "embpet: no bootstrapped ESP-IDF in '$_embpet_root'" >&2
    echo "        run: scripts/bootstrap.sh $_embpet_root" >&2
    unset _embpet_root
    return 1 2>/dev/null || exit 1
fi

# shellcheck source=/dev/null
. "$_embpet_root/env.sh"
# Use the venv bootstrap.sh created (the lexically last one, if there are several). IDF
# otherwise looks for a venv named after the first python3 on PATH, so installing a newer
# Python (e.g. Homebrew) breaks every export. `|| ...`: callers run under set -e/pipefail.
if [ -z "${IDF_PYTHON_ENV_PATH:-}" ]; then
    _embpet_env=$(find "$IDF_TOOLS_PATH/python_env" -maxdepth 1 -name 'idf*_py*_env' 2>/dev/null \
        | sort | tail -n 1) || _embpet_env=
    if [ -n "$_embpet_env" ] && [ -x "$_embpet_env/bin/python" ]; then
        export IDF_PYTHON_ENV_PATH="$_embpet_env"
    fi
    unset _embpet_env
fi
# shellcheck source=/dev/null
. "$_embpet_root/esp-idf/export.sh" >/dev/null || {
    echo "embpet: ESP-IDF export failed (re-run bootstrap.sh?)" >&2
    unset _embpet_root
    return 1 2>/dev/null || exit 1
}

echo "embpet: ESP-IDF $EMBPET_IDF_TAG from $_embpet_root (IDF_TOOLS_PATH=$IDF_TOOLS_PATH)"
unset _embpet_root
