#!/usr/bin/env python3
"""Run the self-checking test apps (test/apps) on a Tab5 and judge what they
print: each one writes "<name>: PASS ..." / "<name>: FAIL ..." lines and a
last line this script waits for. They go across USB (`app put`), are
installed, run and closed again; build them first with `t5 apps`.

  fscope   file isolation for native apps (own folder, others, OS, shared, SD)
  fspy     the same for Python apps
  netleak  sockets and the HTTP server through t5_net, and the BSD layer
  nonet    an app without the net permission is refused

    tools/t5/t5 hil app_checks [--only fscope,fspy]
"""
import argparse
import os
import re
import sys

from t5link import connect

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BUILD = os.path.join(ROOT, "test", "apps", "build")

# name: (app id, the line that ends its run). The ids are in test/apps/*/manifest.toml.
APPS = {
    "fscope": ("com.t5.test.fscope", rb"fscope: done"),
    "fspy": ("com.t5.test.fspy", rb"fspy: done"),
    "netleak": ("com.t5.test.netleak", rb"netleak: bsd (PASS|FAIL)"),
    "nonet": ("com.t5.test.nonet", rb"nonet: .* -> (PASS|FAIL)"),
}


def run_app(t, name, app_id, last):
    """(passes, failures, output) of one app's run."""
    with open(os.path.join(BUILD, f"{name}.t5app"), "rb") as f:
        data = f.read()
    t.cmd("app exit", 10)
    try:
        t.install(data, name)
    except (RuntimeError, ValueError) as e:
        return 0, 1, str(e)
    t.flush_input()
    t.send(f"app run {app_id}")
    text, m = t.read_until(re.compile(last), 30)
    t.cmd("app exit", 10)
    text = text.decode(errors="replace")
    if not m:
        return 0, 1, text + f"\n[no '{last.decode()}' within 30 s]"
    # Native apps print straight to the console; a Python app's print goes
    # through the log ("I (..) py: <id>: fspy: ...").
    lines = re.findall(rf"(?:^|: )({name}: .*)$", text.replace("\r", ""), re.M)
    passes = sum(bool(re.search(r"\bPASS\b", x)) for x in lines)
    fails = sum(bool(re.search(r"\bFAIL\b", x)) for x in lines)
    return passes, fails, "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", help="comma list of " + ", ".join(APPS))
    a = ap.parse_args()
    names = a.only.split(",") if a.only else list(APPS)
    missing = [n for n in names if not os.path.isfile(os.path.join(BUILD, f"{n}.t5app"))]
    if missing:
        sys.exit(f"not built: {', '.join(missing)} (run `t5 apps`)")
    bad = 0
    with connect(cmd_timeout=30) as t:
        for name in names:
            app_id, last = APPS[name]
            passes, fails, out = run_app(t, name, app_id, last)
            ok = passes > 0 and fails == 0
            bad += not ok
            print(f"{name:8s} {'PASS' if ok else 'FAIL'}: {passes} passed, {fails} failed")
            if not ok:
                print("    " + out.replace("\n", "\n    "))
    print(f"APP CHECKS {'PASS' if not bad else 'FAIL'}: {len(names) - bad} of {len(names)}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
