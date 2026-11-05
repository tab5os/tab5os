#!/usr/bin/env python3
"""Log the battery service to a CSV on this machine, once a second.

For discharge traces (plan Phase 2, "trace collection") prefer the device's
own logger, which keeps running on battery with no cable:
    t5 console "bat log start"        # /sd/battery-<epoch>.csv
This script is for watching charging on USB, or a short run at the bench.

    ~/.espressif/python_env/idf6.1_py3.15_env/bin/python test/hil/battery_log.py out.csv --seconds 3600
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools", "t5"))
from t5link import Tab5  # noqa: E402

LINE1 = re.compile(r"(\d+) mV\s+(-?[\d.]+) mA\s+(-?\d+) mW\s+(-?\d+)%\s+(-?\d+)/(\d+) mAh")
LINE2 = re.compile(r"usb (\w+), ([^,]+), CHG_EN (\d), fast (\d)")
TEMP = re.compile(r"imu ([\d.-]+|-) ?C?, chip ([\d.-]+|-)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--seconds", type=int, default=3600)
    a = ap.parse_args()
    with Tab5(timeout=10) as t, open(a.csv, "w") as f:
        f.write("t,mv,ma,mw,pct,charge_mah,capacity_mah,usb,state,chg_en,fast,imu_c,chip_c\n")
        t0 = time.time()
        while time.time() - t0 < a.seconds:
            out = t.cmd("bat")
            m1, m2, m3 = LINE1.search(out), LINE2.search(out), TEMP.search(out)
            if m1 and m2:
                row = [f"{time.time() - t0:.1f}", *m1.groups(), m2.group(1), m2.group(2).strip(), m2.group(3),
                       m2.group(4), m3.group(1) if m3 else "", m3.group(2) if m3 else ""]
                f.write(",".join(row) + "\n")
                f.flush()
                print(" ".join(row), flush=True)
            time.sleep(max(0.0, 1.0 - (time.time() - t0) % 1.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
