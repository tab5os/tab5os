#!/usr/bin/env python3
"""Bench 1.8: interrupt /data writes and check LittleFS + atomic writes survive.

Each cycle: read back /data/stress.bin (must be intact, sequence must never go
backwards), start `fs stress`, then interrupt it after a random delay.

  --mode reset     chip reset over USB (RTS). Fast, but flash keeps power.
  --mode powercut  real main-rail power cut: the RTC countdown interrupt makes
                   the PMS150G power controller switch the board off (bench
                   0.7 finding); the next pulse switches it back on, and boot
                   stops the timer.

    tools/t5/t5 hil fs_powerloss --mode powercut --cycles 10
"""
import argparse
import random
import re
import sys
import time

from t5link import Tab5, connect, restart


def verify(t):
    out = t.cmd("fs verify")
    m = re.search(r"VERIFY (\w+)(?: seq (\d+))?", out)
    return (m.group(1), int(m.group(2)) if m.group(2) else 0) if m else ("?", 0)


def ready(mode):
    """A connection to the board once it has booted (after a power cut,
    someone may have to press the power button)."""
    t = connect(timeout=180, cmd_timeout=30)
    if mode == "powercut":
        t.cmd("rtc clear")
    return t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=["reset", "powercut"], default="reset")
    ap.add_argument("--cycles", type=int, default=20)
    a = ap.parse_args()

    last_seq, bad = 0, 0
    for c in range(1, a.cycles + 1):
        t = ready(a.mode)
        state, seq = verify(t)
        ok = state in ("ok", "none") and seq >= last_seq
        bad += not ok
        print(f"cycle {c:3d}: verify {state:7s} seq {seq:6d} {'ok' if ok else 'FAIL (corrupt or went backwards)'}",
              flush=True)
        last_seq = max(last_seq, seq)
        if a.mode == "powercut":
            k = random.randint(2, 4)
            t.cmd(f"rtc timer {k}")
            t.send("fs stress 100000")
            if not t.read_until_gone(k + 15):
                print("           power did not drop; resetting instead", flush=True)
                restart(Tab5().open(console=False), "reset").close()   # its console is busy with fs stress
        else:
            t.send("fs stress 100000")
            time.sleep(random.uniform(0.3, 3.0))      # interrupt it at a random point
            restart(t, "reset").close()
    t = ready(a.mode)
    state, seq = verify(t)
    final_ok = state in ("ok", "none") and seq >= last_seq
    bad += not final_ok
    print(f"final:     verify {state} seq {seq}")
    print(f"POWERLOSS {'PASS' if bad == 0 else 'FAIL'}: {a.cycles} cycles ({a.mode}), {bad} bad")
    t.close()
    return bad != 0


if __name__ == "__main__":
    sys.exit(main())
