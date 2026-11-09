#!/usr/bin/env bash
# Prove the SDK release is self-contained (plan 9.7): extract it outside the
# repo and, with nothing on PATH but the RISC-V compiler, CMake, Ninja and
# Python (no ESP-IDF environment, no firmware build), build the C template,
# pack the Python template and build every example app.
#
#   test/sdk/check_tarball.sh dist/.../tab5os-sdk-<v>.tar.gz
#
# Set T5_HIDE_FIRMWARE_BUILD=1 to also move firmware/build aside while it runs.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TAR="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
GCC="$(command -v riscv32-esp-elf-gcc || true)"
[ -n "$GCC" ] || { echo "riscv32-esp-elf-gcc isn't on PATH (. tools/env.sh, then run this)" >&2; exit 1; }
TOOLS=""
for t in cmake ninja; do TOOLS="$TOOLS:$(dirname "$(command -v $t)")"; done
PY="$(command -v python3)"
WORK="$(mktemp -d /tmp/t5sdk.XXXXXX)"
case "$WORK" in "$ROOT"/*) echo "temp dir is inside the repo" >&2; exit 1 ;; esac

hidden=""
restore() { [ -z "$hidden" ] || mv "$hidden" "$ROOT/firmware/build"; }
trap restore EXIT
if [ "${T5_HIDE_FIRMWARE_BUILD:-0}" = 1 ] && [ -d "$ROOT/firmware/build" ]; then
    hidden="$ROOT/firmware/build.hidden.$$"
    mv "$ROOT/firmware/build" "$hidden"
fi

tar -C "$WORK" -xzf "$TAR"
SDK="$(echo "$WORK"/tab5os-sdk-*)"
# A bare environment: only what the README lists.
run() {
    env -i HOME="$WORK/home" PATH="$(dirname "$GCC")$TOOLS:$(dirname "$PY"):/usr/bin:/bin" "$@"
}
mkdir -p "$WORK/home"
cd "$WORK"

echo "== C template"
run "$SDK/tools/t5/t5" new "Hello SDK" --dir "$WORK/hello" >/dev/null
run "$SDK/tools/t5/t5" app-build "$WORK/hello" | tail -1
echo "== Python template"
run "$SDK/tools/t5/t5" new "Hello Py" --python --dir "$WORK/hellopy" >/dev/null
run "$SDK/tools/t5/t5" app-build "$WORK/hellopy" | tail -1
echo "== examples"
run cmake -S "$SDK/apps/examples" -B "$WORK/examples" -G Ninja >/dev/null
run cmake --build "$WORK/examples" | grep -E "warning|error|Packing" || true
n=$(ls "$WORK"/examples/*.t5app | wc -l | tr -d ' ')
want=$(grep -c "^t5_app(" "$SDK/apps/examples/CMakeLists.txt")
[ "$n" = "$want" ] || { echo "built $n of $want example apps" >&2; exit 1; }
for f in "$WORK"/hello/build/*.t5app "$WORK"/examples/*.t5app; do
    run "$PY" "$SDK/tools/t5/t5pack.py" --inspect "$f" | head -1
done
# The builds didn't reach outside the SDK for a header.
if grep -rh "\.h" "$WORK"/examples/CMakeFiles/*/flags.make 2>/dev/null | grep -q "$ROOT"; then
    echo "a build flag points into the repo" >&2; exit 1
fi
if grep -rl "$ROOT" "$WORK/examples" "$WORK/hello/build" 2>/dev/null | grep -v "\.t5app$" | head -1 | grep -q .; then
    echo "the build names the repo: $(grep -rl "$ROOT" "$WORK/examples" "$WORK/hello/build" | head -3)" >&2; exit 1
fi
echo "OK: the SDK in $WORK built the C template, the Python template and $n example apps"
rm -rf "$WORK"
