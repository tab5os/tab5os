#!/usr/bin/env python3
"""Plan 9.6: OS update cycles with forced failures. After every cycle the
Tab5 must boot, and boot the slot the cycle predicts.

Each cycle installs the running image into the other slot with `ota selftest`
(the whole verify/write/switch path, without moving 3.4 MB over a link),
then one of:

  ok        restart; the new slot runs on trial and is confirmed after 60 s
  crash     restart, then reset the chip during the trial, as an OS crash
            would (apps' own crashes are contained and don't restart it):
            the next boot must be the previous slot again (rollback)
  interrupt reset in the middle of the install: the old slot must still boot
            and the half-written one must not
  badsig    a tampered signature (needs a firmware built with T5_OTA_KEYS_DIR
            holding KEY's public half): refused, nothing changes

    tools/t5/t5 hil ota_cycles --cycles 1000                  # the plan's run: days
    tools/t5/t5 hil ota_cycles --cycles 4 --mix ok,crash,interrupt   # a few minutes

--mode powercut cuts the main rail instead of a chip reset for `interrupt`
(the RTC countdown trick from fs_powerloss.py; someone may need to press the
power button if the board stays off).
"""
import argparse
import random
import re
import sys
import time

from t5link import SERIAL_ERRORS, connect, restart


def state(t):
    out = t.cmd("ota", 10)
    sysout = t.cmd("sys", 10)
    part = re.search(r"partition\s+(ota_\d)", sysout)
    return {
        "slot": part.group(1) if part else "?",
        "trial": "on trial" in out,
        "safe": "safe mode on" in out,
        "failed": int(re.search(r"(\d+) failed boots", out).group(1)) if "failed boots" in out else -1,
        "raw": out,
    }


def wait_confirmed(t, timeout=90):
    end = time.time() + timeout
    while time.time() < end:
        if not state(t)["trial"]:
            return True
        time.sleep(5)
    return False


def selftest(t, sig=None):
    cmd = "ota selftest" + (f" {sig}" if sig else "")
    return t.cmd(cmd, 120)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cycles", type=int, default=4)
    ap.add_argument("--mix", default="ok,crash,interrupt", help="comma list of ok, crash, interrupt, badsig")
    ap.add_argument("--sig", help="'SIG128HEX KEYID64HEX' of a signed build, for badsig")
    ap.add_argument("--mode", choices=("reset", "powercut"), default="reset")
    a = ap.parse_args()
    mix = a.mix.split(",")
    t = connect(cmd_timeout=30)
    s = state(t)
    print(f"start: {s['slot']}, {'trial' if s['trial'] else 'confirmed'}")
    if s["trial"]:
        wait_confirmed(t)
    fails = 0
    for i in range(1, a.cycles + 1):
        kind = mix[(i - 1) % len(mix)] if len(mix) > 1 else mix[0]
        before = state(t)["slot"]
        other = "ota_1" if before == "ota_0" else "ota_0"
        t0 = time.time()
        expect = before
        if kind == "interrupt":
            # Reset during the install (erasing the slot and writing it take
            # about 10 s); watch the output to know whether it finished first.
            t.flush_input()
            t.send("ota selftest")
            done = b"installed in" in t.read_for(random.uniform(1.0, 9.0))
            if a.mode == "powercut":
                t0 = time.time()
                t.send("rtc timer 1")
                if not t.read_until_gone(10):
                    print(f"  cycle {i}: power did not drop")
                t = connect(timeout=180, cmd_timeout=30, booted_after=t0)
            else:
                t = restart(t, "reset")
            # Finished: the new slot boots on trial and is confirmed; not
            # finished: the old one, untouched.
            expect = other if done else before
            if done and not wait_confirmed(t):
                print(f"  cycle {i}: never confirmed")
                fails += 1
        elif kind == "badsig":
            if not a.sig:
                sys.exit("badsig needs --sig")
            sig, kid = a.sig.split()
            bad = bytearray.fromhex(sig)
            bad[random.randrange(64)] ^= 1 << random.randrange(8)
            out = selftest(t, f"{bad.hex()} {kid}")
            if "DENIED" not in out:
                print(f"  cycle {i} badsig: accepted a tampered signature!\n{out}")
                fails += 1
            t = restart(t)
        else:
            out = selftest(t)
            if not re.search(r"^OK", out, re.M):
                print(f"  cycle {i}: install failed: {out[-200:]}")
                fails += 1
                continue
            t = restart(t)
            s = state(t)
            if s["slot"] != other or not s["trial"]:
                print(f"  cycle {i}: expected {other} on trial, got {s['slot']} {'trial' if s['trial'] else ''}")
                fails += 1
            if kind == "crash":
                # The OS contains an app's crash, so the stand-in for an OS
                # that dies during its trial is a chip reset.
                t = t.reset()
                expect = before          # rolled back
            else:
                expect = other
                if not wait_confirmed(t):
                    print(f"  cycle {i}: never confirmed")
                    fails += 1
        s = state(t)
        ok = s["slot"] == expect and not s["trial"]
        fails += not ok
        print(f"cycle {i:4d} {kind:<9} {before} -> {s['slot']} (expected {expect}) "
              f"{'ok' if ok else 'FAIL'} {time.time() - t0:5.0f} s{' SAFE MODE' if s['safe'] else ''}")
    print(f"{a.cycles} cycles, {fails} failures")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
