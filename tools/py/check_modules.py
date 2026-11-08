#!/usr/bin/env python3
"""Containment check (plan 8.2): every module the firmware's MicroPython
registers must be on the allowlist.

usage: check_modules.py MODULEDEFS_H ALLOWLIST [STAMP]

MODULEDEFS_H is MicroPython's generated genhdr/moduledefs.h (every
MP_REGISTER_MODULE in the build). ALLOWLIST has one module name per line
(# comments). Exits 1, naming the modules, if anything else is registered:
that would be a way for Python code to reach something `t5` doesn't check.
Also fails on the names that must never appear (machine, uctypes, ...), in
case someone adds them to the allowlist by accident.
"""
import re
import sys

NEVER = {"machine", "uctypes", "esp", "esp32", "network", "_thread", "ffi", "pyb", "vfs", "lwip", "usocket",
         "bluetooth", "rp2", "espnow", "ssl", "tls", "onewire", "framebuf"}
# Names allowed only when they are Tab5 OS's own implementation: `socket` must
# be mod_socket.c (t5 handles), never MicroPython's lwIP module.
OURS = {"socket": "py_mod_socket"}


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    with open(argv[1]) as f:
        defs = f.read()
    registered = set(re.findall(r"MP_ROM_QSTR\(MP_QSTR_(\w+)\)", defs))
    with open(argv[2]) as f:
        allowed = {l.split("#", 1)[0].strip() for l in f}
    allowed.discard("")
    bad_list = sorted(allowed & NEVER)
    if bad_list:
        sys.exit("t5_py: modules_allowed.txt lists modules that must never be built: " + ", ".join(bad_list))
    for name, sym in OURS.items():
        m = re.search(r"MP_ROM_QSTR\(MP_QSTR_%s\), MP_ROM_PTR\(&(\w+)\)" % name, defs)
        if m and m.group(1) != sym:
            sys.exit("t5_py: module %s is %s, not Tab5 OS's %s (plan 8.2)" % (name, m.group(1), sym))
    extra = sorted(registered - allowed)
    if extra:
        sys.exit("t5_py: MicroPython registers modules outside the allowlist (plan 8.2): " + ", ".join(extra) +
                 "\n  turn them off in mpconfigport.h, or review and add them to modules_allowed.txt")
    if len(argv) > 3:
        with open(argv[3], "w") as f:
            f.write("\n".join(sorted(registered)) + "\n")
    print("t5_py: %d modules, all on the allowlist" % len(registered))


if __name__ == "__main__":
    main(sys.argv)
