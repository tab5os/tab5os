#!/usr/bin/env python3
"""Generate MicroPython bindings for the scalar part of the Tab5 OS SDK (plan 8.3).

usage: genbind.py SYMFILE BINDINGS OUT_H INCLUDE_DIR...

Reads the published ABI (sdk/abi/api_v1.sym) for each function's prototype,
permission and tracking wrapper, the binding list (which functions, under
which Python module and name; which other enums need converters), and the
SDK headers for enum definitions. Writes OUT_H, which mod_*.c include: a
wrapper per function, enum converters, and per module a
T5_GEN_<MODULE>_GLOBALS list of dict entries. Next to it, bind_perm.h has
T5G_PERM_<symbol> for every symbol with a permission, for the bindings
written by hand.

Types handled: void, bool, char/short/int/long and the fixed-width integers,
size_t, time_t, float, double, const char * (str), t5_err_t (raises OSError
on failure), t5_app_t * (the running app, not a Python argument) and enums
(lower-case member names, or plain ints for members like T5_ROT_90).
Anything else is an error: write that function by hand.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "abi"))
import abi  # noqa: E402  (parse_sym, the .sym format)

INTS = {
    "int": ("int", True), "long": ("int", True), "short": ("int", True), "char": ("int", True),
    "int8_t": ("int", True), "int16_t": ("int", True), "int32_t": ("int", True),
    "uint8_t": ("int", False), "uint16_t": ("int", False), "uint32_t": ("int", False),
    "unsigned": ("int", False), "unsigned int": ("int", False), "size_t": ("int", False),
}
LONG = {"int64_t", "time_t", "long long", "uint64_t"}
FLOATS = {"float", "double"}


def strip_c(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def load_enums(dirs):
    enums = {}
    for d in dirs:
        for root, _, files in os.walk(d):
            for fn in files:
                if not fn.endswith(".h"):
                    continue
                with open(os.path.join(root, fn), encoding="utf-8", errors="replace") as f:
                    text = strip_c(f.read())
                for body, name in re.findall(r"typedef\s+enum\s*\w*\s*\{(.*?)\}\s*(\w+)\s*;", text, re.S):
                    members, val = [], -1
                    for m in filter(None, (x.strip() for x in body.split(","))):
                        k, _, v = m.partition("=")
                        k = k.strip()
                        if v.strip():
                            try:
                                val = int(v.strip(), 0)
                            except ValueError:
                                val = None
                        else:
                            val = None if val is None else val + 1
                        if val is None:
                            break
                        members.append((k, val))
                    else:
                        members = [(k, v) for k, v in members if not k.endswith("_COUNT")]
                        if members:
                            enums[name] = members
    return enums


def enum_names(members):
    names = [k for k, _ in members]
    pre = names[0]
    for n in names[1:]:
        while not n.startswith(pre):
            pre = pre[:-1]
    if "_" in pre:
        pre = pre[:pre.rfind("_") + 1]
    else:
        pre = ""
    short = [k[len(pre):].lower() for k in names]
    numeric = all(s.isdigit() for s in short)
    return short, numeric


def parse_proto(proto):
    m = re.match(r"\s*(.*?)\s*\b(\w+)\s*\((.*)\)\s*;?\s*$", proto)
    if not m:
        raise ValueError("can't parse prototype: " + proto)
    ret, name, args = m.group(1), m.group(2), m.group(3).strip()
    params = []
    if args and args != "void":
        for a in args.split(","):
            a = a.strip()
            am = re.match(r"(.*?)(\w+)$", a)
            if not am:
                raise ValueError("can't parse argument %r of %s" % (a, name))
            params.append((re.sub(r"\s+", " ", am.group(1)).strip(), am.group(2)))
    return re.sub(r"\s+", " ", ret).strip(), name, params


def norm_type(t):
    return re.sub(r"\s*\*\s*", " *", t).strip()


def classify(t, enums):
    t = norm_type(t)
    if t == "void":
        return ("void",)
    if t == "bool":
        return ("bool",)
    if t in ("const char *",):
        return ("str",)
    if t == "t5_err_t":
        return ("err",)
    if t in ("t5_app_t *", "struct t5_app *"):
        return ("app",)
    if t in INTS:
        return ("int", INTS[t][1])
    if t in LONG:
        return ("long",)
    if t in FLOATS:
        return ("float",)
    if t in enums:
        return ("enum", t)
    return None


def gen(sym_path, bind_path, out_path, inc_dirs):
    _, syms = abi.parse_sym(sym_path)
    by_name = {s.name: s for s in syms}
    enums = load_enums(inc_dirs)
    binds = []
    extra_enums = []
    with open(bind_path) as f:
        for i, line in enumerate(f, 1):
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) == 2 and parts[0] == "enum":
                extra_enums.append((i, parts[1]))
                continue
            if len(parts) != 3:
                sys.exit("%s:%d: expected 'module python_name c_symbol' or 'enum type'" % (bind_path, i))
            binds.append((i, *parts))

    # The header that declares each bound function or listed enum, so the
    # output compiles alone.
    decl_hdr = {}
    wanted = [(csym, r"\b%s\s*\(" % re.escape(csym)) for _, _, _, csym in binds]
    wanted += [(e, r"\}\s*%s\s*;" % re.escape(e)) for _, e in extra_enums]
    for d in inc_dirs:
        for root, _, files in os.walk(d):
            for fn in files:
                if fn.endswith(".h"):
                    path = os.path.join(root, fn)
                    with open(path, encoding="utf-8", errors="replace") as f:
                        text = strip_c(f.read())
                    for name, pat in wanted:
                        if name not in decl_hdr and re.search(pat, text):
                            decl_hdr[name] = os.path.relpath(path, d)
    out = []
    w = out.append
    w("/* Generated by tools/py/genbind.py from sdk/abi/api_v1.sym and")
    w(" * firmware/components/t5_py/bindings.txt. Do not edit. */")
    w("#pragma once")
    w('#include "py/runtime.h"')
    w('#include "py/obj.h"')
    w('#include "py_priv.h"')
    for h in sorted(set(decl_hdr.values())):
        w('#include "%s"' % h)
    w("")
    used_enums = {}
    for line, ename in extra_enums:
        if ename not in enums:
            sys.exit("%s:%d: no enum %s with plain values in the SDK headers" % (bind_path, line, ename))
        used_enums[ename] = enums[ename]
    modules = {}
    for line, mod, pyname, csym in binds:
        s = by_name.get(csym)
        if not s:
            sys.exit("%s:%d: %s is not in %s" % (bind_path, line, csym, sym_path))
        ret, name, params = parse_proto(s.proto)
        rk = classify(ret, enums)
        if rk is None:
            sys.exit("%s:%d: %s returns %s, which genbind can't map" % (bind_path, line, csym, ret))
        pk = []
        for t, pn in params:
            k = classify(t, enums)
            if k is None or k[0] in ("void", "err"):
                sys.exit("%s:%d: %s takes %s %s, which genbind can't map" % (bind_path, line, csym, t, pn))
            pk.append((t, pn, k))
        for k in [rk] + [p[2] for p in pk]:
            if k[0] == "enum":
                used_enums[k[1]] = enums[k[1]]
        call = s.flags.get("wrap", csym) if isinstance(s.flags.get("wrap"), str) else csym
        py_args = [p for p in pk if p[2][0] != "app"]
        fn = "t5g_%s_%s" % (mod, pyname)
        w("/* %s.%s: %s */" % (mod, pyname, s.proto.strip()))
        if len(py_args) <= 3:
            sig = ", ".join("mp_obj_t a%d" % j for j in range(len(py_args))) or "void"
            w("static mp_obj_t %s(%s)" % (fn, sig))
        else:
            w("static mp_obj_t %s(size_t n_args, const mp_obj_t *args)" % fn)
        w("{")
        if "perm" in s.flags:
            w("    py_need_perm(%s);" % abi.PERMS[s.flags["perm"]])
        cargs = []
        j = 0
        for t, pn, k in pk:
            src = "a%d" % j if len(py_args) <= 3 else "args[%d]" % j
            if k[0] == "app":
                w("    void *v_%s = py_vm ? py_vm->app : NULL;" % pn)
                w("    if (!v_%s) mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT(\"only inside an app\"));" % pn)
                cargs.append("(%s)v_%s" % (norm_type(t), pn))
                continue
            j += 1
            if k[0] == "bool":
                w("    bool v_%s = mp_obj_is_true(%s);" % (pn, src))
            elif k[0] == "str":
                w("    const char *v_%s = mp_obj_str_get_str(%s);" % (pn, src))
            elif k[0] == "int":
                w("    %s v_%s = (%s)mp_obj_get_int(%s);" % (norm_type(t), pn, norm_type(t), src))
            elif k[0] == "long":
                w("    %s v_%s = (%s)mp_obj_get_ll(%s);" % (norm_type(t), pn, norm_type(t), src))
            elif k[0] == "float":
                w("    %s v_%s = (%s)mp_obj_get_float(%s);" % (norm_type(t), pn, norm_type(t), src))
            elif k[0] == "enum":
                w("    %s v_%s = (%s)t5g_enum_in_%s(%s);" % (k[1], pn, k[1], k[1], src))
            cargs.append("v_%s" % pn)
        callexpr = "%s(%s)" % (call, ", ".join(cargs))
        r = rk[0]
        if r == "void":
            w("    %s;" % callexpr)
            w("    return mp_const_none;")
        elif r == "err":
            w("    py_check(%s);" % callexpr)
            w("    return mp_const_none;")
        elif r == "bool":
            w("    return mp_obj_new_bool(%s);" % callexpr)
        elif r == "str":
            w("    const char *r = %s;" % callexpr)
            w("    return r ? mp_obj_new_str_from_cstr(r) : mp_const_none;")
        elif r == "int":
            w("    return %s(%s);" % ("mp_obj_new_int" if rk[1] else "mp_obj_new_int_from_uint", callexpr))
        elif r == "long":
            w("    return mp_obj_new_int_from_ll((long long)%s);" % callexpr)
        elif r == "float":
            w("    return mp_obj_new_float((mp_float_t)%s);" % callexpr)
        elif r == "enum":
            w("    return t5g_enum_out_%s((int)%s);" % (rk[1], callexpr))
        elif r == "app":
            sys.exit("%s:%d: %s returns an app handle" % (bind_path, line, csym))
        w("}")
        n = len(py_args)
        if n <= 3:
            w("static MP_DEFINE_CONST_FUN_OBJ_%d(%s_obj, %s);" % (n, fn, fn))
        else:
            w("static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(%s_obj, %d, %d, %s);" % (fn, n, n, fn))
        w("")
        modules.setdefault(mod, []).append((pyname, fn))

    # Enum converters go first in the file: prepend them.
    conv = []
    c = conv.append
    for ename, members in sorted(used_enums.items()):
        short, numeric = enum_names(members)
        c("/* %s */" % ename)
        c("static int __attribute__((unused)) t5g_enum_in_%s(mp_obj_t o)" % ename)
        c("{")
        if not numeric:
            c("    if (mp_obj_is_str(o)) {")
            c("        const char *s = mp_obj_str_get_str(o);")
            for (k, v), sn in zip(members, short):
                c('        if (!strcmp(s, "%s")) return %d;' % (sn, v))
            c('        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("unknown name %s"), s);')
            c("    }")
            c("    return (int)mp_obj_get_int(o);")
        else:
            c("    int v = (int)mp_obj_get_int(o);")
            for (k, v), sn in zip(members, short):
                c("    if (v == %s) return %d;" % (sn, v))
            c("    return v;")
        c("}")
        c("static mp_obj_t __attribute__((unused)) t5g_enum_out_%s(int v)" % ename)
        c("{")
        c("    switch (v) {")
        for (k, v), sn in zip(members, short):
            if numeric:
                c("    case %d: return MP_OBJ_NEW_SMALL_INT(%s);" % (v, sn))
            else:
                c('    case %d: return MP_OBJ_NEW_QSTR(MP_QSTR_%s);' % (v, sn))
        c("    default: return mp_obj_new_int(v);")
        c("    }")
        c("}")
        c("")
    idx = out.index("") + 1
    out[idx:idx] = ['#include <string.h>', ""] + conv

    # Material icons by name, from md_icons.h (t5.ui icon="wifi").
    icons = []
    for d in inc_dirs:
        path = os.path.join(d, "md", "md_icons.h")
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                icons = re.findall(r'#define MD_ICON_(\w+)\s+"([^"]*)"', f.read())
            break
    w("typedef struct { const char *name; const char *glyph; } t5g_icon_t;")
    w("static const t5g_icon_t t5g_icons[] __attribute__((unused)) = {")
    for name, glyph in icons:
        w('    { "%s", "%s" },' % (name.lower(), glyph))
    w("    { NULL, NULL },")
    w("};")
    w("")
    for mod, entries in sorted(modules.items()):
        w("#define T5_GEN_%s_GLOBALS \\" % mod.upper())
        for pyname, fn in entries:
            w("    { MP_ROM_QSTR(MP_QSTR_%s), MP_ROM_PTR(&%s_obj) }, \\" % (pyname, fn))
        w("")
    write_if_changed(out_path, "\n".join(out) + "\n")

    perm = ["/* Generated by tools/py/genbind.py from sdk/abi/api_v1.sym. Do not edit. */",
            "#pragma once", '#include "t5_pkg/manifest.h"', ""]
    for s in sorted(syms, key=lambda s: s.name):
        if "perm" in s.flags:
            perm.append("#define T5G_PERM_%s %s" % (s.name, abi.PERMS[s.flags["perm"]]))
    write_if_changed(os.path.join(os.path.dirname(out_path), "bind_perm.h"), "\n".join(perm) + "\n")


def write_if_changed(path, text):
    old = None
    if os.path.exists(path):
        with open(path) as f:
            old = f.read()
    if old != text:
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        with open(path, "w") as f:
            f.write(text)


if __name__ == "__main__":
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    gen(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:])
