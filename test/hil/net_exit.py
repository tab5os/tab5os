#!/usr/bin/env python3
"""Exit an app while its tasks are blocked inside network calls, and check
that the Tab5 stays up (it used to reboot: the tcpip task signalled the
select() semaphore of a deleted app task).

  netexit   test/apps/netexit: four workers blocked in t5_http_fetch,
            select(), recv() and accept() (build it with `t5 apps`)
  --app ID  any installed app instead, exited --after seconds after launch
            (com.t5.example.weather reproduced the original crash at 0.2 s)

Each round: run, wait until the workers are in their calls, `app exit`,
then check that the console answers with uptime still growing, that the
exit was clean and that `net handles` lists nothing of the app.

    tools/t5/t5 hil net_exit [--rounds 10] [--app com.t5.example.weather --after 0.2]
"""
import argparse
import os
import re
import sys
import time

from t5link import connect

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PKG = os.path.join(ROOT, "test", "apps", "build", "netexit.t5app")
NETEXIT = "com.t5.test.netexit"
PANIC = re.compile(rb"assert failed|Guru Meditation|abort\(\) was called|Rebooting|ESP-ROM")


def one_round(t, app_id, after, n):
    """None if the round passed, else what went wrong."""
    up0 = t.uptime()
    t.flush_input()
    t.send(f"app run {app_id}")
    if app_id == NETEXIT:
        text, m = t.read_until(re.compile(rb"netexit: armed"), 20)
        if not m:
            return "no 'netexit: armed' within 20 s:\n" + text.decode(errors="replace")[-600:]
    else:
        text, _ = t.read_until(re.compile(rb"(?!)"), after)
    t0 = time.time()
    try:
        out = t.cmd("app exit", 30)
    except TimeoutError as e:
        out = str(e)
    took = time.time() - t0
    tail = t.read_for(1.5)
    log = (text + out.encode() + tail).decode(errors="replace")
    if PANIC.search(text + out.encode() + tail):
        return f"panic on exit:\n{log[-1500:]}"
    try:
        up1 = t.uptime()
    except (TimeoutError, RuntimeError) as e:
        return f"console gone after exit ({e}):\n{log[-1500:]}"
    if up1 < up0:
        return f"rebooted (uptime {up0:.0f} -> {up1:.0f} s):\n{log[-1500:]}"
    handles = t.cmd("net handles", 10)
    if app_id in handles:
        return f"handles left open:\n{handles}"
    notes = [ln.strip() for ln in log.replace("\r", "").splitlines()
             if re.search(r"network call|closed \d+ socket|still hold|returned", ln)]
    print(f"round {n:3d} ok: exit took {took:.1f} s" + "".join(f"\n    {x}" for x in notes))
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=10)
    ap.add_argument("--app", default=NETEXIT)
    ap.add_argument("--after", type=float, default=0.2, help="seconds before exiting (--app only)")
    a = ap.parse_args()
    with connect(cmd_timeout=30) as t:
        t.cmd("app exit", 10)
        if a.app == NETEXIT:
            if not os.path.isfile(PKG):
                sys.exit("not built: netexit (run `t5 apps`)")
            with open(PKG, "rb") as f:
                t.install(f.read(), "netexit")
        bad = 0
        for n in range(1, a.rounds + 1):
            why = one_round(t, a.app, a.after, n)
            if why:
                bad += 1
                print(f"round {n:3d} FAIL: {why}")
                break
    print(f"NET EXIT {'PASS' if not bad else 'FAIL'}: {n - bad} of {a.rounds} rounds")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
