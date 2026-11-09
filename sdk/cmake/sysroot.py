#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Stage the headers and compile flags native apps build with.

An app sees exactly what the OS was built with: the flags of the firmware's
ABI table (t5_abi/abi_table.c, which includes every header behind the
exported API) and the headers the public SDK headers reach. This copies
those headers out of the firmware build, ESP-IDF and the managed
components into one directory, keeping each file's path relative to where
it came from, so every #include, #include_next and relative "../x.h"
resolves to the same file as in the firmware build. Headers that belong to
the compiler itself (GCC's and picolibc's, in the toolchain) aren't copied:
the RISC-V toolchain that comes with ESP-IDF supplies them.

    sysroot.py stage FIRMWARE_BUILD OUT     write OUT/ (headers, flags.cmake, flags.txt)

OUT/flags.cmake sets T5_APP_FLAGS with paths relative to OUT, so OUT can be
moved anywhere (the SDK release ships it as sdk/sysroot/). Nothing in it
names the machine it was made on.
"""
import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys

SDK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(SDK)
CC = "riscv32-esp-elf-gcc"
# The C library headers, besides the ones in abi_includes.h, that an app may
# include. Any that ESP-IDF overrides (its newlib/picolibc, pthread, vfs and
# lwIP ports) are staged, so they resolve as in the firmware.
EXTRA_SYSTEM = ("assert.h", "ctype.h", "dirent.h", "errno.h", "fcntl.h", "float.h", "inttypes.h", "limits.h",
                "locale.h", "math.h", "poll.h", "pthread.h", "sched.h", "setjmp.h", "signal.h", "stdalign.h",
                "stdarg.h", "stdatomic.h", "stdbool.h", "stddef.h", "stdint.h", "stdio.h", "stdlib.h", "string.h",
                "strings.h", "termios.h", "time.h", "unistd.h", "wchar.h", "sys/fcntl.h", "sys/ioctl.h",
                "sys/lock.h", "sys/param.h", "sys/poll.h", "sys/queue.h", "sys/reent.h", "sys/select.h",
                "sys/socket.h", "sys/stat.h", "sys/termios.h", "sys/time.h", "sys/types.h", "sys/uio.h",
                "sys/un.h", "sys/unistd.h", "netdb.h", "arpa/inet.h", "netinet/in.h", "netinet/tcp.h",
                "net/if.h", "ifaddrs.h")
ABI_INCLUDES = os.path.join(ROOT, "firmware", "components", "t5_abi", "abi_includes.h")


def public_headers():
    d = os.path.join(SDK, "include", "t5")
    return sorted("t5/" + f for f in os.listdir(d) if f.endswith(".h"))


def abi_system_headers():
    """The C library and lwIP headers behind exported functions (abi_includes.h)."""
    with open(ABI_INCLUDES) as fh:
        names = re.findall(r'^#include [<"]([^>"]+)[>"]', fh.read(), re.M)
    return [n for n in names if "/" not in n or n.startswith(("sys/", "lwip/"))]


def firmware_command(build):
    try:
        db = json.load(open(os.path.join(build, "compile_commands.json")))
    except OSError:
        sys.exit(f"no {build}/compile_commands.json: build the firmware first (t5 build)")
    cmd = next((e for e in db if e["file"].endswith("abi_table.c")), None)
    if not cmd:
        sys.exit(f"{build} has no t5_abi/abi_table.c: is it a Tab5 OS firmware build?")
    return cmd.get("arguments") or shlex.split(cmd["command"])


def split_flags(args):
    """(include dirs in search order, other flags) an app needs."""
    incs, other = [], []
    i = 0
    while i < len(args):
        a = args[i]
        if a.startswith("@"):            # IDF's toolchain response file: -march, -mabi, picolibc specs
            with open(a[1:]) as fh:
                for line in fh:
                    other += shlex.split(line)
            i += 1
            continue
        if a in ("-I", "-D"):
            a, i = a + args[i + 1], i + 1
        if a.startswith("-I"):
            incs.append(os.path.realpath(a[2:]))
        elif a.startswith(("-D", "-m", "-std=", "-specs=", "--specs=")) or a == "-fno-builtin-memcpy":
            other.append(a)
        i += 1
    return incs, other


def toolchain_root():
    gcc = shutil.which(CC)
    if not gcc:
        sys.exit(f"{CC} isn't on PATH: run `. tools/env.sh`")
    # .../riscv32-esp-elf/bin/riscv32-esp-elf-gcc -> .../riscv32-esp-elf
    return os.path.realpath(os.path.join(os.path.dirname(os.path.realpath(gcc)), ".."))


def toolchain_version():
    """The toolchain's release, e.g. esp-15.2.0_20251204 (its install directory)."""
    return os.path.basename(os.path.dirname(toolchain_root()))


def idf_root(paths):
    env = os.environ.get("IDF_PATH")
    if env:
        return os.path.realpath(env)
    for p in paths:
        m = re.match(r"(.*?)/components/", p)
        if m and os.path.exists(os.path.join(m.group(1), "tools", "idf.py")):
            return m.group(1)
    return None


def bases(build, incs):
    """Where staged files go: (source prefix, staged name), longest first."""
    fw = os.path.realpath(os.path.join(ROOT, "firmware"))
    out = [(os.path.realpath(build), "build"),
           (os.path.join(fw, "managed_components"), "managed"),
           (os.path.join(fw, "components"), "os"),
           (os.path.realpath(SDK), "sdk")]
    idf = idf_root(incs)
    if idf:
        out.append((idf, "idf"))
    return sorted(out, key=lambda b: -len(b[0]))


