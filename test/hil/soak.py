#!/usr/bin/env python3
"""Plan 9.6: the mixed-use soak. Runs for --hours (the plan asks for 7 days)
doing what a person and the apps would, and records anything that looks
like wear: unexpected resets, crash records, internal-RAM low-water mark,
Wi-Fi reconnects.

Each round picks one of:
  app      open a built-in app (--apps; Settings, Files, Power, Terminal,
           Editor...), tap around, leave it
  python   run a short script on the device (UI, ports, JSON, a GC-heavy loop)
  net      check Wi-Fi state; reset the link if it's down
  idle     leave it alone for a few minutes (screen off, sleep, wake)
  rotate   change the rotation and back to where it was

Every round appends a CSV line (time, uptime, resets, internal heap free and
minimum, PSRAM free, Wi-Fi state, app crash count) to --csv, so a leak or a
slow decline shows up as a trend. It fails on an unexpected reset, a new app
crash record, a Python script that doesn't finish, or a round that loses the
console.

    tools/t5/t5 hil soak --hours 168 --csv soak.csv        # the plan's run
    tools/t5/t5 hil soak --minutes 4                        # a smoke test
"""
import argparse
import csv
import os
import random
import re
import sys
import time

from t5const import define
from t5link import connect

DEFAULT_APPS = ",".join(define("t5_app/include/t5_app/registry.h", "T5_APP_ID_" + n)
                        for n in ("SETTINGS", "FILES", "POWER", "PORTS", "MANAGER", "TERMINAL", "EDITOR"))
SCRIPTS = [
    "import t5, json; print(json.dumps(t5.power.battery()))",
    "import gc; l=[bytearray(1000) for i in range(300)]; del l; gc.collect(); print(gc.mem_free())",
    "import t5; p=t5.port.open('grove', mode='i2c'); print(p.scan()); p.close()",
    "import t5; print(t5.net.status()['state'], t5.time.local())",
    "s=sum(i*i for i in range(20000)); print(s)",
]


def builtin_apps(t):
    """Ids of the built-in apps, from `app` (t5_diag/diag_app.c)."""
    return set(re.findall(r"^\s+(\S+)\s+\S+\s+built-in", t.cmd("app", 10), re.M))


def ui_rotation(t):
    """(rotation, width, height) from `ui` (t5_diag/diag_ui.c)."""
    m = re.search(r"^rotation (\d+) \((\d+)x(\d+)\)", t.cmd("ui", 10), re.M)
    if not m:
        raise RuntimeError("no rotation in `ui`")
    return tuple(int(x) for x in m.groups())


def crash_count(t):
    """Crashes recorded so far, summed over apps (`app crashes`)."""
    return sum(int(n) for n in re.findall(r"^\s+\S+\s+\d+ in a row, (\d+) total", t.cmd("app crashes", 10), re.M))


def sample(t):
    s = t.cmd("sys", 10)
    up = re.search(r"uptime\s+([\d.]+)", s)
    hi = re.search(r"heap int\s+free (\d+)\s+largest \d+\s+min-ever (\d+)", s)
    hp = re.search(r"heap psram free (\d+)", s)
    rr = re.search(r"reset\s+(\S+)", s)
    st = re.search(r"state (\w+)", t.cmd("net", 10))
    return {
        "uptime": float(up.group(1)) if up else -1,
        "int_free": int(hi.group(1)) if hi else -1,
        "int_min": int(hi.group(2)) if hi else -1,
        "psram_free": int(hp.group(1)) if hp else -1,
        "reset": rr.group(1) if rr else "?",
        "net": st.group(1) if st else "?",
        "crashes": crash_count(t),
    }


