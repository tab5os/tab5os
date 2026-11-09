#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The part of the app ABI that function prototypes don't show.

Apps compile struct layouts, enum values and macro constants into their own
code, so changing one breaks every app already built, with no error anywhere.
This tool compiles the public SDK headers exactly as an app would, reads the
types back from the debug information, and records them with the macros:

    layout.py gen   FIRMWARE_BUILD        write sdk/abi/layout_v1.lock
    layout.py check FIRMWARE_BUILD        fail if anything recorded changed

Only the recorded items are promised: new types, enumerators and macros are
fine. See docs/abi-stability.md. Needs the RISC-V compiler on PATH and
pyelftools (both come with ESP-IDF: `. tools/env.sh`).
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile

try:
    from elftools.elf.elffile import ELFFile
except ImportError:
    sys.exit("layout.py needs pyelftools: run `. tools/env.sh` first")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SDK = os.path.join(ROOT, "sdk")
SYM = os.path.join(SDK, "abi", "api_v1.sym")
LOCK = os.path.join(SDK, "abi", "layout_v1.lock")
CC = "riscv32-esp-elf-gcc"

# Beyond the types named in a prototype: the ones an app reaches through a
# void pointer, an event payload or a callback.
EXTRA_ROOTS = re.compile(
    r"^(t5_app_desc_t|t5_ev_\w+_t|t5_port_\w+_cfg_t|t5_event_handler_t|lv_image_dsc_t|lv_image_header_t|"
    r"stat|tm|timeval|dirent|addrinfo|sockaddr|sockaddr_in|sockaddr_in6|sockaddr_storage|in_addr|fd_set|msghdr|iovec)$")

# Every macro of the firmware headers the SDK headers reach is compiled into
# apps; from the libraries, these: the LVGL configuration and FreeRTOS
# constants an app's code uses.
LIBRARY_MACROS = re.compile(
    r"^(LV_VERSION_\w+|CONFIG_LV_USE_\w+|CONFIG_LV_COLOR_DEPTH\w*|CONFIG_LV_DRAW_BUF_\w+|CONFIG_LV_FONT_\w+|"
    r"CONFIG_FREERTOS_HZ|portTICK_PERIOD_MS|portMAX_DELAY|pdTRUE|pdFALSE|pdPASS|pdFAIL|tskNO_AFFINITY)$")
FIRMWARE_INCLUDES = os.path.realpath(os.path.join(ROOT, "firmware", "components"))
SYSROOT_PY = os.path.join(SDK, "cmake", "sysroot.py")
sys.path.insert(0, os.path.dirname(SYSROOT_PY))
import sysroot  # noqa: E402


def system_headers():
    return sysroot.abi_system_headers()


def app_flags(build, tmp):
    """Stage the SDK sysroot from the build, as the SDK release does, and
    compile against that: a header the staging misses fails here."""
    sr = os.path.join(tmp, "sysroot")
    with open(os.devnull, "w") as null:
        r = subprocess.run([sys.executable, SYSROOT_PY, "stage", build, sr], stdout=null)
    if r.returncode:
        sys.exit("can't stage the SDK headers from the firmware build")
    return sysroot.flags(sr) + ["-I" + os.path.join(SDK, "include")], sr


def public_headers():
    return sorted("t5/" + os.path.basename(h) for h in glob.glob(os.path.join(SDK, "include", "t5", "*.h")))


def compile_all(flags, tmp):
    """Every public header must compile alone; then all together with debug info."""
    bad = 0
    for h in public_headers():
        r = subprocess.run([CC] + flags + ["-fsyntax-only", "-x", "c", "-"], input=f"#include <{h}>\n",
                           capture_output=True, text=True, cwd=ROOT)
        if r.returncode:
            first = next((l for l in r.stderr.splitlines() if "error" in l), r.stderr.strip())
            print(f"{h} doesn't compile on its own: {first}", file=sys.stderr)
            bad += 1
    if bad:
        sys.exit(1)
    src = "".join(f"#include <{h}>\n" for h in public_headers() + system_headers())
    # Linked, because pyelftools can't apply RISC-V relocations to an object file.
    obj = os.path.join(tmp, "layout.so")
    subprocess.run([CC] + flags + ["-g", "-O0", "-fno-eliminate-unused-debug-types", "-w", "-fPIC", "-shared", "-nostdlib",
                                   "-Wl,--no-warn-rwx-segments", "-x", "c", "-", "-o", obj],
                   input=src, text=True, check=True, cwd=ROOT)
    macros = subprocess.run([CC] + flags + ["-E", "-dD", "-x", "c", "-"], input=src, capture_output=True, text=True,
                            check=True, cwd=ROOT).stdout
    return obj, macros