def relocate(path, base_list):
    for src, name in base_list:
        if path == src or path.startswith(src + os.sep):
            return os.path.join(name, os.path.relpath(path, src)) if path != src else name
    return None


def probe_source(flags):
    """Every header an app may include: the public SDK headers, the library
    headers behind exported functions, the custom-widget class header, and
    whichever C library headers compile."""
    names = public_headers() + abi_system_headers()
    for h in EXTRA_SYSTEM:
        if h in names:
            continue
        r = subprocess.run([CC] + flags + ["-fsyntax-only", "-x", "c", "-"], input=f"#include <{h}>\n",
                           capture_output=True, text=True)
        if r.returncode == 0:
            names.append(h)
    return "".join(f"#include <{h}>\n" for h in names)


LICENSE_RE = re.compile(r"^(LICEN[SC]E|COPYING)(\.\w+)?$", re.I)


def copy_license(src, rel, out):
    """The nearest licence file above a staged header, in its source tree,
    goes along with it (ESP-IDF, lwIP, FreeRTOS, LVGL)."""
    d, r = os.path.dirname(src), os.path.dirname(rel)
    while r:
        found = [f for f in os.listdir(d) if LICENSE_RE.match(f)] if os.path.isdir(d) else []
        if found:
            for f in found:
                dst = os.path.join(out, r, f)
                if not os.path.exists(dst):
                    shutil.copyfile(os.path.join(d, f), dst)
            return
        d, r = os.path.dirname(d), os.path.dirname(r)


def stage(build, out):
    incs, other = split_flags(firmware_command(build))
    base_list = bases(build, incs)
    sdk_inc = os.path.join(SDK, "include")
    flags = other + ["-I" + d for d in incs] + ["-I" + sdk_inc]
    src = probe_source(flags)
    r = subprocess.run([CC] + flags + ["-M", "-x", "c", "-"], input=src, capture_output=True, text=True)
    if r.returncode:
        sys.exit("the SDK headers don't compile:\n" + r.stderr)
    deps = [os.path.realpath(p) for p in r.stdout.replace("\\\n", " ").split()[1:] if p != "-"]
    tc = toolchain_root()
    if os.path.isdir(out):
        shutil.rmtree(out)
    staged = 0
    for p in sorted(set(deps)):
        if p.startswith(tc + os.sep):
            continue                     # the compiler's own headers
        if p.startswith(sdk_inc + os.sep):
            continue                     # the public headers: sdk/include, always on the path
        rel = relocate(p, base_list)
        if not rel:
            sys.exit(f"{p}: an SDK header reaches this file, which isn't under ESP-IDF, the firmware or its build")
        dst = os.path.join(out, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(p, dst)
        staged += 1
        copy_license(p, rel, out)
    # Include dirs in the firmware's order; ones nothing was found in are dropped.
    dirs = []
    for d in incs:
        rel = relocate(d, base_list)
        if rel and rel.split(os.sep)[0] != "sdk" and os.path.isdir(os.path.join(out, rel)):
            dirs.append(rel)
        elif not rel and any(p.startswith(d + os.sep) for p in deps):
            sys.exit(f"{d}: include directory outside ESP-IDF, the firmware and its build")
    # The picolibc specs file IDF generates is relative to the compiler (%R).
    final = []
    for f in other:
        m = re.match(r"(-?-specs=)(.*)", f)
        if m and os.path.isabs(m.group(2)):
            shutil.copyfile(m.group(2), os.path.join(out, os.path.basename(m.group(2))))
            final.append(("rel", m.group(1), os.path.basename(m.group(2))))
        else:
            final.append(("abs", f, None))
    write_flags(out, final, dirs)
    leaks = [f for f in other if f.startswith(("-D", "-m", "-std")) and os.sep + "Users" + os.sep in f]
    if leaks:
        sys.exit(f"flags name a local path: {leaks}")
    return staged, len(dirs)


def write_flags(out, final, dirs):
    cm = ["# Generated by sdk/cmake/sysroot.py: the compile flags of the firmware's ABI table,",
          "# with include paths relative to this directory. Do not edit.",
          f'set(T5_TOOLCHAIN_VERSION "{toolchain_version()}")',
          "set(T5_APP_FLAGS"]
    txt = []
    for kind, a, b in final:
        if kind == "rel":
            cm.append(f'    "{a}${{T5_SYSROOT}}/{b}"')
            txt.append(f"{a}{{SYSROOT}}/{b}")
        else:
            cm.append(f"    [=[{a}]=]")      # a bracket argument: quotes kept as they are
            txt.append(a)
    for d in dirs:
        cm.append(f'    "-I${{T5_SYSROOT}}/{d}"')
        txt.append(f"-I{{SYSROOT}}/{d}")
    cm.append(")")
    with open(os.path.join(out, "flags.cmake"), "w") as fh:
        fh.write("\n".join(cm) + "\n")
    with open(os.path.join(out, "flags.txt"), "w") as fh:
        fh.write("\n".join(txt) + "\n")


def flags(sysroot):
    """The flags in OUT/flags.txt, with OUT filled in (for Python tools)."""
    with open(os.path.join(sysroot, "flags.txt")) as fh:
        return [l.replace("{SYSROOT}", sysroot) for l in fh.read().split("\n") if l]


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("stage")
    s.add_argument("build")
    s.add_argument("out")
    a = p.parse_args()
    n, d = stage(os.path.realpath(a.build), os.path.realpath(a.out))
    print(f"{a.out}: {n} headers in {d} include directories")


if __name__ == "__main__":
    main()
