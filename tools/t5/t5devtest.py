#!/usr/bin/env python3
"""Reset the board running the device-test app and collect the Unity results.
Reconnects if USB drops (a panic reboot re-enumerates the port)."""
import argparse
import glob
import re
import subprocess
import sys
import time

import serial

ap = argparse.ArgumentParser()
ap.add_argument("--port", required=True)
ap.add_argument("--timeout", type=float, default=150)
ap.add_argument("--no-reset", action="store_true")
a = ap.parse_args()


def open_port():
    end = time.time() + 30
    while time.time() < end:
        try:
            return serial.Serial(a.port, 115200, timeout=0.2)
        except (serial.SerialException, OSError):
            time.sleep(0.2)
    return None


if not a.no_reset:
    # esptool's hard reset restarts the app reliably; a bare RTS toggle didn't.
    subprocess.Popen(["esptool", "--port", a.port, "--after", "hard-reset", "chip-id"],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
s = open_port()
buf, drops, end = "", 0, time.time() + a.timeout
while time.time() < end and s:
    try:
        buf += s.read(4096).decode(errors="replace")
    except (serial.SerialException, OSError):
        drops += 1
        buf += f"\n[USB dropped at {time.strftime('%H:%M:%S')}]\n"
        try:
            s.close()
        except Exception:
            pass
        time.sleep(1)
        s = open_port()
        continue
    if "#### T5 DEVICE TESTS END" in buf:
        break
start = buf.find("#### T5 DEVICE TESTS BEGIN")
print(buf[start:] if start >= 0 else buf[-4000:])
if drops:
    print(f"[{drops} USB drop(s) during the run]")
m = re.search(r"failures=(\d+)", buf)
sys.exit(0 if m and m.group(1) == "0" else 1)
