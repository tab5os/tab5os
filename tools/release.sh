#!/usr/bin/env bash
# Build a Tab5 OS release into dist/tab5os-<version>/ (plan 9.7).
#
#   tools/release.sh [--sign KEY.pem] [--rev rev1]
#
# Makes, from a clean build of the current tree:
#   firmware/   bootloader, partition table, OS image, OTA data, recovery
#               image, flash_all.sh (one esptool command, nothing else needed)
#   tab5os-<v>.t5os         the OS as an update (signed with KEY if given)
#   tab5os-c6-<v>.bin       the ESP32-C6 radio image, if c6/ has been built
#   apps/*.t5app            the example apps
#   tab5os-sdk-<v>.tar.gz   SDK headers, CMake, templates, the t5 CLI, docs
#   SHA256SUMS
# It doesn't tag, push or publish anything: check the result, then
#   git tag -a v<version> -m "Tab5 OS <version>" && git push --tags
# and attach dist/tab5os-<version>/ to the release.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REV=rev1
KEY=""
while [ $# -gt 0 ]; do
    case "$1" in
        --sign) KEY="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"; shift 2 ;;
        --rev) REV="$2"; shift 2 ;;
        *) echo "usage: $0 [--sign KEY.pem] [--rev rev1]" >&2; exit 1 ;;
    esac
done
command -v idf.py >/dev/null || { echo "run '. tools/env.sh' first" >&2; exit 1; }
VER="$(tr -d '[:space:]' < "$ROOT/VERSION")"
OUT="$ROOT/dist/tab5os-$VER"
FW="$ROOT/firmware/build-$REV"
rm -rf "$OUT"
mkdir -p "$OUT/firmware" "$OUT/apps"

echo "== firmware ($REV)"
git -C "$ROOT" submodule update --init third_party/micropython >/dev/null
(cd "$ROOT/firmware" && idf.py -B "build-$REV" -D T5_REV="$REV" build >/dev/null)
size=$(stat -f %z "$FW/tab5os.bin" 2>/dev/null || stat -c %s "$FW/tab5os.bin")
[ "$size" -le $((4 * 1024 * 1024 * 9 / 10)) ] || { echo "tab5os.bin is over 90% of its slot" >&2; exit 1; }
cp "$FW/bootloader/bootloader.bin" "$FW/partition_table/partition-table.bin" "$FW/ota_data_initial.bin" \
   "$FW/tab5os.bin" "$OUT/firmware/"

echo "== recovery"
(cd "$ROOT/recovery" && { [ -f sdkconfig ] || idf.py -B build set-target esp32p4 >/dev/null; } && idf.py -B build build >/dev/null)
cp "$ROOT/recovery/build/tab5recovery.bin" "$OUT/firmware/"

cat > "$OUT/firmware/flash_all.sh" <<'FLASH'
#!/bin/sh
# Flash a Tab5 from scratch: bootloader, partition table, OS (slot ota_0) and
# the recovery image. Settings and files in /data are kept. Needs esptool
# (pip install esptool). PORT defaults to the first USB serial port.
set -e
cd "$(dirname "$0")"
PORT="${PORT:-$(ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null | head -1)}"
esptool --chip esp32p4 --port "$PORT" -b 460800 write-flash --flash-mode dio --flash-size 16MB \
    0x2000 bootloader.bin 0x10000 partition-table.bin 0x40000 ota_data_initial.bin \
    0x1d0000 tab5os.bin 0x50000 tab5recovery.bin
FLASH
chmod +x "$OUT/firmware/flash_all.sh"

echo "== update package"
PY="${IDF_PYTHON_ENV_PATH:+$IDF_PYTHON_ENV_PATH/bin/python}"
PY="${PY:-python3}"
"$PY" "$ROOT/tools/ota/t5ota.py" pack "$FW/tab5os.bin" -o "$OUT/tab5os-$VER.t5os" ${KEY:+--sign "$KEY"}

if [ -f "$ROOT/c6/build/tab5os_c6.bin" ]; then
    cp "$ROOT/c6/build/tab5os_c6.bin" "$OUT/tab5os-c6-$VER.bin"
fi

echo "== example apps"
cmake -S "$ROOT/apps/examples" -B "$ROOT/apps/examples/build" -G Ninja -DT5_FIRMWARE_BUILD="$FW" >/dev/null
cmake --build "$ROOT/apps/examples/build" >/dev/null
for f in "$ROOT"/apps/examples/build/*.t5app; do
    if [ -n "$KEY" ]; then
        name=$(basename "$f" .t5app)
        "$PY" "$ROOT/tools/t5/t5pack.py" "$ROOT/apps/examples/build/$name.pkg" -o "$OUT/apps/$name.t5app" --sign "$KEY" >/dev/null
    else
        cp "$f" "$OUT/apps/"
    fi
done

echo "== SDK and CLI"
SDK="$OUT/tab5os-sdk-$VER"
mkdir -p "$SDK/tools"
cp -R "$ROOT/sdk" "$SDK/"
cp -R "$ROOT/tools/t5" "$ROOT/tools/abi" "$ROOT/tools/ota" "$ROOT/tools/env.sh" "$SDK/tools/"
rm -rf "$SDK"/tools/*/__pycache__
mkdir -p "$SDK/docs"
cp "$ROOT"/docs/sdk.md "$ROOT"/docs/python.md "$ROOT"/docs/api-reference.md "$ROOT"/docs/flashing.md \
   "$ROOT"/docs/security.md "$SDK/docs/"
cp "$ROOT/LICENSE" "$SDK/"
# Native apps compile against a firmware build's headers and config: ship
# the compile database and generated config with the SDK.
mkdir -p "$SDK/firmware-build"
cp "$FW/compile_commands.json" "$SDK/firmware-build/"
cp -R "$FW/config" "$SDK/firmware-build/"
tar -C "$OUT" -czf "$OUT/tab5os-sdk-$VER.tar.gz" "tab5os-sdk-$VER"
rm -rf "$SDK"

cp "$ROOT/LICENSE" "$ROOT/docs/flashing.md" "$OUT/"
(cd "$OUT" && find . -type f ! -name SHA256SUMS | sort | xargs shasum -a 256 > SHA256SUMS)
echo
echo "$OUT:"
(cd "$OUT" && find . -type f | sort | sed 's|^\./|  |')
[ -n "$KEY" ] || echo "note: unsigned. Devices install it only in developer mode; pass --sign for a release."
