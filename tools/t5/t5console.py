#!/usr/bin/env python3
"""Send bring-up console commands to a Tab5 and print the replies.

usage: t5console.py [--port PORT] [--reset] "cmd 1" "cmd 2" ...
Run with the ESP-IDF Python (pyserial); `t5 console` does.
"""
import argparse
import sys

from t5link import Tab5


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    ap.add_argument("--reset", action="store_true", help="hard reset first and show the boot log")
    ap.add_argument("--timeout", type=float, default=20.0)
    ap.add_argument("cmds", nargs="*")
    a = ap.parse_args()
    try:
        t = Tab5(a.port, timeout=a.timeout).open()
    except RuntimeError as e:
        sys.exit(str(e))
    rc = 0
    with t:
        if a.reset:
            sys.stdout.write(t.reset().boot_log)
        for cmd in a.cmds:
            try:
                out = t.cmd(cmd)
            except TimeoutError as e:
                print(f"\n### {cmd}\n[{e}]")
                rc = 1
                continue
            sys.stdout.write(f"\n### {cmd}\n{out}\n")
    return rc


if __name__ == "__main__":
    sys.exit(main())
