#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Print the compile flags an app needs, one per line: the -I/-D/-m flags the
firmware used for its ABI table (t5_app/abi_table.c), which includes every
header behind the exported API. Same headers, same LVGL configuration, same
ABI."""
import json
import shlex
import sys

build = sys.argv[1]
try:
    db = json.load(open(f"{build}/compile_commands.json"))
except OSError:
    sys.exit(f"no {build}/compile_commands.json: build the firmware first (t5 build)")
cmd = next((e for e in db if e["file"].endswith("abi_table.c")), None)
if not cmd:
    sys.exit(f"{build} has no t5_app/abi_table.c: is it a Tab5 OS firmware build?")
args = cmd.get("arguments") or shlex.split(cmd["command"])
keep = []
i = 0
while i < len(args):
    a = args[i]
    if a.startswith("@"):            # IDF's toolchain response file: -march, -mabi, picolibc specs
        for line in open(a[1:]):
            keep += shlex.split(line)
        i += 1
        continue
    if a in ("-I", "-D", "-include"):
        keep += [a + args[i + 1]] if a != "-include" else [a, args[i + 1]]
        i += 2
        continue
    if a.startswith(("-I", "-D", "-m", "-std=")) or a in ("-fno-builtin-memcpy",):
        keep.append(a)
    i += 1
print("\n".join(keep))
