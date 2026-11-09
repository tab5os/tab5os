#!/usr/bin/env python3
"""Generate docs/api-reference.md from the published ABI and the Python bindings (plan 8.7).

usage: apidoc.py [OUT]   (default docs/api-reference.md)

C: every non-LVGL symbol in sdk/abi/api_v1.sym, grouped by the firmware
header that declares it (in the order the SDK headers include them), with its
prototype, the permission it needs and whether a tracking wrapper is bound
in its place, plus the header comment above its declaration. Python: every
name in the `t5` modules, read from the module tables in
firmware/components/t5_py/mod_*.c and the generated bindings list.
Deterministic; CI can diff it.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "t5"))
import abi  # noqa: E402
import t5const  # noqa: E402

COMPONENTS = os.path.join(ROOT, "firmware", "components")
SDK_INCLUDE = os.path.join(ROOT, "sdk", "include", "t5")


def firmware_header(rel):
    """Path of a firmware public header named as included ("t5_app/app.h"), or None."""
    comp = rel.split("/", 1)[0]
    for c in (comp, comp.replace("t5_", "", 1)):
        p = os.path.join(COMPONENTS, c, "include", rel)
        if os.path.exists(p):
            return p
    return None


def declared(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        text = abi.strip_c(f.read())
    return set(re.findall(r"\b(\w+)\s*\(", text))


def header_groups():
    """[(title, names declared)] for the firmware headers the SDK headers include, in their order."""
    out = []
    for sdk in sorted(os.listdir(SDK_INCLUDE)):
        with open(os.path.join(SDK_INCLUDE, sdk)) as f:
            for rel in re.findall(r'^#include "([^"]+)"', f.read(), re.M):
                path = firmware_header(rel)
                if path:
                    out.append((f"{rel} (t5/{sdk})", declared(path)))
    return out


def header_comments():
    """name -> the comment block right above its declaration in the headers."""
    out = {}
    for comp in os.listdir(os.path.join(ROOT, "firmware", "components")):
        inc = os.path.join(ROOT, "firmware", "components", comp, "include")
        for root, _, files in os.walk(inc):
            for fn in files:
                if not fn.endswith(".h"):
                    continue
                with open(os.path.join(root, fn), encoding="utf-8", errors="replace") as f:
                    lines = f.read().split("\n")
                for i, line in enumerate(lines):
                    m = re.match(r"[\w\s\*]+?\b(\w+)\s*\(", line)
                    if not m or line.lstrip().startswith(("/*", "*", "#", "//")):
                        continue
                    j, block = i - 1, []
                    while j >= 0 and lines[j].strip().startswith(("/*", "*", "//")) and len(block) < 8:
                        block.insert(0, lines[j].strip().lstrip("/*").rstrip("*/").strip())
                        j -= 1
                    text = " ".join(b for b in block if b)
                    if text and m.group(1) not in out:
                        out[m.group(1)] = text
    return out


def python_names():
    mods = {}
    src = os.path.join(ROOT, "firmware", "components", "t5_py")
    for fn in sorted(os.listdir(src)):
        if not fn.startswith("mod_") or not fn.endswith(".c"):
            continue
        with open(os.path.join(src, fn)) as f:
            text = f.read()
        for name, body in re.findall(r"static const mp_rom_map_elem_t (\w+)_globals_table\[\] = \{(.*?)\};", text, re.S):
            names = [n for n in re.findall(r"MP_QSTR_(\w+)\)", body) if not n.startswith("__")]
            mods.setdefault(name, []).extend(n for n in names if n != name)
        m = re.search(r"widget_locals_table\[\] = \{(.*?)\};", text, re.S)
        if m:
            mods["ui widget methods"] = re.findall(r"MP_QSTR_(\w+)\)", m.group(1))
        for tname, body in re.findall(r"static const mp_rom_map_elem_t net_(\w+)_locals_table\[\] = \{(.*?)\};", text, re.S):
            label = {"udp": "UDP", "websocket": "WebSocket", "mqtt": "MQTT", "httpd": "HTTPServer"}.get(
                tname, tname.capitalize())
            mods["net." + label + " methods"] = [n for n in re.findall(r"MP_QSTR_(\w+)\)", body) if not n.startswith("__")]
        if fn == "mod_socket.c":
            m = re.search(r"net_sock_locals_table\[\] = \{(.*?)\};", text, re.S)
            mods["socket.socket methods"] = [n for n in re.findall(r"MP_QSTR_(\w+)\)", m.group(1)) if not n.startswith("_")]
        m = re.search(r"port_locals_table\[\] = \{(.*?)\};", text, re.S)
        if m:
            mods["port.Port methods"] = [n for n in re.findall(r"MP_QSTR_(\w+)\)", m.group(1)) if not n.startswith("__")]
    with open(os.path.join(src, "bindings.txt")) as f:
        for line in f:
            p = line.split("#", 1)[0].split()
            if len(p) == 3:
                mods.setdefault(p[0], []).append(p[1] + " (from " + p[2] + ")")
    return mods


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "docs", "api-reference.md")
    _, syms = abi.parse_sym(os.path.join(ROOT, "sdk", "abi", "api_v1.sym"))
    notes = header_comments()
    groups_of = header_groups()
    order = [t for t, _ in groups_of]
    groups = {}
    lvgl = 0
    libc = 0
    for s in syms:
        if s.name.startswith("lv_"):
            lvgl += 1
            continue
        title = next((t for t, names in groups_of if s.name in names), None)
        if title is None:
            libc += 1
            continue
        groups.setdefault(title, []).append(s)
    w = []
    w.append(f"# Tab5 OS API reference (API level {t5const.define(t5const.MANIFEST_H, 'T5_API_LEVEL')})\n")
    w.append("Generated by `tools/abi/apidoc.py` from [`sdk/abi/api_v1.sym`](../sdk/abi/api_v1.sym), the SDK headers and "
             "the Python bindings. Do not edit; run `python3 tools/abi/apidoc.py`. The guides are [sdk.md](sdk.md) "
             "(C) and [python.md](python.md).\n")
    w.append(f"Native apps may call the {sum(len(v) for v in groups.values())} Tab5 OS functions below, "
             f"{lvgl} LVGL 9.6 functions (the LVGL docs apply unchanged) and {libc} C library and lwIP functions. "
             "**perm** is the manifest permission an app needs: without it the call is bound to a stub that "
             "returns `T5_ERR_DENIED`. **tracked** means the OS binds a wrapper that records what the call "
             "acquires, so it is freed when the app exits.\n")
    w.append("## C\n")
    for title in sorted(groups, key=order.index):
        w.append(f"### {title}\n")
        for s in sorted(groups[title], key=lambda x: x.name):
            flags = []
            if "perm" in s.flags:
                flags.append(f"perm {s.flags['perm']}")
            if "wrap" in s.flags:
                flags.append("tracked")
            w.append(f"- `{s.proto.strip()}`" + (f" ({', '.join(flags)})" if flags else ""))
            if s.name in notes:
                w.append(f"  {notes[s.name]}")
        w.append("")
    w.append("## Python: the `t5` module\n")
    w.append("Python apps reach the hardware only through `t5` ([python.md](python.md) explains the containment). "
             "Names marked *from* are generated from the C function named, with the same permission check.\n")
    for mod, names in sorted(python_names().items()):
        label = "t5" if mod == "t5" else (mod if " " in mod or "." in mod else "t5." + mod)
        w.append(f"- **{label}**: " + ", ".join(f"`{n}`" for n in names))
    w.append("")
    text = "\n".join(w)
    with open(out_path, "w") as f:
        f.write(text)
    print(f"{out_path}: {sum(len(v) for v in groups.values())} C functions, {len(python_names())} Python groups")


if __name__ == "__main__":
    main()
