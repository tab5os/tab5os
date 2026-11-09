#!/usr/bin/env python3
"""Phase 3 exit checks against a Tab5 already joined to Wi-Fi.

1. Sleep with a TCP connection open: start the device's echo server (it
   holds a net lock), force Sleep, send packets from this machine and time
   the replies. Each reply says which power state it found ([sleep]).
   Pass: every reply arrives within 1 s while the device reports sleep.
2. Reconnect after a C6 reset: `net reset` N times. Pass: each one is back
   to "connected" within 5 s.

    tools/t5/t5 hil net_check
"""
import argparse
import re
import sys
import time

import _net as hn
from t5link import connect

PORT = 7777


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--packets", type=int, default=10)
    ap.add_argument("--resets", type=int, default=5)
    a = ap.parse_args()
    ok = True
    with connect(cmd_timeout=40) as t:
        st = t.cmd("net")
        m = re.search(r"^ip (\d+\.\d+\.\d+\.\d+)", st, re.M)
        if not m:
            print("not connected:\n" + st)
            return 1
        ip = m.group(1)
        if "listening" not in t.cmd(f"net echo {PORT}"):
            print("the echo server didn't start")
            return 1
        s = hn.connect_retry(ip, PORT)
        t.cmd("power sleep")
        if not t.poll("power", r"^state sleep,", 10, every=0.2):
            print("the Tab5 didn't go to Sleep")
        worst, asleep = 0.0, 0
        for i in range(a.packets):
            t0 = time.time()
            s.sendall(b"ping\n")
            data = b""
            while not data.endswith(b"ping\n"):
                data += s.recv(256)
            dt = time.time() - t0
            worst = max(worst, dt)
            asleep += b"[sleep]" in data
            time.sleep(2)
        s.close()
        t.cmd("net echo stop")
        t.cmd("power wake")
        p1 = worst < 1.0 and asleep == a.packets
        ok &= p1
        print(f"sleep echo: {asleep}/{a.packets} answered in Sleep, worst {worst * 1000:.0f} ms -> "
              f"{'PASS' if p1 else 'FAIL'}")

        t.cmd("power lock screen")
        times = []
        for i in range(a.resets):
            out = t.cmd("net reset", 40)
            m = re.search(r"C6 reset -> (\S+) in (\d+) ms", out)
            if m and m.group(1) == "connected":
                times.append(int(m.group(2)))
            else:
                times.append(None)
            time.sleep(2)
        t.cmd("power unlock screen")
    good = [x for x in times if x is not None]
    p2 = len(good) == len(times) and max(good) < 5000
    ok &= p2
    print(f"C6 reset to connected (ms): {times} -> {'PASS' if p2 else 'FAIL'}")
    print(f"NET CHECK {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
