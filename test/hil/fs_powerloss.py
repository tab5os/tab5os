#!/usr/bin/env python3
"""Bench 1.8: interrupt /data writes and check LittleFS + atomic writes survive.

Each cycle: read back /data/stress.bin (must be intact, sequence must never go
backwards), start `fs stress`, then interrupt it after a random delay.

  --mode reset     chip reset over USB (RTS). Fast, but flash keeps power.
  --mode powercut  real main-rail power cut: the RTC countdown interrupt makes
                   the PMS150G power controller switch the board off (bench
                   0.7 finding); the next pulse switches it back on, and boot
                   stops the timer.

    ~/.espressif/python_env/idf6.1_py3.15_env/bin/python test/hil/fs_powerloss.py --mode powercut --cycles 10
"""
import argparse
import os
import random
import re
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools", "t5"))
from t5link import Tab5, find_port, wait_port  # noqa: E402


def connect(retries=40):
    """Open the console and wait until it answers (the board may still be booting)."""
    for _ in range(retries):
        try:
            t = Tab5(timeout=8).open()
            t.cmd("rtc", 8)
            t.timeout = 30
            return t
        except Exception:
            try:
                t.close()
            except Exception:
                pass
            time.sleep(1)
    raise RuntimeError("Tab5 not reachable")


def verify(t):
    out = t.cmd("fs verify")
    m = re.search(r"VERIFY (\w+)(?: seq (\d+))?", out)
    return (m.group(1), int(m.group(2)) if m and m.group(2) else 0) if m else ("?", 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=["reset", "powercut"], default="reset")
    ap.add_argument("--cycles", type=int, default=20)
    a = ap.parse_args()

    last_seq, bad = 0, 0
    for c in range(1, a.cycles + 1):
        wait_port(180)
        time.sleep(2)
        t = connect()
        if a.mode == "powercut":
            t.cmd("rtc clear")
        state, seq = verify(t)
        ok = state in ("ok", "none") and seq >= last_seq
        bad += not ok
        print(f"cycle {c:3d}: verify {state:7s} seq {seq:6d} {'ok' if ok else 'FAIL (corrupt or went backwards)'}",
              flush=True)
        last_seq = max(last_seq, seq)
        delay = random.uniform(0.3, 3.0)
        if a.mode == "powercut":
            k = random.randint(2, 4)
            t.cmd(f"rtc timer {k}")
            t.ser.write(b"fs stress 100000\r\n")
            # Wait for the power to go, then come back.
            end = time.time() + k + 15
            while time.time() < end and find_port():
                try:
                    t.ser.read(4096)
                except Exception:
                    break
                time.sleep(0.1)
            try:
                t.close()
            except Exception:
                pass
            if find_port():
                print("           power did not drop; resetting instead", flush=True)
                t = connect()
                t.reset()
                t.close()
        else:
            t.ser.write(b"fs stress 100000\r\n")
            time.sleep(delay)
            try:
                t.reset(settle=3)
                t.close()
            except Exception:
                pass
    wait_port(180)
    time.sleep(2)
    t = connect()
    if a.mode == "powercut":
        t.cmd("rtc clear")
    state, seq = verify(t)
    final_ok = state in ("ok", "none") and seq >= last_seq
    bad += not final_ok
    print(f"final:     verify {state} seq {seq}")
    print(f"POWERLOSS {'PASS' if bad == 0 else 'FAIL'}: {a.cycles} cycles ({a.mode}), {bad} bad")
    t.close()
    return bad != 0


if __name__ == "__main__":
    sys.exit(main())
