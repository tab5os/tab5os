#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The part of the app ABI that function prototypes don't show.

Apps compile struct layouts, enum values and macro constants into their own
code, so changing one breaks every app already built, with no error anywhere.
This tool compiles the public SDK headers exactly as an app would (against
the headers sdk/cmake/sysroot.py stages from the build, as the SDK release
ships them), reads the types back from the debug information, evaluates the
library macros with the compiler, and records them all:

    layout.py gen   FIRMWARE_BUILD        write sdk/abi/layout_v1.lock
    layout.py check FIRMWARE_BUILD        fail if anything recorded changed

Only the recorded items are promised: new types, enumerators and macros are
fine. See docs/abi-stability.md. Needs the RISC-V compiler on PATH and
pyelftools (both come with ESP-IDF: `. tools/env.sh`).
"""
import argparse
import struct
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
# void pointer, an event payload or a callback. lv_obj_class_t: custom
# widgets fill one in (lv_obj_class_create_obj is exported).
EXTRA_ROOTS = re.compile(
    r"^(t5_app_desc_t|t5_ev_\w+_t|t5_port_\w+_cfg_t|t5_event_handler_t|lv_image_dsc_t|lv_image_header_t|"
    r"lv_obj_class_t|_lv_obj_class_t|"
    r"stat|tm|timeval|dirent|addrinfo|sockaddr|sockaddr_in|sockaddr_in6|sockaddr_storage|in_addr|fd_set|msghdr|iovec)$")

# Macros. Every macro of the OS's own headers that the SDK headers reach is
# compiled into apps, and recorded as written. From the sdkconfig, the
# settings below. From the libraries apps call (the ext symbols in
# api_v1.sym), every macro their headers define that an app could compile in
# as a value: the compiler evaluates each one (as a constant, a string, or
# a function-like macro at sample arguments) and the value and type are
# recorded. Names reserved for the implementation (a leading underscore) are
# left out. LIBRARY_TEXT_MACROS were locked as text before values were
# probed; they stay that way so their lock entries don't change.
CONFIG_MACROS = re.compile(
    r"^(CONFIG_LV_USE_\w+|CONFIG_LV_COLOR_DEPTH\w*|CONFIG_LV_DRAW_BUF_\w+|CONFIG_LV_FONT_\w+|CONFIG_FREERTOS_HZ)$")
LIBRARY_TEXT_MACROS = re.compile(
    r"^(LV_VERSION_\w+|portTICK_PERIOD_MS|portMAX_DELAY|pdTRUE|pdFALSE|pdPASS|pdFAIL|tskNO_AFFINITY)$")
# Where each library's headers are, as staged in the SDK sysroot
# (sdk/cmake/sysroot.py), plus the compiler's own headers (picolibc, GCC's
# <stdint.h>, <limits.h>...), which count as the C library.
LIBRARIES = (("libc", "idf/components/esp_libc/"), ("lwip", "idf/components/lwip/"),
             ("freertos", "idf/components/freertos/"), ("lvgl", "managed/lvgl__lvgl/"))
OURS = ("os/",)
# The libraries' tuning headers. Their options reach apps through values that
# are recorded (FD_SETSIZE, pdMS_TO_TICKS(x), LV_DPX(x), the CONFIG_LV_*
# settings), so tuning lwIP or FreeRTOS doesn't trip the check by itself.
CONFIG_HEADERS = re.compile(r"(/lv_conf\w*|/lwipopts|/lwip/opt|/FreeRTOSConfig\w*)\.h$")
# Values that depend on where the macro is expanded, not on the library.
WHERE_MACROS = re.compile(r"\b(__FILE__|__FILE_NAME__|__LINE__|__COUNTER__|__DATE__|__TIME__|__func__)\b")
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
    src = all_headers()
    obj = link(flags, src, os.path.join(tmp, "layout.so"))
    macros = subprocess.run([CC] + flags + ["-E", "-dD", "-x", "c", "-"], input=src, capture_output=True, text=True,
                            check=True, cwd=ROOT).stdout
    return obj, macros


def all_headers():
    return "".join(f"#include <{h}>\n" for h in public_headers() + system_headers())


def link(flags, src, out, check=True):
    """Compile src with debug info into a shared object (linked, because
    pyelftools can't apply RISC-V relocations to an object file)."""
    path = out[:-3] + ".c"
    with open(path, "w") as fh:
        fh.write(src)
    r = subprocess.run([CC] + flags + ["-g", "-O0", "-fno-eliminate-unused-debug-types", "-w", "-fmax-errors=0",
                                       "-fPIC", "-shared", "-nostdlib", "-Wl,--no-warn-rwx-segments", path, "-o", out],
                       capture_output=True, text=True, cwd=ROOT)
    if check and r.returncode:
        sys.exit(r.stderr)
    return out if not r.returncode else r.stderr


class Origin:
    """Which library a header belongs to, by where it is in the staged
    sysroot (or the compiler's own include directories)."""

    def __init__(self, sr):
        self.sr = os.path.realpath(sr) + os.sep
        self.tc = sysroot.toolchain_root() + os.sep

    def of(self, path):
        path = os.path.realpath(path)
        if path.startswith(self.tc):
            return "libc"
        if not path.startswith(self.sr):
            return None
        rel = path[len(self.sr):]
        if rel.startswith(OURS):
            return "os"
        return next((name for name, prefix in LIBRARIES if rel.startswith(prefix)), None)


class Dwarf:
    def __init__(self, path):
        self.fh = open(path, "rb")
        self.elf = elf = ELFFile(self.fh)
        self.dies = {}
        self.named = {}
        self.enums = []
        self.files = {}
        dwarf = elf.get_dwarf_info()
        for cu in dwarf.iter_CUs():
            self.files[cu.cu_offset] = self.file_table(dwarf, cu)
            for die in cu.iter_DIEs():
                if die.tag == "DW_TAG_enumeration_type" and die.get_parent() is not None \
                        and die.get_parent().tag == "DW_TAG_compile_unit":
                    self.enums.append(die)
                self.dies[die.offset] = die
                if die.tag in ("DW_TAG_structure_type", "DW_TAG_union_type", "DW_TAG_enumeration_type", "DW_TAG_typedef"):
                    n = self.name(die)
                    if n and not ("DW_AT_declaration" in die.attributes and n in self.named):
                        # A complete definition beats a forward declaration.
                        if n not in self.named or "DW_AT_declaration" in self.named[n].attributes:
                            self.named[n] = die

    @staticmethod
    def file_table(dwarf, cu):
        """DW_AT_decl_file index -> path, for this CU's line program."""
        lp = dwarf.line_program_for_CU(cu)
        if lp is None:
            return {}
        v5 = lp.header.version >= 5
        dirs = [d.decode() if isinstance(d, bytes) else d for d in lp.header.include_directory]
        comp_dir = cu.get_top_DIE().attributes.get("DW_AT_comp_dir")
        comp_dir = comp_dir.value.decode() if comp_dir else ""
        out = {}
        for i, fe in enumerate(lp.header.file_entry):
            name = fe.name.decode() if isinstance(fe.name, bytes) else fe.name
            if v5:
                d = dirs[fe.dir_index]
            else:
                d = dirs[fe.dir_index - 1] if fe.dir_index else comp_dir
            out[i if v5 else i + 1] = os.path.join(comp_dir, d, name)
        return out

    def decl_file(self, die):
        a = die.attributes.get("DW_AT_decl_file")
        return self.files[die.cu.cu_offset].get(a.value) if a else None

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
            add_enum(d, die, items)
        elif die.tag == "DW_TAG_subroutine_type":
            visit(d.target(die))
            for c in die.iter_children():
                if c.tag == "DW_TAG_formal_parameter":
                    visit(d.target(c))
    return items


def add_enum(d, die, items):
    label = d.name(die) or d.enum_key(die)
    items[f"enum {label}"] = f"size={d.size(die)}"
    for c in die.iter_children():
        if c.tag == "DW_TAG_enumerator":
            items[f"enumerator {d.name(c)}"] = str(d.attr(c, "DW_AT_const_value"))


def collect_enums(d, origin):
    """Every enumeration the OS's headers and the libraries' headers define,
    reachable from a prototype or not: an app can name any enumerator and
    compile its value in (LV_OPA_COVER, LV_ALIGN_CENTER...)."""
    items = {}
    for die in d.enums:
        path = d.decl_file(die)
        if path and origin.of(path) and is_complete(die):
            add_enum(d, die, items)
    return items


def collect_macros(text, origin):
    """From -dD output: the definitions in force at the end, each with the
    file that made it (the line markers say which). Returns (macros recorded
    as text, library macros to probe)."""
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
    items, probe = {}, []
    for name, (args, value, path) in defs.items():
        if not value.strip():
            continue
        lib = origin.of(path) if path else None
        if lib == "os" or CONFIG_MACROS.match(name) or LIBRARY_TEXT_MACROS.match(name):
            items[f"macro {name}{args}"] = " ".join(value.split())
        elif lib and not name.startswith("_") and "..." not in args and not CONFIG_HEADERS.search(path) \
                and not WHERE_MACROS.search(value):
            probe.append((name, args))
    return items, probe


# Function-like macros are probed at these arguments (LV_PCT(x) -> LV_PCT(1), LV_PCT(100)).
SAMPLE_ARGS = ((1,), (100,))
PROBE_TYPES = """
struct t5_probe { unsigned long long v; double d; unsigned int size; unsigned char sign, flt; };
#define T5_NUM(N, E) __attribute__((used)) static const struct t5_probe N = { (unsigned long long)(E), (double)(E), \\
    sizeof(E), (__typeof__(E))-1 < 0, _Generic((E), float: 1, double: 1, long double: 1, default: 0) };
#define T5_STR(N, E) __attribute__((used)) static const char N[] = E;
"""


def probe_exprs(name, args):
    """[(label, expression)]: the macro itself, or a call at sample arguments
    (one argument: SAMPLE_ARGS; more: 1, 2, 3...)."""
    if not args:
        return [("", name)]
    n = len([a for a in args[1:-1].split(",") if a.strip()])
    samples = list(SAMPLE_ARGS) if n == 1 else [tuple(range(1, n + 1))]
    out = []
    for s in samples:
        call = "(" + ", ".join(map(str, s)) + ")"
        out.append((call, name + call))
    return out


def probe_values(flags, tmp, cands):
    """Compile every candidate as a constant; the ones that aren't drop out.
    Returns {key: recorded value}."""
    exprs = []
    for name, args in cands:
        for label, e in probe_exprs(name, args):
            exprs.append((name, args, label, e))
    found = {}
    for kind in ("num", "str"):
        live = [i for i in range(len(exprs)) if i not in found]
        for _ in range(12):                      # each pass drops at least one line
            lines = [f"T5_{'NUM' if kind == 'num' else 'STR'}(t5_p{i}, {exprs[i][3]})" for i in live]
            head = all_headers() + PROBE_TYPES
            base = head.count("\n") + 1
            out = link(flags, head + "\n".join(lines) + "\n", os.path.join(tmp, f"probe_{kind}.so"), check=False)
            if not out.endswith(".so"):
                bad = {int(m) - base for m in re.findall(r"probe_%s\.c:(\d+):" % kind, out)}
                bad = {live[b] for b in bad if 0 <= b < len(live)}
                if not bad:
                    sys.exit("macro probe failed outside the probes:\n" + out[:2000])
                live = [i for i in live if i not in bad]
                continue
            found.update(read_probes(out, kind, live))
            break
        else:
            sys.exit("macro probe didn't converge")
    items = {}
    for i, (name, args, label, _) in enumerate(exprs):
        if i in found:
            key = f"macro {name}{args}"
            val = f"{label}={found[i]}" if label else found[i]
            items[key] = items[key] + "; " + val if key in items else val
    return items


def read_probes(path, kind, live):
    with open(path, "rb") as fh:
        elf = ELFFile(fh)
        syms = {s.name: s for s in elf.get_section_by_name(".symtab").iter_symbols() if s.name.startswith("t5_p")}
        out = {}
        for i in live:
            s = syms.get(f"t5_p{i}")
            if s is None:
                continue
            sec = elf.get_section(s["st_shndx"])
            off = s["st_value"] - sec["sh_addr"]
            data = sec.data()[off:off + s["st_size"]]
            if kind == "str":
                if b"probe_" in data:
                    continue             # expands to the file name through another macro
                text ="".join(chr(b) if 32 <= b < 127 and b not in (34, 92) else f"\\x{b:02x}" for b in data[:-1])
                out[i] = f'"{text}" str'
                continue
            v, dbl, size, sign, flt = struct.unpack("<QdIBB", data[:22])
            if flt:
                out[i] = f"{dbl!r} float{size}"
            else:
                if sign and v >= 1 << 63:
                    v -= 1 << 64
                out[i] = f"{v} {'int' if sign else 'uint'}{size}"
        return out


def snapshot(build):
    with tempfile.TemporaryDirectory() as tmp:
        flags, sr = app_flags(build, tmp)
        origin = Origin(sr)
        obj, macros = compile_all(flags, tmp)
        d = Dwarf(obj)
        items = collect_enums(d, origin)
        items.update(collect_types(d))
        d.fh.close()
        text, probe = collect_macros(macros, origin)
        items.update(text)
        items.update(probe_values(flags, tmp, probe))
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
