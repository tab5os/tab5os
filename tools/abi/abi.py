#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Tab5 OS app ABI tool (plan 5.5, 5.6).

sdk/abi/api_v<N>.sym lists every symbol a native app may import:

    name | flags | prototype;

flags (comma-separated, may be empty):
    perm=<group>   needs the manifest permission group (else a stub that
                   returns T5_ERR_DENIED / NULL / false / 0)
    wrap=<symbol>  bind to this OS function instead (tracking wrappers)
    ext            the prototype is maintained by hand (libc, FreeRTOS,
                   lwIP): `update` leaves it alone
    data           an object, not a function (its "prototype" is the extern
                   declaration)
    since=<N>      first API level that has it, when later than the file's
Lines starting with '#' are comments.

sdk/abi/api_v<N>.lock is the contract for level N (docs/abi-stability.md): what
apps may already depend on. Its first line says "candidate" or "released". A
locked symbol can't be removed or change prototype, kind or permission. Once
released, a new symbol also needs since=<N+1>.

Commands:
    gen SYM OUT              write abi_table.c for the firmware build
    update SYM HEADERDIR...  fill prototypes of non-ext symbols from headers
    add-lvgl SYM LIB HDRDIR  add every public lv_* function compiled into LIB
    check SYM [--base REF]   well-formed, sorted, unique, and true to the lock;
                             with --base, also compare against REF's file
    freeze SYM [--release]   write the lock from SYM; a released lock can only
                             be rewritten with --force
"""
import argparse
import os
import re
import subprocess
import sys

PERMS = {
    "camera": "T5_PERM_CAMERA", "mic": "T5_PERM_MIC", "net": "T5_PERM_NET", "ble": "T5_PERM_BLE",
    "ports": "T5_PERM_PORTS", "usb": "T5_PERM_USB", "storage": "T5_PERM_STORAGE",
    "background_wake": "T5_PERM_BACKGROUND_WAKE", "system": "T5_PERM_SYSTEM",
}


class Sym:
    def __init__(self, name, flags, proto, line):
        self.name, self.flags, self.proto, self.line = name, flags, proto, line

    @property
    def perm(self):
        return self.flags.get("perm")

    @property
    def wrap(self):
        return self.flags.get("wrap")

    @property
    def ext(self):
        return "ext" in self.flags

    @property
    def data(self):
        return "data" in self.flags

    @property
    def since(self):
        return int(self.flags["since"]) if "since" in self.flags else 0

    def format(self):
        f = []
        for k in ("perm", "wrap"):
            if k in self.flags:
                f.append(f"{k}={self.flags[k]}")
        if self.ext:
            f.append("ext")
        if self.data:
            f.append("data")
        if self.since:
            f.append(f"since={self.since}")
        return f"{self.name} | {','.join(f)} | {self.proto}"


def parse_sym(path):
    syms, header = [], []
    with open(path) as fh:
        for i, raw in enumerate(fh, 1):
            line = raw.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                if not syms:
                    header.append(line)
                continue
            parts = [p.strip() for p in line.split("|", 2)]
            if len(parts) != 3:
                sys.exit(f"{path}:{i}: expected 'name | flags | prototype'")
            name, fl, proto = parts
            flags = {}
            for f in filter(None, (x.strip() for x in fl.split(","))):
                k, _, v = f.partition("=")
                if k not in ("perm", "wrap", "ext", "data", "since"):
                    sys.exit(f"{path}:{i}: unknown flag {k}")
                if k == "perm" and v not in PERMS:
                    sys.exit(f"{path}:{i}: unknown permission {v}")
                flags[k] = v if v else True
            syms.append(Sym(name, flags, proto, i))
    return header, syms


def write_sym(path, header, syms):
    syms = sorted(syms, key=lambda s: s.name.encode())
    with open(path, "w") as fh:
        for h in header:
            fh.write(h + "\n")
        for s in syms:
            fh.write(s.format() + "\n")


def norm(proto):
    p = re.sub(r"\s+", " ", proto.strip())
    p = re.sub(r"\s*([(),*])\s*", r"\1", p)
    p = re.sub(r"([(,])", r"\1 ", p)
    p = p.replace("( ", "(")
    p = re.sub(r"\*(\w)", r"* \1", p)
    p = re.sub(r"(\w)\*", r"\1 *", p)
    return p.rstrip(";") + ";"


def ret_kind(sym):
    m = re.match(r"(.*?)\b" + re.escape(sym.name) + r"\s*\(", sym.proto)
    ret = (m.group(1) if m else "").strip()
    ret = re.sub(r"\b(extern|static|inline|LV_ATTRIBUTE_\w+)\b", "", ret).strip()
    if ret.endswith("*"):
        return "T5_ABI_RET_PTR"
    if ret == "void":
        return "T5_ABI_RET_VOID"
    if ret == "bool":
        return "T5_ABI_RET_BOOL"
    if ret in ("t5_err_t", "int", "BaseType_t", "esp_err_t"):
        return "T5_ABI_RET_ERR"
    return "T5_ABI_RET_ZERO"


def rename(proto, old, new):
    return re.sub(r"\b" + re.escape(old) + r"(\s*\()", new + r"\1", proto, count=1)


def guarded(proto, name):
    """(name)(...) so a libc function-like macro (putchar...) isn't expanded."""
    return re.sub(r"\b" + re.escape(name) + r"\s*\(", "(" + name + ")(", proto, count=1)


