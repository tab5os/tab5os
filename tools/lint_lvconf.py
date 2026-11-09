#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check that the simulator's LVGL configuration (sim/lv_conf.h) matches the
firmware's (CONFIG_LV_* in firmware/sdkconfig.defaults) wherever it affects
how things look, and that its refresh period is the UI's (T5_UI_REFRESH_MS).

Every CONFIG_LV_* line must either match an LV_* define of the same name,
match through CHOICES, or be listed in DEVICE_ONLY. Exit status 1 on a
mismatch.

    tools/lint_lvconf.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools", "t5"))
import t5const  # noqa: E402

DEFAULTS = os.path.join(ROOT, "firmware", "sdkconfig.defaults")
LV_CONF = os.path.join(ROOT, "sim", "lv_conf.h")

# Kconfig choices: CONFIG_LV_<key>=y is lv_conf's (define, value).
CHOICES = {
    "COLOR_DEPTH_16": ("LV_COLOR_FORMAT_DEFAULT", "LV_COLOR_FORMAT_RGB565"),
    "OS_NONE": ("LV_USE_OS", "LV_OS_NONE"),
    "LOG_LEVEL_WARN": ("LV_LOG_LEVEL", "LV_LOG_LEVEL_WARN"),
    "FONT_DEFAULT_MONTSERRAT_28": ("LV_FONT_DEFAULT", "&lv_font_montserrat_28"),
}
# The device's allocator, string functions, draw units and build switches:
# the simulator uses the host's (or nothing).
DEVICE_ONLY = {
    "USE_CUSTOM_MALLOC", "USE_CLIB_STRING", "USE_CLIB_SPRINTF", "USE_PPA", "USE_PPA_IMG",
    "DRAW_SW_ASM_NONE", "USE_SYSMON", "BUILD_EXAMPLES", "BUILD_DEMOS",
}


def main():
    sim = {}
    for line in open(LV_CONF):
        m = re.match(r"\s*#\s*define\s+(LV_\w+)\s+(.+?)\s*$", line)
        if m:
            sim[m.group(1)] = m.group(2)
    problems = []
    for n, line in enumerate(open(DEFAULTS), 1):
        m = re.match(r"CONFIG_LV_(\w+)=(.*)$", line.strip())
        if not m:
            continue
        key, val = m.groups()
        where = "firmware/sdkconfig.defaults:%d CONFIG_LV_%s" % (n, key)
        if key in DEVICE_ONLY:
            continue
        if key in CHOICES:
            name, want = CHOICES[key]
            if val == "y" and sim.get(name) != want:
                problems.append("%s: sim/lv_conf.h should define %s %s" % (where, name, want))
            continue
        name = "LV_" + key
        want = {"y": "1", "n": "0"}.get(val, val.strip('"'))
        if name not in sim:
            problems.append("%s: not in sim/lv_conf.h (mirror it, or list it in DEVICE_ONLY)" % where)
        elif sim[name] != want:
            problems.append("%s=%s but sim/lv_conf.h has %s %s" % (where, val, name, sim[name]))
    refresh = t5const.define("t5_ui/include/t5_ui/t5_ui.h", "T5_UI_REFRESH_MS")
    if sim.get("LV_DEF_REFR_PERIOD") != str(refresh):
        problems.append("sim/lv_conf.h: LV_DEF_REFR_PERIOD should be T5_UI_REFRESH_MS (%d)" % refresh)
    for p in problems:
        print(p)
    if problems:
        return 1
    print("lint_lvconf: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
