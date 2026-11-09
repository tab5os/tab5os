#!/usr/bin/env python3
"""Bench 1.10: hammer the system I2C bus from five tasks for a long time.

Runs `i2c stress <seconds>` on the device (five tasks on both cores reading
known constant registers from the expanders, IMU, INA226 and RTC), and
optionally forces bus recoveries while it runs. Pass = no bad data, no lock
timeouts, and every device that answered before still answering afterwards.

The physical part of 1.10 (shorting SDA to GND for ~100 ms with a probe while
this runs) needs a person: pass --manual-short and follow the prompt.

    tools/t5/t5 hil i2c_stress --seconds 3600
"""
import argparse
import re
import sys
import time

from t5link import connect


def scan(t):
    """Addresses that answer `i2c scan`."""
    return {int(x, 16) for x in re.findall(r"^\s+0x([0-9a-f]{2})\s", t.cmd("i2c scan", 30), re.M)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=3600)
    ap.add_argument("--manual-short", action="store_true")
    a = ap.parse_args()

    with connect(cmd_timeout=a.seconds + 120) as t:
        devices_before = scan(t)
        before = t.cmd("i2c stats", 10)
        if a.manual_short:
            print("Short SDA (G31) to GND for ~100 ms a few times during the run.")
        t0 = time.time()
        out = t.cmd(f"i2c stress {a.seconds}")
        print(out)
        after = t.cmd("i2c stats", 10)
        devices_after = scan(t)
    print(f"\nran {time.time() - t0:.0f} s\n--- stats before ---\n{before}\n--- stats after ---\n{after}")
    m = re.search(r"STRESS (PASS|FAIL).*lock timeouts (\d+)", out)
    gone = sorted(devices_before - devices_after)
    ok = m and m.group(1) == "PASS" and m.group(2) == "0" and not gone
    print(f"I2C STRESS {'PASS' if ok else 'FAIL'}: {len(devices_after)} devices answer after the run"
          + (f", gone: {' '.join(hex(x) for x in gone)}" if gone else ""))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
