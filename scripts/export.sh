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
# shellcheck source=/dev/null
. "$_embpet_root/esp-idf/export.sh" >/dev/null || {
    echo "embpet: ESP-IDF export failed (re-run bootstrap.sh?)" >&2
    unset _embpet_root
    return 1 2>/dev/null || exit 1
}

echo "embpet: ESP-IDF $EMBPET_IDF_TAG from $_embpet_root (IDF_TOOLS_PATH=$IDF_TOOLS_PATH)"
unset _embpet_root
