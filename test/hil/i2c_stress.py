#!/usr/bin/env python3
"""Bench 1.10: hammer the system I2C bus from five tasks for a long time.

Runs `i2c stress <seconds>` on the device (five tasks on both cores reading
known constant registers from the expanders, IMU, INA226 and RTC), and
optionally forces bus recoveries while it runs. Pass = no bad data, no lock
timeouts, and every device still answering afterwards.

The physical part of 1.10 (shorting SDA to GND for ~100 ms with a probe while
this runs) needs a person: pass --manual-short and follow the prompt.

    ~/.espressif/python_env/idf6.1_py3.15_env/bin/python test/hil/i2c_stress.py --seconds 3600
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools", "t5"))
from t5link import Tab5  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=3600)
    ap.add_argument("--manual-short", action="store_true")
    a = ap.parse_args()

    with Tab5(timeout=a.seconds + 120) as t:
        before = t.cmd("i2c stats", 10)
        if a.manual_short:
            print("Short SDA (G31) to GND for ~100 ms a few times during the run.")
        t0 = time.time()
        out = t.cmd(f"i2c stress {a.seconds}")
        print(out)
        after = t.cmd("i2c stats", 10)
        scan = t.cmd("i2c scan", 30)
    print(f"\nran {time.time() - t0:.0f} s\n--- stats before ---\n{before}\n--- stats after ---\n{after}")
    m = re.search(r"STRESS (PASS|FAIL).*lock timeouts (\d+)", out)
    devices = len(re.findall(r"^\s+0x[0-9a-f]{2}\s", scan, re.M))
    ok = m and m.group(1) == "PASS" and m.group(2) == "0" and devices >= 9
    print(f"I2C STRESS {'PASS' if ok else 'FAIL'}: {devices} devices answer after the run")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
