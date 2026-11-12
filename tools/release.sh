#!/usr/bin/env bash
# Build a Tab5 OS release into dist/tab5os-<version>/ (plan 9.7).
#
#   tools/release.sh [--sign KEY.pem]
#
# Makes, from a clean build of the current tree:
#   firmware/   bootloader, partition table, OS image, OTA data, recovery
#               image, C6 radio image, flash_all.sh (one esptool command,
#               nothing else needed)
#   tab5os-<v>.t5os         the OS as an update (signed with KEY if given)
#   tab5os-c6-<v>.t5c6      the C6 radio firmware for that OS (signed the same);
#                           the OS fetches it from the release (t5_ota_net/c6.h)
#   apps/*.t5app            the example apps
#   tab5os-sdk-<v>.tar.gz   the app SDK, self-contained (tools/sdk_release.sh)
#   SHA256SUMS
# It doesn't tag, push or publish anything: check the result, then
#   git tag -a v<version> -m "Tab5 OS <version>" && git push --tags
# and attach dist/tab5os-<version>/ to the release.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KEY=""
while [ $# -gt 0 ]; do
    case "$1" in
        --sign) KEY="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"; shift 2 ;;
        *) echo "usage: $0 [--sign KEY.pem]" >&2; exit 1 ;;
    esac
done
command -v idf.py >/dev/null || { echo "run '. tools/env.sh' first" >&2; exit 1; }
T5="$ROOT/tools/t5/t5"
VER="$(tr -d '[:space:]' < "$ROOT/VERSION")"
OUT="$ROOT/dist/tab5os-$VER"
FW="$ROOT/firmware/build"
rm -rf "$OUT"
mkdir -p "$OUT/firmware" "$OUT/apps"

echo "== firmware"
git -C "$ROOT" submodule update --init third_party/micropython >/dev/null
"$T5" build >/dev/null
"$T5" size-check
cp "$FW/bootloader/bootloader.bin" "$FW/partition_table/partition-table.bin" "$FW/ota_data_initial.bin" \
   "$FW/tab5os.bin" "$OUT/firmware/"

echo "== C6 radio firmware"
"$T5" c6-build >/dev/null
cp "$ROOT/c6/build/tab5os_c6.bin" "$OUT/firmware/"

echo "== recovery"
"$T5" recovery-build | tail -1
cp "$ROOT/recovery/build/tab5recovery.bin" "$OUT/firmware/"

# Offsets: the build's flasher_args.json, and the recovery partition from partitions.csv.
FLASH_ARGS="$(python3 "$ROOT/tools/t5/t5const.py" flash-args "$FW")"
FLASH_ARGS="$FLASH_ARGS $(python3 "$ROOT/tools/t5/t5const.py" partition recovery offset) tab5recovery.bin"
FLASH_ARGS="$FLASH_ARGS $(python3 "$ROOT/tools/t5/t5const.py" partition c6fw offset) tab5os_c6.bin"
cat > "$OUT/firmware/flash_all.sh" <<FLASH
#!/bin/sh
# Flash a Tab5 from scratch: bootloader, partition table, OS (slot ota_0),
# the recovery image and the C6 radio firmware (the OS moves the C6 onto it
# after its first minute). Settings and files in /data are kept. Needs esptool
# (pip install esptool). Set PORT to pick the serial port; esptool finds it otherwise.
set -e
cd "\$(dirname "\$0")"
esptool --chip esp32p4 \${PORT:+--port "\$PORT"} -b 460800 write-flash $FLASH_ARGS
FLASH
chmod +x "$OUT/firmware/flash_all.sh"

echo "== update package"
PY="${IDF_PYTHON_ENV_PATH:+$IDF_PYTHON_ENV_PATH/bin/python}"
PY="${PY:-python3}"
"$PY" "$ROOT/tools/ota/t5ota.py" pack "$FW/tab5os.bin" -o "$OUT/tab5os-$VER.t5os" ${KEY:+--sign "$KEY"}

"$PY" "$ROOT/tools/ota/t5ota.py" pack-c6 "$ROOT/c6/build/tab5os_c6.bin" -o "$OUT/tab5os-c6-$VER.t5c6" ${KEY:+--sign "$KEY"}

echo "== example apps"
"$T5" examples >/dev/null
for f in "$ROOT"/apps/examples/build/*.t5app; do
    if [ -n "$KEY" ]; then
        name=$(basename "$f" .t5app)
        "$PY" "$ROOT/tools/t5/t5pack.py" "$ROOT/apps/examples/build/$name.pkg" -o "$OUT/apps/$name.t5app" --sign "$KEY" >/dev/null
    else
        cp "$f" "$OUT/apps/"
    fi
done

echo "== SDK and CLI"
"$ROOT/tools/sdk_release.sh" "$OUT" "$FW" >/dev/null

cp "$ROOT/LICENSE" "$ROOT/docs/flashing.md" "$OUT/"
(cd "$OUT" && find . -type f ! -name SHA256SUMS | sort | xargs shasum -a 256 > SHA256SUMS)
echo
echo "$OUT:"
(cd "$OUT" && find . -type f | sort | sed 's|^\./|  |')
[ -n "$KEY" ] || echo "note: unsigned. Devices install it only in developer mode; pass --sign for a release."