def cmd_gen(a):
    _, syms = parse_sym(a.sym)
    level = int(re.search(r"api_v(\d+)", os.path.basename(a.sym)).group(1))
    out = [
        "/* Generated by tools/abi/abi.py from %s. Do not edit. */" % os.path.basename(a.sym),
        '#include "abi_includes.h"',
        '#include "t5_app/abi.h"',
        '#include "t5_pkg/manifest.h"',
        "",
        "/* Every exported prototype, redeclared: if a header no longer matches",
        " * the published ABI, this file stops compiling. */",
        "#pragma GCC diagnostic push",
        '#pragma GCC diagnostic ignored "-Wredundant-decls"',
    ]
    for s in syms:
        out.append(guarded(s.proto, s.name))
        if s.wrap:
            out.append(rename(s.proto, s.name, s.wrap))
    out += ["#pragma GCC diagnostic pop", "", "const t5_abi_sym_t t5_abi_table[] = {"]
    for s in sorted(syms, key=lambda s: s.name.encode()):
        target = s.wrap or s.name
        perm = PERMS[s.perm] if s.perm else "0"
        out.append(f'    {{ "{s.name}", (const void *)&{target}, {perm}, {ret_kind(s)}, {s.since or level} }},')
    out += ["};", f"const size_t t5_abi_count = {len(syms)};", ""]
    data = "\n".join(out)
    if os.path.exists(a.out) and open(a.out).read() == data:
        return
    with open(a.out, "w") as fh:
        fh.write(data)


def strip_c(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r"^\s*#.*?(?<!\\)$", " ", text, flags=re.M)
    # Annotations that don't change the ABI (docs/coding-standard.md, rule 7).
    text = re.sub(r"\bT5_MUST_CHECK\b", " ", text)
    return text


def header_protos(dirs):
    """name -> prototype for every function declaration (not inline) found."""
    protos = {}
    for d in dirs:
        for root, _, files in os.walk(d):
            if "private" in root.split(os.sep):
                continue
            for f in sorted(files):
                if not f.endswith(".h") or f.endswith("_private.h"):
                    continue
                text = strip_c(open(os.path.join(root, f), errors="replace").read())
                text = re.sub(r'extern\s*"C"\s*{', " ", text)
                for stmt in re.split(r"[;{}]", text):
                    s = " ".join(stmt.split())
                    if not s or "typedef" in s or "static" in s or "=" in s:
                        continue
                    m = re.match(r"^(?:extern\s+)?([\w\s\*]+?)\b(\w+)\s*\((.*)\)\s*(?:LV_\w+(?:\([^)]*\))?\s*)*$", s)
                    if not m or m.group(2) in ("if", "while", "for", "return", "sizeof"):
                        continue
                    name = m.group(2)
                    protos.setdefault(name, norm(s))
    return protos


def cmd_update(a):
    header, syms = parse_sym(a.sym)
    protos = header_protos(a.headers)
    bad = 0
    for s in syms:
        if s.ext:
            s.proto = norm(s.proto)
            continue
        p = protos.get(s.name)
        if not p:
            print(f"{a.sym}:{s.line}: {s.name}: no declaration found in the headers", file=sys.stderr)
            bad += 1
            continue
        s.proto = p
    if bad:
        sys.exit(1)
    write_sym(a.sym, header, syms)


def cmd_add_lvgl(a):
    header, syms = parse_sym(a.sym)
    have = {s.name for s in syms}
    nm = os.environ.get("NM", "riscv32-esp-elf-nm")
    out = subprocess.run([nm, "-g", "--defined-only", a.lib], capture_output=True, text=True, check=True).stdout
    compiled = {l.split()[-1] for l in out.splitlines() if re.search(r" T lv_\w+$", l)}
    protos = header_protos([a.headers])
    added = 0
    for name in sorted(compiled):
        if name in have or name not in protos:
            continue
        syms.append(Sym(name, {}, protos[name], 0))
        added += 1
    write_sym(a.sym, header, syms)
    print(f"added {added} LVGL functions ({len(compiled)} compiled, {len(syms)} symbols total)")


def git_show(ref, path):
    root = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True,
                          check=True).stdout.strip()
    rel = os.path.relpath(os.path.abspath(path), root)
    r = subprocess.run(["git", "show", f"{ref}:{rel}"], capture_output=True, text=True, cwd=root)
    return r.stdout if r.returncode == 0 else None


