#!/usr/bin/env python3
"""Reset the board running the device-test app (test/device) and collect the
Unity results. A USB drop (a panic reboot re-enumerates the port) is waited
out and reported."""
import argparse
import re
import sys

from t5link import Tab5

BEGIN, END = "#### T5 DEVICE TESTS BEGIN", "#### T5 DEVICE TESTS END"   # test/device/main/test_main.c


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--timeout", type=float, default=150)
    ap.add_argument("--no-reset", action="store_true")
    a = ap.parse_args()
    with Tab5(a.port).open(console=False) as t:
        if not a.no_reset:
            t.reset(console=False)
        t.ser.write(b"\n")              # the test app waits for this (test/device/main/test_main.c)
        out, _ = t.read_until(re.compile(re.escape(END.encode()) + rb" failures=\d+"), a.timeout, reconnect=True)
    buf = out.decode(errors="replace")
    start = buf.find(BEGIN)
    print(buf[start:] if start >= 0 else buf[-4000:])
    if t.drops:
        print(f"[{t.drops} USB drop(s) during the run]")
    m = re.search(re.escape(END) + r" failures=(\d+)", buf)
    return 0 if m and m.group(1) == "0" else 1


if __name__ == "__main__":
    sys.exit(main())
