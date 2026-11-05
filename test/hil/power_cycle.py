#!/usr/bin/env python3
"""Plan 2.13: cycle Sleep and wake many times and check every resume.

Each cycle: `power sleep`, wait until the state is sleep, `power wake`, wait
until it is active again, then read the panel's DCS ID and power mode over
DSI (the panel must still answer) and the power stats (resume time, hook
failures). Without --net-lock the C6 is powered off and on every cycle too.

Draw per state needs an external meter in series with the pack; pass
--meter-pause to stop in each state for a reading (bench-procedures.md, 0.9).

    ~/.espressif/python_env/idf6.1_py3.15_env/bin/python test/hil/power_cycle.py           # 150 cycles, ~5 min
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools", "t5"))
from t5link import Tab5  # noqa: E402


def wait_state(t, want, timeout=8.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        out = t.cmd("power", 10)
        m = re.search(r"^state ([a-z-]+)", out, re.M)
        if m and m.group(1) == want:
            return out
        time.sleep(0.1)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cycles", type=int, default=150)   # ~5 min
    ap.add_argument("--net-lock", action="store_true", help="hold a net lock: the C6 stays up in power save")
    ap.add_argument("--meter-pause", action="store_true", help="pause in each state for a meter reading")
    a = ap.parse_args()

    fails, resume_ms, panel_bad = [], [], 0
    with Tab5(timeout=20) as t:
        t.cmd("power unlock screen")
        if a.net_lock:
            t.cmd("power lock net")
        before = t.cmd("power", 10)
        t0 = time.time()
        for i in range(1, a.cycles + 1):
            t.cmd("power sleep")
            if not wait_state(t, "sleep"):
                fails.append((i, "did not reach sleep"))
                t.cmd("power wake")
                continue
            if a.meter_pause:
                input(f"cycle {i}: in Sleep. Note the meter, then press Enter")
            t.cmd("power wake")
            out = wait_state(t, "active")
            if not out:
                fails.append((i, "did not wake"))
                continue
            m = re.search(r"last resume (\d+) ms", out)
            if m:
                resume_ms.append(int(m.group(1)))
            ids = t.cmd("disp ids", 10)
            if not re.search(r"power.mode\s*0x9c|pm 0x9c|0x9c", ids, re.I):
                panel_bad += 1
                fails.append((i, "panel: " + ids.strip().replace("\n", " ")[:80]))
            if i % 50 == 0:
                print(f"{i} cycles, {len(fails)} failures, {time.time() - t0:.0f} s", flush=True)
        after = t.cmd("power", 10)
        hooks = t.cmd("power hooks", 10)
        if a.net_lock:
            t.cmd("power unlock net")
    print(f"\n--- before ---\n{before}\n--- after ---\n{after}\n--- hooks ---\n{hooks}")
    rf = re.search(r"resumes (\d+) \(failures (\d+)\)", after)
    hook_fail = int(rf.group(2)) if rf else -1
    if resume_ms:
        s = sorted(resume_ms)
        print(f"resume ms: min {s[0]} median {s[len(s) // 2]} p99 {s[int(len(s) * 0.99) - 1]} max {s[-1]}")
    for f in fails[:20]:
        print("FAIL cycle %d: %s" % f)
    ok = not fails and hook_fail == 0
    print(f"POWER CYCLE {'PASS' if ok else 'FAIL'}: {a.cycles} cycles, {len(fails)} failed, "
          f"{panel_bad} panel errors, {hook_fail} resume hook failures")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