class Dwarf:
    def __init__(self, path):
        self.fh = open(path, "rb")
        elf = ELFFile(self.fh)
        self.dies = {}
        self.named = {}
        for cu in elf.get_dwarf_info().iter_CUs():
            for die in cu.iter_DIEs():
                self.dies[die.offset] = die
                if die.tag in ("DW_TAG_structure_type", "DW_TAG_union_type", "DW_TAG_enumeration_type", "DW_TAG_typedef"):
                    n = self.name(die)
                    if n and not ("DW_AT_declaration" in die.attributes and n in self.named):
                        # A complete definition beats a forward declaration.
                        if n not in self.named or "DW_AT_declaration" in self.named[n].attributes:
                            self.named[n] = die

    @staticmethod
    def name(die):
        a = die.attributes.get("DW_AT_name")
        return a.value.decode() if a else None

    def target(self, die):
        a = die.attributes.get("DW_AT_type")
        return self.dies[a.value + die.cu.cu_offset] if a else None

    @staticmethod
    def attr(die, name, default=None):
        a = die.attributes.get(name)
        return a.value if a else default

    def size(self, die):
        while die is not None:
            s = self.attr(die, "DW_AT_byte_size")
            if s is not None:
                return s
            if die.tag == "DW_TAG_array_type":
                n = 1
                for c in die.iter_children():
                    n *= self.attr(c, "DW_AT_upper_bound", -1) + 1
                return n * (self.size(self.target(die)) or 0)
            if die.tag == "DW_TAG_pointer_type":
                return 4
            die = self.target(die)
        return 0

    def tname(self, die):
        """A stable text name for a type, without following named aggregates."""
        if die is None:
            return "void"
        t, n = die.tag, self.name(die)
        if t in ("DW_TAG_base_type", "DW_TAG_typedef"):
            return n
        if t == "DW_TAG_structure_type":
            return f"struct {n}" if n else "struct{" + self.body(die) + "}"
        if t == "DW_TAG_union_type":
            return f"union {n}" if n else "union{" + self.body(die) + "}"
        if t == "DW_TAG_enumeration_type":
            return f"enum {n}" if n else "enum{" + self.enum_key(die) + "}"
        if t == "DW_TAG_pointer_type":
            return self.tname(self.target(die)) + " *"
        if t == "DW_TAG_const_type":
            return "const " + self.tname(self.target(die))
        if t == "DW_TAG_volatile_type":
            return "volatile " + self.tname(self.target(die))
        if t == "DW_TAG_restrict_type":
            return self.tname(self.target(die))
        if t == "DW_TAG_array_type":
            dims = "".join(f"[{self.attr(c, 'DW_AT_upper_bound', -1) + 1}]" for c in die.iter_children())
            return self.tname(self.target(die)) + dims
        if t == "DW_TAG_subroutine_type":
            args = ", ".join(self.tname(self.target(c)) for c in die.iter_children()
                             if c.tag == "DW_TAG_formal_parameter")
            return f"fn({args}) -> {self.tname(self.target(die))}"
        return t

    def body(self, die):
        parts = []
        for c in die.iter_children():
            if c.tag != "DW_TAG_member":
                continue
            off = self.attr(c, "DW_AT_data_member_location", 0)
            bits = self.attr(c, "DW_AT_bit_size")
            pos = f"+{off}" if bits is None else f"bit{self.attr(c, 'DW_AT_data_bit_offset', 0)}:{bits}"
            parts.append(f"{self.name(c) or ''}@{pos}:{self.tname(self.target(c))}")
        return "; ".join(parts)

    def enum_key(self, die):
        first = next((self.name(c) for c in die.iter_children() if c.tag == "DW_TAG_enumerator"), "?")
        return first


def is_complete(die):
    return "DW_AT_declaration" not in die.attributes


