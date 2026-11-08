#!/usr/bin/env python3
"""Plan 9.6: the mixed-use soak. Runs for --hours (the plan asks for 7 days)
doing what a person and the apps would, and records anything that looks
like wear: unexpected resets, crash records, internal-RAM low-water mark,
Wi-Fi reconnects.

Each round picks one of:
  app      open a built-in app (Settings, Files, Power, Terminal, Editor...),
           tap around, leave it
  python   run a short script on the device (UI, ports, JSON, a GC-heavy loop)
  net      check Wi-Fi state; reset the link if it's down
  idle     leave it alone for a few minutes (screen off, sleep, wake)
  rotate   change the rotation and back

Every round appends a CSV line (time, uptime, resets, internal heap free and
minimum, PSRAM free, Wi-Fi state, crash count) to --csv, so a leak or a slow
decline shows up as a trend.

    PY=~/.espressif/python_env/idf6.1_py3.15_env/bin/python
    $PY test/hil/soak.py --hours 168 --csv soak.csv        # the plan's run
    $PY test/hil/soak.py --minutes 4                        # a smoke test
"""
import argparse
import csv
import os
import random
import re
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools", "t5"))
from t5link import Tab5  # noqa: E402

APPS = ["com.t5.settings", "com.t5.files", "com.t5.power", "com.t5.ports", "com.t5.appmanager",
        "com.t5.terminal", "com.t5.editor"]
SCRIPTS = [
    "import t5, json; print(json.dumps(t5.power.battery()))",
    "import gc; l=[bytearray(1000) for i in range(300)]; del l; gc.collect(); print(gc.mem_free())",
    "import t5; p=t5.port.open('grove', mode='i2c'); print(p.scan()); p.close()",
    "import t5; print(t5.net.status()['state'], t5.time.local())",
    "s=sum(i*i for i in range(20000)); print(s)",
]


def connect(retries=60):
    for _ in range(retries):
        try:
            t = Tab5(timeout=10).open()
            t.cmd("sys", 10)
            t.timeout = 40
            return t
        except Exception:
            try:
                t.close()
            except Exception:
                pass
            time.sleep(2)
    raise RuntimeError("Tab5 not reachable")


def sample(t):
    s = t.cmd("sys", 10)
    up = re.search(r"uptime\s+([\d.]+)", s)
    hi = re.search(r"heap int\s+free (\d+)\s+largest \d+\s+min-ever (\d+)", s)
    hp = re.search(r"heap psram free (\d+)", s)
    rr = re.search(r"reset\s+(\S+)", s)
    net = t.cmd("net", 10)
    st = re.search(r"state (\w+)", net)
    crashes = t.cmd("app crashes", 10).count("\n")
    return {
        "uptime": float(up.group(1)) if up else -1,
        "int_free": int(hi.group(1)) if hi else -1,
        "int_min": int(hi.group(2)) if hi else -1,
        "psram_free": int(hp.group(1)) if hp else -1,
        "reset": rr.group(1) if rr else "?",
        "net": st.group(1) if st else "?",
        "crash_lines": crashes,
    }


def round_app(t):
    app = random.choice(APPS)
    t.cmd(f"app run {app}", 15)
    for _ in range(random.randint(1, 4)):
        t.cmd(f"ui tap {random.randint(40, 1240)} {random.randint(120, 700)}", 5)
        time.sleep(random.uniform(0.3, 2.0))
    time.sleep(random.uniform(1, 6))
    t.cmd("app exit", 10)
    return app


def round_python(t):
    code = random.choice(SCRIPTS)
    out = t.cmd("py exec " + code, 40)
    if "[done]" not in out:
        print(f"    python: {out[-160:]}")
    return "python"


def round_net(t):
    if "state connected" not in t.cmd("net", 10):
        t.cmd("net reset", 30)
        return "net (reset)"
    return "net"


def round_idle(t, quick):
    time.sleep(20 if quick else random.uniform(150, 300))     # long enough for screen off and sleep
    return "idle"


def round_rotate(t):
    t.cmd(f"ui rot {random.choice([0, 90, 180, 270])}", 10)
    time.sleep(2)
    t.cmd("ui rot 270", 10)
    return "rotate"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hours", type=float, default=0)
    ap.add_argument("--minutes", type=float, default=0)
    ap.add_argument("--csv", default="soak.csv")
    a = ap.parse_args()
    seconds = a.hours * 3600 + a.minutes * 60 or 3600 * 168
    quick = seconds < 1800
    t = connect()
    first = sample(t)
    resets = 0
    last_up = first["uptime"]
    end = time.time() + seconds
    new = not os.path.exists(a.csv)
    with open(a.csv, "a", newline="") as f:
        w = csv.writer(f)
        if new:
            w.writerow(["time", "round", "uptime_s", "resets", "int_free", "int_min", "psram_free", "net", "crash_lines"])
        n = 0
        while time.time() < end:
            n += 1
            kind = random.choices(["app", "python", "net", "idle", "rotate"], [5, 3, 1, 1, 1])[0]
            try:
                what = {"app": round_app, "python": round_python, "net": round_net,
                        "idle": lambda tt: round_idle(tt, quick), "rotate": round_rotate}[kind](t)
                s = sample(t)
            except Exception as e:      # the board went away: count it, reconnect
                print(f"    lost the console ({e}); reconnecting")
                try:
                    t.close()
                except Exception:
                    pass
                t = connect()
                s = sample(t)
                what = kind + " (lost)"
            if s["uptime"] < last_up:
                resets += 1
                print(f"    RESET ({s['reset']}) during round {n}")
            last_up = s["uptime"]
            w.writerow([time.strftime("%Y-%m-%d %H:%M:%S"), what, s["uptime"], resets, s["int_free"], s["int_min"],
                        s["psram_free"], s["net"], s["crash_lines"]])
            f.flush()
            print(f"round {n:5d} {what:<20} up {s['uptime']:8.0f} s  int {s['int_free']:6d} (min {s['int_min']:6d})"
                  f"  psram {s['psram_free'] // 1024:6d} KB  net {s['net']}  resets {resets}")
    print(f"done: {n} rounds, {resets} unexpected resets; data in {a.csv}")
    return 1 if resets else 0


if __name__ == "__main__":
    sys.exit(main())
