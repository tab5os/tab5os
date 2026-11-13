#!/usr/bin/env python3
"""Exclusive USB-C (t5_usbc_raw_*) against this computer: the Tab5's
`usbc raw S` self-test is a vendor-class device (303A:4FF0) that answers
vendor request 1 with "tab5" and echoes bulk OUT 0x01 on IN 0x81.

Needs pyusb and libusb (brew install libusb):
    /tmp/t5io-venv/bin/pip install pyusb pyserial
    PYTHONPATH=tools/t5 /tmp/t5io-venv/bin/python test/hil/usbc_raw.py
"""
import argparse
import os
import sys
import time

import usb.backend.libusb1
import usb.core
import usb.util
from t5link import connect

VID, PID = 0x303A, 0x4FF0
# Homebrew's libusb isn't on the default search path.
BACKEND = usb.backend.libusb1.get_backend(
    find_library=lambda _: next((p for p in ("/opt/homebrew/lib/libusb-1.0.dylib", "/usr/local/lib/libusb-1.0.dylib")
                                 if os.path.exists(p)), None))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--secs", type=int, default=20)
    ap.add_argument("-n", type=int, default=50)
    a = ap.parse_args()
    with connect(cmd_timeout=10) as t:
        if "self-test" not in t.cmd(f"usbc raw {a.secs}"):
            print("didn't start")
            return 1
    t0 = time.time()
    dev = None
    while time.time() - t0 < 10 and dev is None:
        dev = usb.core.find(idVendor=VID, idProduct=PID, backend=BACKEND)
        time.sleep(0.2)
    ok = True
    if dev is None:
        print("FAIL: the device didn't show up")
        ok = False
    else:
        print(f"enumerated after {time.time() - t0:.1f} s: {dev.product!r} by {dev.manufacturer!r}")
        dev.set_configuration()
        got = bytes(dev.ctrl_transfer(0xC0, 1, 0, 0, 4, timeout=1000))
        print(f"vendor request: {got!r}")
        ok &= got == b"tab5"
        good, worst = 0, 0.0
        for i in range(a.n):
            data = os.urandom(1 + i % 64)
            s = time.time()
            dev.write(0x01, data, timeout=1000)
            back = bytes(dev.read(0x81, 64, timeout=1000))
            worst = max(worst, time.time() - s)
            good += back == data
        print(f"bulk echo {good}/{a.n}, worst {worst * 1000:.1f} ms")
        ok &= good == a.n
        usb.util.dispose_resources(dev)
    time.sleep(max(0, a.secs - (time.time() - t0)) + 3)
    status = None
    end = time.time() + 20
    while status is None and time.time() < end:
        try:
            with connect(cmd_timeout=10) as t:
                status = t.cmd("usbc raw status")
        except Exception:
            time.sleep(1)
    if status is None:
        print("FAIL: the console didn't come back")
        return 1
    print("console back: " + status.strip().splitlines()[-1])
    ok &= ", OK," in status
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