def collect_types(d):
    with open(SYM) as fh:
        text = "\n".join(l.split("|", 2)[2] for l in fh if "|" in l and not l.startswith("#"))
    words = set(re.findall(r"[A-Za-z_]\w*", text))
    todo = [die for name, die in d.named.items() if name in words or EXTRA_ROOTS.match(name)]
    seen, items = set(), {}

    def visit(die):
        while die is not None and die.tag in ("DW_TAG_const_type", "DW_TAG_volatile_type", "DW_TAG_restrict_type",
                                              "DW_TAG_pointer_type", "DW_TAG_array_type"):
            die = d.target(die)
        if die is not None:
            todo.append(die)

    while todo:
        die = todo.pop()
        if die.offset in seen:
            continue
        seen.add(die.offset)
        name = d.name(die)
        if die.tag == "DW_TAG_typedef":
            visit(d.target(die))
            tgt = d.target(die)
            if tgt is not None and not (tgt.tag in ("DW_TAG_structure_type", "DW_TAG_union_type") and not is_complete(tgt)):
                items[f"typedef {name}"] = d.tname(tgt)
        elif die.tag in ("DW_TAG_structure_type", "DW_TAG_union_type") and is_complete(die):
            kind = "struct" if die.tag == "DW_TAG_structure_type" else "union"
            if name:
                items[f"{kind} {name}"] = f"size={d.size(die)} {d.body(die)}"
            for c in die.iter_children():
                if c.tag == "DW_TAG_member":
                    visit(d.target(c))
        elif die.tag == "DW_TAG_enumeration_type":
            label = name or d.enum_key(die)
            items[f"enum {label}"] = f"size={d.size(die)}"
            for c in die.iter_children():
                if c.tag == "DW_TAG_enumerator":
                    items[f"enumerator {d.name(c)}"] = str(d.attr(c, "DW_AT_const_value"))
        elif die.tag == "DW_TAG_subroutine_type":
            visit(d.target(die))
            for c in die.iter_children():
                if c.tag == "DW_TAG_formal_parameter":
                    visit(d.target(c))
    return items


def collect_macros(text):
    """From -dD output: the definitions in force at the end, each with the
    file that made it (the line markers say which)."""
    defs, where = {}, None
    for line in text.splitlines():
        mark = re.match(r'# \d+ "([^"]+)"', line)
        if mark:
            where = mark.group(1)
            continue
        m = re.match(r"#define (\w+)(\([^)]*\))? ?(.*)$", line)
        if m:
            defs[m.group(1)] = (m.group(2) or "", m.group(3), where)
        elif line.startswith("#undef "):
            defs.pop(line.split()[1], None)
    items = {}
    for name, (args, value, path) in defs.items():
        ours = path and "/sysroot/os/" in os.path.realpath(path)
        if value.strip() and (ours or LIBRARY_MACROS.match(name)):
            items[f"macro {name}{args}"] = " ".join(value.split())
    return items


def snapshot(build):
    with tempfile.TemporaryDirectory() as tmp:
        flags, _ = app_flags(build, tmp)
        obj, macros = compile_all(flags, tmp)
        d = Dwarf(obj)
        items = collect_types(d)
        d.fh.close()
    items.update(collect_macros(macros))
    return items


def read_lock():
    if not os.path.exists(LOCK):
        return None
    items = {}
    with open(LOCK) as fh:
        for line in fh:
            if line.startswith("#") or not line.strip():
                continue
            key, _, value = line.rstrip("\n").partition(" = ")
            items[key] = value
    return items


def cmd_gen(a):
    items = snapshot(a.build)
    with open(LOCK, "w") as fh:
        fh.write("# Struct layouts, enum values and macros that apps compile in, API level 1.\n"
                 "# Do not edit: tools/abi/layout.py gen. See docs/abi-stability.md.\n")
        for k in sorted(items):
            fh.write(f"{k} = {items[k]}\n")
    print(f"{LOCK}: {len(items)} items")


def cmd_check(a):
    locked = read_lock()
    if locked is None:
        sys.exit(f"{LOCK}: missing; run layout.py gen")
    now = snapshot(a.build)
    errs = 0
    for k, v in sorted(locked.items()):
        if k not in now:
            print(f"removed from the ABI: {k}\n  was: {v}", file=sys.stderr)
            errs += 1
        elif now[k] != v:
            print(f"changed in the ABI: {k}\n  was: {v}\n  now: {now[k]}", file=sys.stderr)
            errs += 1
    if errs:
        sys.exit(f"{errs} ABI layout changes: apps already built would break (docs/abi-stability.md)")
    print(f"layout: {len(locked)} items match, {len(now) - len(locked)} new")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("cmd", choices=["gen", "check"])
    p.add_argument("build", nargs="?", default=os.path.join(ROOT, "firmware", "build"), help="firmware build directory")
    a = p.parse_args()
    {"gen": cmd_gen, "check": cmd_check}[a.cmd](a)


if __name__ == "__main__":
    main()
