#!/usr/bin/env python3
"""Plan 9.6: OS update cycles with forced failures. After every cycle the
Tab5 must boot, and boot the slot the cycle predicts.

Each cycle installs the running image into the other slot with `ota selftest`
(the whole verify/write/switch path, without moving 3.4 MB over a link),
then one of:

  ok        restart; the new slot runs on trial and is confirmed after 60 s
  crash     restart, then crash it during the trial (the crasher test app):
            the next boot must be the previous slot again (rollback)
  interrupt reset in the middle of the install: the old slot must still boot
            and the half-written one must not
  badsig    a tampered signature (needs a firmware built with T5_OTA_KEYS_DIR
            holding KEY's public half): refused, nothing changes

    PY=~/.espressif/python_env/idf6.1_py3.15_env/bin/python
    $PY test/hil/ota_cycles.py --cycles 1000                  # the plan's run: days
    $PY test/hil/ota_cycles.py --cycles 4 --mix ok,crash,interrupt   # a few minutes

--mode powercut cuts the main rail instead of a chip reset for `interrupt`
(the RTC countdown trick from fs_powerloss.py; someone may need to press the
power button if the board stays off).
"""
import argparse
import os
import random
import re
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools", "t5"))
from t5link import Tab5  # noqa: E402


def connect(retries=60):
    for _ in range(retries):
        try:
            t = Tab5(timeout=8).open()
            t.cmd("ota", 8)
            t.timeout = 30
            return t
        except Exception:
            try:
                t.close()
            except Exception:
                pass
            time.sleep(1)
    raise RuntimeError("Tab5 not reachable")


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


def reboot(t):
    try:
        t.ser.write(b"reboot\r\n")
    except Exception:
        pass
    t.close()
    time.sleep(8)
    return connect()


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
    t = connect()
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
            t.ser.reset_input_buffer()
            t.ser.write(b"ota selftest\r\n")
            stop = time.time() + random.uniform(1.0, 9.0)
            seen = b""
            while time.time() < stop:
                seen += t.ser.read(256)
            done = b"installed in" in seen
            if a.mode == "powercut":
                t.ser.write(b"rtc timer 1\r\n")
                time.sleep(3)
            else:
                t.ser.dtr = False
                t.ser.rts = True
                time.sleep(0.1)
                t.ser.rts = False
            t.close()
            time.sleep(8)
            t = connect()
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
            t = reboot(t)
        else:
            out = selftest(t)
            if not re.search(r"^OK", out, re.M):
                print(f"  cycle {i}: install failed: {out[-200:]}")
                fails += 1
                continue
            t = reboot(t)
            s = state(t)
            if s["slot"] != other or not s["trial"]:
                print(f"  cycle {i}: expected {other} on trial, got {s['slot']} {'trial' if s['trial'] else ''}")
                fails += 1
            if kind == "crash":
                try:
                    t.cmd("app run com.t5.test.crasher", 5)
                except Exception:
                    pass
                t.close()
                time.sleep(10)
                t = connect()
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
