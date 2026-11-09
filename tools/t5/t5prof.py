#!/usr/bin/env python3
"""Summarise a `prof dump` from the Tab5 console by function.

On the device:  prof start 3000 1000   (sample core 1 for 3 s after 1 s)
                ui gate scroll 5        (or whatever load to profile)
                prof dump 800
Save the console output to a file, then:

    tools/t5/t5prof.py firmware/build/tab5os.elf dump.log [--lines]

--lines also breaks the top functions down by source line.
"""
import collections
import re
import subprocess
import sys


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    elf, log_path = sys.argv[1], sys.argv[2]
    log = open(log_path, errors="replace").read()
    hits = [(int(a, 16), int(n)) for a, n in re.findall(r"^P ([0-9a-f]{8}) (\d+)", log, re.M)]
    m = re.search(r"prof samples (\d+)", log)
    if not hits or not m:
        sys.exit("no `prof dump` output in " + log_path)
    total = int(m.group(1))
    out = subprocess.run(["riscv32-esp-elf-addr2line", "-f", "-e", elf] + ["0x%x" % a for a, _ in hits],
                         capture_output=True, text=True, check=True).stdout.split("\n")
    funcs, lines = collections.Counter(), collections.Counter()
    for i, (_, n) in enumerate(hits):
        f, where = out[2 * i], out[2 * i + 1].split("/")[-1]
        funcs[f] += n
        lines[(f, where)] += n
    print("%d samples, %d in the dump" % (total, sum(n for _, n in hits)))
    for f, n in funcs.most_common(30):
        print("%5.1f%%  %s" % (100.0 * n / total, f))
    if "--lines" in sys.argv:
        print()
        for (f, where), n in lines.most_common(30):
            print("%5.1f%%  %s  %s" % (100.0 * n / total, f, where))


if __name__ == "__main__":
    main()
