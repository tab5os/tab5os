#!/usr/bin/env python3
"""Send bring-up console commands to a Tab5 and print the replies.

usage: t5console.py [--port PORT] [--reset] "cmd 1" "cmd 2" ...
Uses pyserial (present in the ESP-IDF Python env).
"""
import argparse
import os
import glob
import sys
import time

import serial

PROMPT = os.environ.get("T5_PROMPT", "tab5> ").encode()  # "recovery> " in the recovery image


def read_until_prompt(ser, timeout):
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            if buf.rstrip().endswith(PROMPT.rstrip()):
                return buf, True
    return buf, False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    ap.add_argument("--reset", action="store_true", help="hard reset first and show the boot log")
    ap.add_argument("--timeout", type=float, default=20.0)
    ap.add_argument("cmds", nargs="*")
    a = ap.parse_args()

    port = a.port or next(iter(sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))), None)
    if not port:
        sys.exit("no Tab5 found")
    ser = serial.Serial(port, 115200, timeout=0.1)
    if a.reset:
        # USB Serial/JTAG: RTS pulses reset, DTR low keeps it out of download mode.
        ser.dtr = False
        ser.rts = True
        time.sleep(0.1)
        ser.rts = False
        ser.close()
        time.sleep(2.5)  # USB re-enumerates after reset
        ser = serial.Serial(port, 115200, timeout=0.1)
        boot, _ = read_until_prompt(ser, 12)
        sys.stdout.write(boot.decode(errors="replace"))
    ser.write(b"\r\n")
    read_until_prompt(ser, 2)
    rc = 0
    for cmd in a.cmds:
        ser.write(cmd.encode() + b"\r\n")
        out, ok = read_until_prompt(ser, a.timeout)
        text = out.decode(errors="replace").replace("\r", "")
        sys.stdout.write(f"\n### {cmd}\n" + text.rsplit(PROMPT.decode(), 1)[0].split("\n", 1)[-1])
        if not ok:
            print(f"\n[timeout waiting for prompt after '{cmd}']")
            rc = 1
    ser.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
