#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Flag raw pixel numbers in UI code (plan 4.3).

Sizes, positions, padding, radii and other lengths in the MD2 layer and the
shell must be written in dp, as T5_DP(n), so the 2 px/dp density stays in
one place. This scans calls that take a length (lv_obj_set_size,
lv_obj_set_style_pad_*, lv_obj_align...) and reports any bare number other
than 0, unless the line carries a /* px */ comment (a deliberate hairline).

    tools/lint_px.py [paths...]     default: md, t5_shell and the system apps
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT = ["firmware/components/md", "firmware/components/t5_shell", "firmware/sysapps"]

# function -> indexes of length arguments
LENGTH_FUNCS = {
    "lv_obj_set_width": [1], "lv_obj_set_height": [1], "lv_obj_set_size": [1, 2],
    "lv_obj_set_x": [1], "lv_obj_set_y": [1], "lv_obj_set_pos": [1, 2],
    "lv_obj_align": [2, 3], "lv_obj_align_to": [3, 4], "lv_obj_set_ext_click_area": [1],
    "lv_obj_set_content_width": [1], "lv_obj_set_content_height": [1],
}
STYLE_LENGTH = re.compile(
    r"^lv_(?:obj|style)_set_(?:style_)?(pad_\w+|margin_\w+|width|height|min_width|max_width|min_height|max_height|"
    r"radius|border_width|outline_width|outline_pad|shadow_width|shadow_spread|shadow_offset_[xy]|"
    r"line_width|arc_width|text_line_space|text_letter_space|translate_[xy]|transform_width|transform_height|"
    r"length|x|y)$")
CALL = re.compile(r"\b(lv_\w+)\s*\(")
NUM = re.compile(r"(?<![\w.])-?\d+(?![\w.])")


def split_args(text, start):
    """Arguments of the call whose '(' is at text[start]; returns (args, end)."""
    depth, args, cur = 0, [], ""
    i = start
    while i < len(text):
        c = text[i]
        if c == "(":
            depth += 1
            if depth > 1:
                cur += c
        elif c == ")":
            depth -= 1
            if depth == 0:
                args.append(cur.strip())
                return args, i
            cur += c
        elif c == "," and depth == 1:
            args.append(cur.strip())
            cur = ""
        else:
            cur += c
        i += 1
    return args, i


def bare_numbers(arg):
    # Numbers inside T5_DP(...), LV_PCT(...), MD_* constants are fine.
    stripped = re.sub(r"\b(T5_DP|LV_PCT|LV_GRID_FR|LV_OPA_\w+)\s*\([^()]*\)", "", arg)
    stripped = re.sub(r"[*/]\s*\d+", "", stripped)      # scale factors: x / 2, n * 3
    return [n for n in NUM.findall(stripped) if n not in ("0", "-0")]


def lint_file(path):
    text = open(path, errors="replace").read()
    lines = text.split("\n")
    problems = []
    for m in CALL.finditer(text):
        fn = m.group(1)
        if fn in LENGTH_FUNCS:
            idx = LENGTH_FUNCS[fn]
        elif STYLE_LENGTH.match(fn):
            idx = [1]
        else:
            continue
        args, _ = split_args(text, m.end() - 1)
        line_no = text.count("\n", 0, m.start()) + 1
        if "/* px" in lines[line_no - 1]:
            continue
        for i in idx:
            if i < len(args):
                bad = bare_numbers(args[i])
                if bad:
                    problems.append(f"{path}:{line_no}: {fn}(): raw pixel value {', '.join(bad)} "
                                    f"in '{args[i]}' (use T5_DP(n), or mark a hairline with /* px */)")
    return problems


def main():
    paths = sys.argv[1:] or [os.path.join(ROOT, p) for p in DEFAULT]
    problems, files = [], 0
    for p in paths:
        if not os.path.exists(p):
            continue
        for root, dirs, fs in os.walk(p):
            dirs[:] = [d for d in dirs if d != "assets"]       # generated data
            for f in fs:
                if f.endswith(".c"):
                    files += 1
                    problems += lint_file(os.path.join(root, f))
    for pr in problems:
        print(pr)
    print(f"lint_px: {files} files, {len(problems)} raw pixel values")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