class Soak:
    def __init__(self, t, apps, quick):
        self.t, self.apps, self.quick = t, apps, quick
        self.rot, self.w, self.h = ui_rotation(t)
        self.py_fails = 0

    def app(self):
        app = random.choice(self.apps)
        self.t.cmd(f"app run {app}", 15)
        for _ in range(random.randint(1, 4)):
            # Below the status bar and above the bottom edge (gesture area).
            self.t.cmd(f"ui tap {random.randint(40, self.w - 40)} {random.randint(120, self.h - 20)}", 5)
            time.sleep(random.uniform(0.3, 2.0))
        time.sleep(random.uniform(1, 6))
        self.t.cmd("app exit", 10)
        return app

    def python(self):
        out = self.t.cmd("py exec " + random.choice(SCRIPTS), 40)
        if "[done]" not in out:
            self.py_fails += 1
            print(f"    python: {out[-160:]}")
            return "python (FAIL)"
        return "python"

    def net(self):
        if "state connected" not in self.t.cmd("net", 10):
            self.t.cmd("net reset", 30)
            return "net (reset)"
        return "net"

    def idle(self):
        time.sleep(20 if self.quick else random.uniform(150, 300))     # long enough for screen off and sleep
        return "idle"

    def rotate(self):
        self.t.cmd(f"ui rot {random.choice([0, 90, 180, 270])}", 10)
        time.sleep(2)
        self.t.cmd(f"ui rot {self.rot}", 10)
        return "rotate"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hours", type=float, default=0)
    ap.add_argument("--minutes", type=float, default=0)
    ap.add_argument("--csv", default="soak.csv")
    ap.add_argument("--apps", default=DEFAULT_APPS, help="comma list of built-in app ids to open")
    a = ap.parse_args()
    seconds = a.hours * 3600 + a.minutes * 60 or 3600 * 168
    t = connect(timeout=120, cmd_timeout=40)
    have = builtin_apps(t)
    apps = [x for x in a.apps.split(",") if x in have]
    if missing := sorted(set(a.apps.split(",")) - have):
        print(f"not built in, skipped: {', '.join(missing)}")
    if not apps:
        sys.exit("no apps to open")
    soak = Soak(t, apps, seconds < 1800)
    first = sample(t)
    resets, lost = 0, 0
    last_up = first["uptime"]
    end = time.time() + seconds
    new = not os.path.exists(a.csv)
    n = 0
    with open(a.csv, "a", newline="") as f:
        w = csv.writer(f)
        if new:
            w.writerow(["time", "round", "uptime_s", "resets", "int_free", "int_min", "psram_free", "net", "crashes"])
        while time.time() < end:
            n += 1
            kind = random.choices(["app", "python", "net", "idle", "rotate"], [5, 3, 1, 1, 1])[0]
            try:
                what = getattr(soak, kind)()
                s = sample(t)
            except (OSError, TimeoutError, RuntimeError) as e:      # the board went away: count it, reconnect
                lost += 1
                print(f"    lost the console ({e}); reconnecting")
                t.close()
                t = soak.t = connect(timeout=120, cmd_timeout=40)
                s = sample(t)
                what = kind + " (lost)"
            if s["uptime"] < last_up:
                resets += 1
                print(f"    RESET ({s['reset']}) during round {n}")
            last_up = s["uptime"]
            w.writerow([time.strftime("%Y-%m-%d %H:%M:%S"), what, s["uptime"], resets, s["int_free"], s["int_min"],
                        s["psram_free"], s["net"], s["crashes"]])
            f.flush()
            print(f"round {n:5d} {what:<20} up {s['uptime']:8.0f} s  int {s['int_free']:6d} (min {s['int_min']:6d})"
                  f"  psram {s['psram_free'] // 1024:6d} KB  net {s['net']}  resets {resets}")
    t.close()
    crashes = s["crashes"] - first["crashes"] if n else 0
    ok = not (resets or crashes or soak.py_fails or lost)
    print(f"SOAK {'PASS' if ok else 'FAIL'}: {n} rounds, {resets} unexpected resets, {crashes} new app crashes, "
          f"{soak.py_fails} Python failures, {lost} rounds lost the console; data in {a.csv}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
