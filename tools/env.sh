# Source this file: `. tools/env.sh`
# Sets up ESP-IDF at the version in firmware/idf_version.txt, separate from
# any other IDF install (e.g. ~/esp/esp-idf-v5.5.5).

_t5_root="$(cd "$(dirname "${BASH_SOURCE[0]:-${(%):-%x}}")/.." && pwd)"
_t5_idf_ver="$(tr -d '[:space:]' < "$_t5_root/firmware/idf_version.txt")"
export T5_ROOT="$_t5_root"
export IDF_PATH="${T5_IDF_PATH:-$HOME/esp/esp-idf-$_t5_idf_ver}"

if [ ! -f "$IDF_PATH/export.sh" ]; then
    echo "ESP-IDF $_t5_idf_ver not found at $IDF_PATH." >&2
    echo "Install it, set T5_IDF_PATH, or build in Docker: tools/t5/t5 build --docker" >&2
    return 1 2>/dev/null || exit 1
fi

. "$IDF_PATH/export.sh" > /dev/null || return 1
export PATH="$T5_ROOT/tools/t5:$PATH"
echo "tab5os: ESP-IDF $(cd "$IDF_PATH" && git describe --tags 2>/dev/null) at $IDF_PATH, variant ${T5_REV:-rev1}"
unset _t5_root _t5_idf_ver
