#!/usr/bin/env bash
# Package the app SDK: OUT/tab5os-sdk-<version>.tar.gz (tools/release.sh
# calls this; it also runs alone against an existing firmware build).
#
#   tools/sdk_release.sh OUT [FIRMWARE_BUILD]
#
# The tarball is self-contained: the public headers, the headers they reach
# and the firmware's compile flags (sdk/sysroot/, sdk/cmake/sysroot.py), the
# CMake helper, templates, the example apps' sources, the t5 CLI and the
# docs. Building an app from it needs only the ESP-IDF v6.1 RISC-V toolchain
# and CMake; no firmware sources or build. It refuses to ship anything that
# names this machine (its home directory or user name).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[ $# -ge 1 ] || { echo "usage: $0 OUT [FIRMWARE_BUILD]" >&2; exit 1; }
mkdir -p "$1"
OUT="$(cd "$1" && pwd)"
FW="${2:-$ROOT/firmware/build}"
command -v riscv32-esp-elf-gcc >/dev/null || { echo "run '. tools/env.sh' first" >&2; exit 1; }
VER="$(tr -d '[:space:]' < "$ROOT/VERSION")"
NAME="tab5os-sdk-$VER"
SDK="$OUT/$NAME"
rm -rf "$SDK"
mkdir -p "$SDK/tools" "$SDK/apps" "$SDK/docs"

cp -R "$ROOT/sdk" "$SDK/"
rm -rf "$SDK/sdk/sysroot" "$SDK"/sdk/cmake/__pycache__
python3 "$ROOT/sdk/cmake/sysroot.py" stage "$FW" "$SDK/sdk/sysroot"
# The facts the tools would otherwise read from the firmware sources.
python3 "$ROOT/tools/t5/t5const.py" snapshot "$SDK/sdk/device.json"
cp "$ROOT/firmware/idf_version.txt" "$SDK/sdk/"

cp -R "$ROOT/tools/t5" "$ROOT/tools/abi" "$ROOT/tools/ota" "$ROOT/tools/env.sh" "$SDK/tools/"
rm -rf "$SDK"/tools/*/__pycache__ "$SDK"/tools/*/test_*.py
# layout.py checks the OS's struct layouts against a firmware build: not for app developers.
rm -f "$SDK/tools/abi/layout.py"

cp -R "$ROOT/apps/examples" "$SDK/apps/"
rm -rf "$SDK/apps/examples/build"

cp "$ROOT"/docs/sdk.md "$ROOT"/docs/python.md "$ROOT"/docs/api-reference.md "$ROOT"/docs/abi-stability.md \
   "$ROOT"/docs/flashing.md "$ROOT"/docs/security.md "$SDK/docs/"
cp -R "$ROOT/docs/wiki" "$SDK/docs/"
cp "$ROOT/LICENSE" "$SDK/"
cat > "$SDK/README.md" <<README
# Tab5 OS SDK $VER

Build native apps for Tab5 OS (API level 1). You need the ESP-IDF v$(tr -d '[:space:]' < "$ROOT/firmware/idf_version.txt" | sed 's/^v//') RISC-V
toolchain (riscv32-esp-elf, with ESP-IDF or on its own), CMake 3.22+, Ninja
and Python 3.11+. Nothing else: the headers and flags are in sdk/sysroot/.

    . tools/env.sh                 # or put riscv32-esp-elf-gcc on PATH yourself
    tools/t5/t5 new "Hello"        # an app from sdk/templates/c
    tools/t5/t5 app-build hello    # hello/build/com.example.hello.t5app
    tools/t5/t5 run hello          # build, push to a paired Tab5, launch, stream the log

Or with CMake directly: cmake -S apps/examples -B build -G Ninja && cmake --build build

Start with docs/sdk.md and docs/wiki/Home.md.
README

# Nothing may name the machine this was made on.
pat=(-e "/Users/" -e "/home/" -e "$HOME/")
[ "$(id -un)" = root ] || pat+=(-e "$(id -un)")
leaks="$(grep -rIlw "${pat[@]}" "$SDK" || true)"
if [ -n "$leaks" ]; then
    echo "the SDK names this machine in:" >&2
    echo "$leaks" | sed "s|$SDK/|  |" >&2
    exit 1
fi

tar -C "$OUT" -czf "$OUT/$NAME.tar.gz" "$NAME"
rm -rf "$SDK"
echo "$OUT/$NAME.tar.gz"
