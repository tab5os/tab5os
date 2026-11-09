#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Coding standard checks the compiler can't make (docs/coding-standard.md).

  rule 1  no setjmp/longjmp in our code except the fault recovery points in
          JMP_OK; in the strict components, no recursion except the bounded
          cases listed in RECURSION_OK
  rule 7  every t5_err_t function a strict component's public headers
          declare is marked T5_MUST_CHECK

The strict components are read from the STRICT lists in firmware/CMakeLists.txt
and recovery/CMakeLists.txt, so each list lives in one place. Exit status 1 if
anything is found.

    tools/lint_std.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUR_CODE = ["firmware/components", "firmware/main", "firmware/sysapps", "recovery/main"]
VENDORED = {"esp_driver_ppa"}

# Where an app fault is survived (t5_core/fault.h): a recovery point is what
# lets the OS leave a crashed app's code without a reboot (rule 1).
JMP_OK = {
    "firmware/components/t5_core/t5_fault_idf.c",
    "firmware/components/t5_ui/ui.c",
    "firmware/components/t5_app/wrappers.c",
}

# file -> functions whose recursion has an explicit depth limit (rule 1).
RECURSION_OK = {
    "firmware/components/t5_app/lifecycle.c": {"du"},                                    # depth > 6 stops
    "firmware/components/t5_ports/profiles.c": {"skip"},                                 # DEPTH_MAX
    "firmware/components/t5_pkg/toml.c": {"parse_value", "parse_array", "value_free"},   # nested arrays: depth 2
    "recovery/main/recovery.c": {"rm_tree"},                                             # RM_TREE_DEPTH_MAX
}


def strict_components():
    """Directories (relative to ROOT) of the strict components of the firmware
    and recovery projects; a project's "main" is its own main/."""
    out = []
    for project in ("firmware", "recovery"):
        text = open(os.path.join(ROOT, project, "CMakeLists.txt")).read()
        m = re.search(r"\bSTRICT\s+([^)]*)\)", text)
        if not m:
            sys.exit("lint_std: no STRICT list in %s/CMakeLists.txt" % project)
        for comp in m.group(1).split():
            d = os.path.join(project, "main") if comp == "main" else os.path.join("firmware", "components", comp)
            if d not in out:
                out.append(d)
    return out


def strip_c(text):
    """Comments and string/char literals blanked, line numbers kept."""
    def blank(m):
        return re.sub(r"[^\n]", " ", m.group(0))
    return re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', blank, text, flags=re.S)


def c_files(paths, exts=(".c", ".h")):
    for p in paths:
        for root, dirs, files in os.walk(os.path.join(ROOT, p)):
            dirs[:] = [d for d in dirs if d not in VENDORED and not d.startswith("build")]
            for f in sorted(files):
                if f.endswith(exts):
                    yield os.path.join(root, f)


def functions(text):
    """(name, line, body) for each function definition: the name at column 0
    or after a return type on the same line, the body's '{' at column 0."""
    for m in re.finditer(r"(?m)^[A-Za-z_][\w \t\*]*?\b(\w+)\s*\([^;{}]*\)\s*\n\{", text):
        start = m.end()
        depth, i = 1, start
        while depth and i < len(text):
            depth += {"{": 1, "}": -1}.get(text[i], 0)
            i += 1
        yield m.group(1), text.count("\n", 0, m.start(1)) + 1, text[start:i]


def main():
    problems = []
    for path in c_files(OUR_CODE):
        text = strip_c(open(path, errors="replace").read())
        if os.path.relpath(path, ROOT) in JMP_OK:
            continue
        for m in re.finditer(r"\b(setjmp|longjmp|sigsetjmp|siglongjmp)\s*\(", text):
            line = text.count("\n", 0, m.start()) + 1
            problems.append("%s:%d: rule 1: %s" % (os.path.relpath(path, ROOT), line, m.group(1)))

    for cdir in strict_components():
        if not os.path.isdir(os.path.join(ROOT, cdir)):
            continue
        for path in c_files([cdir], (".c",)):
            rel = os.path.relpath(path, ROOT)
            text = strip_c(open(path, errors="replace").read())
            for name, line, body in functions(text):
                # Not a member call: a->on_rotate() is the app's, not this function.
                calls_self = re.search(r"(?<![\w.>])%s\s*\(" % re.escape(name), body)
                if calls_self and name not in RECURSION_OK.get(rel, ()):
                    problems.append("%s:%d: rule 1: %s() calls itself; make it "
                                    "iterative, or give it a depth limit and list it in RECURSION_OK"
                                    % (rel, line, name))
        inc = os.path.join(cdir, "include")
        for path in c_files([inc], (".h",)) if os.path.isdir(os.path.join(ROOT, inc)) else []:
            for n, line in enumerate(open(path, errors="replace"), 1):
                if re.match(r"t5_err_t\s+\w+\s*\(", line):
                    problems.append("%s:%d: rule 7: mark it T5_MUST_CHECK (or don't return t5_err_t)"
                                    % (os.path.relpath(path, ROOT), n))

    for p in problems:
        print(p)
    if problems:
        print("lint_std: %d problem(s); see docs/coding-standard.md" % len(problems))
        return 1
    print("lint_std: OK (strict: %s)" % " ".join(os.path.basename(d) if "components" in d else d
                                                 for d in strict_components()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