def lock_path(sym):
    return re.sub(r"\.sym$", ".lock", sym)


LOCK_HEADER = """# status: %s
# The contract for this API level: do not edit (docs/abi-stability.md).
# Written by tools/abi/abi.py freeze.
"""


def read_lock(path):
    """(status, [Sym]), or (None, []) if there is no lock."""
    if not os.path.exists(path):
        return None, []
    with open(path) as fh:
        first = fh.readline()
    m = re.match(r"# status: (candidate|released)\s*$", first)
    if not m:
        sys.exit(f"{path}: first line must be '# status: candidate' or '# status: released'")
    return m.group(1), parse_sym(path)[1]


def check_against(base, syms, what):
    """Everything in base must still be there, unchanged. Returns the error count."""
    errs = 0
    now = {s.name: s for s in syms}
    for b in base:
        s = now.get(b.name)
        if not s:
            print(f"removed from {what}: {b.name}", file=sys.stderr)
            errs += 1
        elif s.data != b.data or norm(s.proto) != norm(b.proto):
            print(f"changed in {what}: {b.name}\n  was: {b.proto}\n  now: {s.proto}", file=sys.stderr)
            errs += 1
        elif s.perm != b.perm:
            print(f"permission changed in {what}: {b.name}: {b.perm} -> {s.perm}", file=sys.stderr)
            errs += 1
    return errs


def cmd_freeze(a):
    _, syms = parse_sym(a.sym)
    path = lock_path(a.sym)
    status, _ = read_lock(path)
    if status == "released" and not a.force:
        sys.exit(f"{path} is released: apps depend on it. Add symbols with since=<N+1> instead (--force overrides)")
    new = "released" if a.release or status == "released" else "candidate"
    with open(path, "w") as fh:
        fh.write(LOCK_HEADER % new)
        for s in sorted(syms, key=lambda s: s.name.encode()):
            flags = [f"perm={s.perm}"] if s.perm else []
            if s.data:
                flags.append("data")
            fh.write(f"{s.name} | {','.join(flags)} | {norm(s.proto) if not s.data else s.proto}\n")
    print(f"{path}: {len(syms)} symbols, {new}")


def cmd_check(a):
    _, syms = parse_sym(a.sym)
    errs = 0
    names = [s.name for s in syms]
    if names != sorted(names, key=lambda n: n.encode()):
        print(f"{a.sym}: not sorted (run abi.py update)", file=sys.stderr)
        errs += 1
    if len(set(names)) != len(names):
        print(f"{a.sym}: duplicate symbols", file=sys.stderr)
        errs += 1
    for s in syms:
        declares = re.search(r"\b" + re.escape(s.name) + (r"\b" if s.data else r"\s*\("), s.proto)
        if not declares:
            print(f"{a.sym}:{s.line}: {s.name}: prototype doesn't declare it", file=sys.stderr)
            errs += 1
    status, locked = read_lock(lock_path(a.sym))
    if status:
        errs += check_against(locked, syms, "the locked ABI")
        known = {s.name for s in locked}
        level = int(re.search(r"api_v(\d+)", os.path.basename(a.sym)).group(1))
        added = [s for s in syms if s.name not in known]
        if status == "released":
            for s in added:
                if s.since <= level:
                    print(f"{s.name}: level {level} is released; a new symbol needs since={level + 1}", file=sys.stderr)
                    errs += 1
        print(f"{len(locked)} locked symbols checked ({status}), {len(added)} added since")
    if a.base:
        old = git_show(a.base, a.sym)
        if old is None:
            print(f"{a.sym} not in {a.base}: nothing published to compare against")
        else:
            tmp = a.sym + ".base"
            open(tmp, "w").write(old)
            try:
                _, base = parse_sym(tmp)
            finally:
                os.remove(tmp)
            errs += check_against(base, syms, a.base)
            print(f"{len(base)} published symbols checked against {a.base}, {len(syms) - len(base)} added")
    if errs:
        sys.exit(1)
    print(f"{a.sym}: {len(syms)} symbols OK")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gen"); g.add_argument("sym"); g.add_argument("out"); g.set_defaults(fn=cmd_gen)
    u = sub.add_parser("update"); u.add_argument("sym"); u.add_argument("headers", nargs="+"); u.set_defaults(fn=cmd_update)
    l = sub.add_parser("add-lvgl"); l.add_argument("sym"); l.add_argument("lib"); l.add_argument("headers")
    l.set_defaults(fn=cmd_add_lvgl)
    c = sub.add_parser("check"); c.add_argument("sym"); c.add_argument("--base"); c.set_defaults(fn=cmd_check)
    f = sub.add_parser("freeze"); f.add_argument("sym"); f.add_argument("--release", action="store_true")
    f.add_argument("--force", action="store_true"); f.set_defaults(fn=cmd_freeze)
    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
