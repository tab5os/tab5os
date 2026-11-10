#!/usr/bin/env python3
"""Generate the Tab5 OS Material Design 2 fonts for LVGL 9.

Downloads pinned Roboto and Material Icons sources into tools/assets/.cache/,
checks their SHA-256, runs a pinned lv_font_conv through npx and writes:

  firmware/components/md/assets/fonts/*.c        LVGL bitmap fonts (bpp 4, plain)
  firmware/components/md/include/md/md_fonts.h   LV_FONT_DECLARE for every font
  firmware/components/md/include/md/md_icons.h   MD_ICON_* UTF-8 string macros
  firmware/components/md/assets/LICENSES/        font licences + README

Density is fixed at 2 px per dp, so every MD2 type size is doubled.
The output is deterministic: running it twice gives identical bytes.

Usage: python3 tools/assets/fonts.py [--offline]
Needs python3 (stdlib only) and node/npx.
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import urllib.request

LV_FONT_CONV = "lv_font_conv@1.5.3"

ROBOTO_COMMIT = "38062f4b4a0be4346d07a928408da21602545e9e"  # googlefonts/roboto main, v2.138 era
ICONS_COMMIT = "10ecc14790612ca8fd164e3f4ffe9c16fc50d5b7"   # google/material-design-icons master
MONO_COMMIT = "7bf0b33178844cc7d0c9915b746ece1be7f46bea"    # googlefonts/RobotoMono main

_R = "https://raw.githubusercontent.com/googlefonts/roboto/" + ROBOTO_COMMIT
_M = "https://raw.githubusercontent.com/google/material-design-icons/" + ICONS_COMMIT
_RM = "https://raw.githubusercontent.com/googlefonts/RobotoMono/" + MONO_COMMIT

SOURCES = {
    "Roboto-Regular.ttf": (_R + "/src/hinted/Roboto-Regular.ttf",
                           "56a45233d29f11b4dfb86d248e921939d115778f87325e7ae8cc108383d6664d"),
    "Roboto-Medium.ttf": (_R + "/src/hinted/Roboto-Medium.ttf",
                          "2879a5ecb7fbfa13a7fc3e2cdd7fecbf73aa45e91b541dfdfa2c442eed0aac21"),
    "Roboto-LICENSE.txt": (_R + "/LICENSE",
                           "c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4"),
    "MaterialIcons-Regular.ttf": (_M + "/font/MaterialIcons-Regular.ttf",
                                  "ef149f08bdd2ff09a4e2c8573476b7b0f3fbb15b623954ade59899e7175bedda"),
    "MaterialIcons-Regular.codepoints": (_M + "/font/MaterialIcons-Regular.codepoints",
                                         "530f25bf7b2d71c8e1da9476d53f9a9bb6b7e187bff69bb7128bb679b8194894"),
    "MaterialIcons-LICENSE.txt": (_M + "/LICENSE",
                                  "58d1e17ffe5109a7ae296caafcadfdbe6a7d176f0bc4ab01e12a689b0499d8bd"),
    "RobotoMono-Regular.ttf": (_RM + "/fonts/ttf/RobotoMono-Regular.ttf",
                               "af0bff7599c3df3831755c16e39b3c496df74b8c8d8a1161b14dc8461be17cb4"),
    "RobotoMono-OFL.txt": (_RM + "/OFL.txt",
                           "50ab8dd54680d3473f649c9db86fece88434d097c7834475c1c72d2f8c429215"),
}

# ASCII, Latin-1, and • … – — ‘ ’ “ ” € (° and × are already in Latin-1).
TEXT_RANGES = "0x20-0x7E,0xA0-0xFF,0x2022,0x2026,0x2013,0x2014,0x2018,0x2019,0x201C,0x201D,0x20AC"

# (font name, source, px, MD2 roles)
TEXT_FONTS = [
    ("md_font_roboto_regular_68", "Roboto-Regular.ttf", 68, "Headline 4 (34 dp)"),
    ("md_font_roboto_regular_48", "Roboto-Regular.ttf", 48, "Headline 5 (24 dp)"),
    ("md_font_roboto_medium_40", "Roboto-Medium.ttf", 40, "Headline 6 (20 dp)"),
    ("md_font_roboto_regular_32", "Roboto-Regular.ttf", 32, "Subtitle 1, Body 1 (16 dp)"),
    ("md_font_roboto_regular_28", "Roboto-Regular.ttf", 28, "Body 2 (14 dp)"),
    ("md_font_roboto_medium_28", "Roboto-Medium.ttf", 28, "Button (14 dp, uppercase)"),
    ("md_font_roboto_regular_24", "Roboto-Regular.ttf", 24, "Caption (12 dp)"),
    ("md_font_roboto_medium_24", "Roboto-Medium.ttf", 24, "Overline (12 dp, uppercase)"),
    # Not an MD2 role: code and terminal text (Editor, Terminal, Python output).
    ("md_font_roboto_mono_24", "RobotoMono-Regular.ttf", 24, "Code, terminal (12 dp)"),
]

# Not an MD2 role either: the standby clock, which fills half the screen.
# Only what shell_fmt_clock and SHELL_CLOCK_NOT_SET print ("14:30", "--:--").
CLOCK_FONTS = [
    ("md_font_roboto_clock_200", "Roboto-Regular.ttf", 200, "Standby clock (100 dp)"),
]
CLOCK_RANGES = "0x20,0x2D,0x30-0x3A"

ICON_FONTS = [
    ("md_font_icons_36", 36, "Icon 18 dp (dense, status)"),
    ("md_font_icons_48", 48, "Icon 24 dp (standard)"),
    ("md_font_icons_80", 80, "Icon 40 dp (launcher)"),
]

# Curated Material Icons subset. Names must exist in the codepoints file.
ICONS = [
    # navigation / actions
    "add", "arrow_back", "arrow_forward", "check", "close", "menu", "more_vert",
    "search", "settings", "expand_more", "expand_less", "chevron_left",
    "chevron_right", "refresh", "delete", "edit", "share", "save", "undo", "redo",
    "content_copy", "content_paste", "open_in_new", "fullscreen", "fullscreen_exit",
    "drag_handle", "tune", "home", "apps",
    # selection controls
    "check_circle", "check_box", "check_box_outline_blank", "radio_button_checked",
    "radio_button_unchecked", "visibility", "visibility_off",
    # status / system
    "info", "warning", "error", "notifications", "lock", "lock_open",
    "power_settings_new", "restart_alt", "bedtime", "system_update",
    "access_time", "alarm", "timer", "schedule", "calendar_today",
    # connectivity
    "wifi", "wifi_off", "wifi_lock", "bluetooth", "bluetooth_connected",
    "bluetooth_disabled", "usb", "cable", "cloud", "download", "upload", "nfc",
    # power
    "battery_full", "battery_std", "battery_charging_full", "battery_alert",
    "battery_saver",
    # display / sound
    "brightness_6", "brightness_low", "brightness_high", "light_mode", "dark_mode",
    "screen_rotation", "screen_lock_rotation", "volume_up", "volume_down",
    "volume_off", "speaker", "headphones",
    # media / capture
    "camera_alt", "mic", "mic_off", "videocam", "image", "music_note",
    "play_arrow", "pause", "stop", "skip_next", "skip_previous",
    # files / storage
    "folder", "insert_drive_file", "sd_storage", "storage",
    # hardware / developer
    "memory", "developer_board", "terminal", "sensors", "keyboard", "language",
    "explore", "thermostat", "account_circle",
    # shell and system apps (Phase 6)
    "eject", "history", "bolt", "block", "keyboard_hide", "backspace",
    "keyboard_return", "arrow_upward", "swap_horiz", "delete_forever",
    "extension", "dashboard", "palette", "translate", "code", "android",
    # media and USB (Phase 7)
    "mouse", "piano", "photo_library", "video_library", "fiber_manual_record",
    "cameraswitch", "photo_camera", "flash_on", "graphic_eq", "hub",
    "videogame_asset", "usb_off",
    # developer tools, updates and recovery (Phases 8 and 9)
    "note_add", "developer_mode", "security", "restore", "update", "link",
    "link_off", "folder_open", "healing", "verified_user", "keyboard_arrow_up",
    "keyboard_arrow_down", "keyboard_tab", "format_indent_increase", "bug_report",
    # weather (github.com/tab5os/weather)
    "wb_sunny", "nights_stay", "wb_cloudy", "foggy", "grain", "umbrella",
    "water_drop", "ac_unit", "thunderstorm", "air", "wb_twilight", "speed",
    "location_on",
]

# The 40 dp font is large (about 1.8 KB a glyph): only the icons the launcher
# and the system apps draw at that size. md_icon_font(40) falls back to 24 dp
# glyphs for anything else (see md_text.c).
ICONS_LARGE = [
    "apps", "settings", "folder", "extension", "battery_charging_full",
    "developer_board", "camera_alt", "terminal", "videocam", "usb", "sensors",
    "android", "dashboard", "photo_library", "image", "music_note", "mic",
    "insert_drive_file", "sd_storage", "warning", "wifi", "bluetooth",
    "memory", "cable", "keyboard", "mouse", "piano", "hub", "graphic_eq",
    "code", "system_update",
    "wb_sunny", "nights_stay", "wb_cloudy", "cloud", "foggy", "grain",
    "umbrella", "ac_unit", "thunderstorm",
]

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
CACHE = os.path.join(HERE, ".cache")
MD = os.path.join(REPO, "firmware", "components", "md")
OUT_FONTS = os.path.join(MD, "assets", "fonts")
OUT_LIC = os.path.join(MD, "assets", "LICENSES")
OUT_INC = os.path.join(MD, "include", "md")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(offline):
    os.makedirs(CACHE, exist_ok=True)
    gi = os.path.join(CACHE, ".gitignore")
    if not os.path.exists(gi):
        with open(gi, "w") as f:
            f.write("# Downloaded font sources (tools/assets/fonts.py). Not committed.\n*\n!.gitignore\n")
    for name, (url, want) in SOURCES.items():
        path = os.path.join(CACHE, name)
        if not (os.path.exists(path) and sha256(path) == want):
            if offline:
                sys.exit("missing or bad cache file %s (run without --offline)" % name)
            print("download", url)
            tmp = path + ".part"
            with urllib.request.urlopen(url) as r, open(tmp, "wb") as f:
                shutil.copyfileobj(r, f)
            got = sha256(tmp)
            if got != want:
                os.remove(tmp)
                sys.exit("sha256 mismatch for %s: got %s want %s" % (name, got, want))
            os.replace(tmp, path)


def load_codepoints():
    cps = {}
    with open(os.path.join(CACHE, "MaterialIcons-Regular.codepoints")) as f:
        for line in f:
            parts = line.split()
            if len(parts) == 2:
                cps[parts[0]] = int(parts[1], 16)
    return cps


def run_conv(src, size, ranges, name, out, extra=()):
    cmd = ["npx", "-y", LV_FONT_CONV,
           "--font", src, "-r", ranges,
           "--size", str(size), "--bpp", "4", "--no-compress", "--no-prefilter",
           "--format", "lvgl", "--lv-font-name", name, "-o", out] + list(extra)
    # Run from the cache dir so the recorded options hold no absolute paths.
    subprocess.run(cmd, cwd=CACHE, check=True, stdout=subprocess.DEVNULL)


def glyph_box_h(text, cp):
    """Height of the bounding box of glyph `cp` in a generated font."""
    m = re.search(r"/\* U\+%04X .*?\*/" % cp, text)
    if not m:
        return None
    # glyph_dsc entries: {.bitmap_index = N, .adv_w = N, .box_w = N, .box_h = N, ...}
    gid = None
    dsc = re.findall(r"\{\.bitmap_index = (\d+), \.adv_w = \d+, \.box_w = \d+, \.box_h = (\d+)", text)
    # Bitmap comments are in glyph order (id 1 onwards; id 0 is reserved).
    comments = re.findall(r"/\* U\+([0-9A-F]+) ", text.split("glyph_bitmap[]", 1)[1].split("};", 1)[0])
    for i, c in enumerate(comments):
        if int(c, 16) == cp:
            gid = i + 1
    if gid is None or gid >= len(dsc):
        return None
    return int(dsc[gid][1])


def patch(text, name, opts_line, cap_cp, x_cp):
    """Make lv_font_conv 1.5.3 output match the LVGL 9.6 built-in font format."""
    text = re.sub(r" \* Opts: .*\n", " * Opts: %s\n" % opts_line, text, count=1)
    text = text.replace(
        '#ifdef LV_LVGL_H_INCLUDE_SIMPLE\n#include "lvgl.h"\n#else\n#include "lvgl/lvgl.h"\n#endif\n',
        "/* Generated by tools/assets/fonts.py. Do not edit. */\n#include \"lvgl.h\"\n", 1)
    if '#include "lvgl.h"' not in text:
        raise SystemExit("unexpected lv_font_conv header in " + name)
    cap = glyph_box_h(text, cap_cp) if cap_cp else None
    xh = glyph_box_h(text, x_cp) if x_cp else None
    metrics = ""
    if cap is not None:
        metrics += "    .cap_height = %d,           /*Cap height of the font*/\n" % cap
    if xh is not None:
        metrics += "    .x_height = %d,             /*x-height of the font*/\n" % xh
    anchor = "#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)\n    .subpx"
    if anchor not in text:
        raise SystemExit("unexpected lv_font_conv footer in " + name)
    if metrics:
        text = text.replace(anchor, "#if LV_VERSION_CHECK(9, 6, 0) || LVGL_VERSION_MAJOR >= 10\n"
                            + metrics + "#endif\n" + anchor, 1)
    anchor = "    .dsc = &font_dsc,"
    text = text.replace(anchor, "#if LV_VERSION_CHECK(9, 3, 0) || LVGL_VERSION_MAJOR >= 10\n"
                        "    .static_bitmap = 1,    /*Bitmaps are const and uncompressed*/\n"
                        "#endif\n" + anchor, 1)
    # Drop the per-file enable guard: the md component always builds every font.
    guard = name.upper()
    text = text.replace("#ifndef %s\n#define %s 1\n#endif\n\n#if %s\n" % (guard, guard, guard), "", 1)
    text = text.replace("#endif /*#if %s*/\n" % guard, "", 1)
    if name == "md_font_icons_80":
        # Subset font: anything not in ICONS_LARGE draws from the 24 dp font.
        text = text.replace("    .fallback = NULL,", "    .fallback = &md_font_icons_48,", 1)
        text = text.replace('#include "lvgl.h"\n', '#include "lvgl.h"\n\nLV_FONT_DECLARE(md_font_icons_48)\n', 1)
    text = re.sub(r"\n{3,}", "\n\n", text).rstrip("\n") + "\n"
    return text


def build_font(name, src, size, ranges, opts_ranges, cap_cp=None, x_cp=None):
    out = os.path.join(OUT_FONTS, name + ".c")
    tmp = os.path.join(CACHE, name + ".c")
    run_conv(src, size, ranges, name, tmp)
    with open(tmp, encoding="utf-8") as f:
        text = f.read()
    os.remove(tmp)
    opts = "%s --font %s -r %s --size %d --bpp 4 --no-compress --no-prefilter --format lvgl" % (
        LV_FONT_CONV, src, opts_ranges, size)
    text = patch(text, name, opts, cap_cp, x_cp)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return out


def bitmap_bytes(path):
    """Approximate flash use: glyph bitmaps + 8 bytes per glyph dsc + kerning."""
    with open(path, encoding="utf-8") as f:
        text = f.read()
    total = 0
    for arr in re.findall(r"const (?:uint8_t|int8_t) \w+\[\] = \{(.*?)\};", text, re.S):
        total += len(re.findall(r"0x[0-9a-fA-F]+|-?\d+(?=\s*[,}])", re.sub(r"/\*.*?\*/", "", arr, flags=re.S)))
    total += 8 * len(re.findall(r"\{\.bitmap_index", text))
    return total


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def utf8_escape(cp):
    return "".join("\\x%02X" % b for b in chr(cp).encode("utf-8"))


def write_headers(icon_cps):
    rows = []
    for name, src, size, role in TEXT_FONTS:
        rows.append(" *   %-28s %-28s %3d px  %s" % (name, role, size, src.replace(".ttf", "")))
    for name, src, size, role in CLOCK_FONTS:
        rows.append(" *   %-28s %-28s %3d px  %s" % (name, role, size, src.replace(".ttf", "")))
    for name, size, role in ICON_FONTS:
        rows.append(" *   %-28s %-28s %3d px  MaterialIcons-Regular" % (name, role, size))
    decls = "\n".join("LV_FONT_DECLARE(%s)" % n for n, *_ in TEXT_FONTS + CLOCK_FONTS + ICON_FONTS)
    write(os.path.join(OUT_INC, "md_fonts.h"), """\
/* Generated by tools/assets/fonts.py. Do not edit. */
/*
 * Material Design 2 type scale as precompiled LVGL bitmap fonts (bpp 4).
 * Density is 2 px per dp. Line heights come from the md styles, not the fonts;
 * Button and Overline are uppercase by convention (the caller upper-cases).
 *
 *   Font                         MD2 role                     Size    Source
%s
 *
 * Text fonts cover ASCII 0x20-0x7E, Latin-1 0xA0-0xFF and the punctuation
 * U+2013 U+2014 U+2018 U+2019 U+201C U+201D U+2022 U+2026 U+20AC.
 * Icon fonts hold the glyphs listed in md/md_icons.h only.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

%s

#ifdef __cplusplus
}
#endif
""" % ("\n".join(rows), decls))

    lines = []
    for icon in ICONS:
        lines.append('#define MD_ICON_%-28s "%s" /* U+%04X */' % (icon.upper(), utf8_escape(icon_cps[icon]), icon_cps[icon]))
    write(os.path.join(OUT_INC, "md_icons.h"), """\
/* Generated by tools/assets/fonts.py. Do not edit. */
/*
 * Material Icons (Regular) subset as UTF-8 strings. Use with md_font_icons_36,
 * md_font_icons_48 or md_font_icons_80 from md/md_fonts.h, e.g.
 *     lv_label_set_text(label, MD_ICON_WIFI);
 * Icons not listed here are not in the fonts; add them to ICONS in fonts.py.
 */
#pragma once

%s
""" % "\n".join(lines))


def write_licenses():
    os.makedirs(OUT_LIC, exist_ok=True)
    shutil.copyfile(os.path.join(CACHE, "Roboto-LICENSE.txt"), os.path.join(OUT_LIC, "Roboto-LICENSE.txt"))
    shutil.copyfile(os.path.join(CACHE, "MaterialIcons-LICENSE.txt"), os.path.join(OUT_LIC, "MaterialIcons-LICENSE.txt"))
    shutil.copyfile(os.path.join(CACHE, "RobotoMono-OFL.txt"), os.path.join(OUT_LIC, "RobotoMono-OFL.txt"))
    write(os.path.join(OUT_LIC, "README.md"), """\
# Third-party font licences

Generated by `tools/assets/fonts.py`. These notices ship with Tab5 OS (About screen).

| Asset | Source | Version | Licence | File |
| --- | --- | --- | --- | --- |
| Roboto Regular, Roboto Medium (`md_font_roboto_*`) | [googlefonts/roboto](https://github.com/googlefonts/roboto) `src/hinted/` at commit `{rc}` | font version 2.001047 (repo release v2.138) | Apache License 2.0 | `Roboto-LICENSE.txt` |
| Material Icons Regular (`md_font_icons_*`) | [google/material-design-icons](https://github.com/google/material-design-icons) `font/MaterialIcons-Regular.ttf` at commit `{ic}` | font version 1.017 | Apache License 2.0 | `MaterialIcons-LICENSE.txt` |
| Roboto Mono Regular (`md_font_roboto_mono_24`) | [googlefonts/RobotoMono](https://github.com/googlefonts/RobotoMono) `fonts/ttf/RobotoMono-Regular.ttf` at commit `{mc}` | | SIL Open Font License 1.1 | `RobotoMono-OFL.txt` |

The fonts are subset and converted to LVGL bitmap fonts with
[lv_font_conv](https://github.com/lvgl/lv_font_conv) 1.5.3 (MIT). Roboto is a
trademark of Google; Tab5 OS is not a Google product.
""".format(rc=ROBOTO_COMMIT, ic=ICONS_COMMIT, mc=MONO_COMMIT))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--offline", action="store_true", help="use the cache only")
    args = ap.parse_args()
    fetch(args.offline)
    cps = load_codepoints()
    missing = [i for i in ICONS if i not in cps]
    if missing:
        sys.exit("icons missing from codepoints: " + ", ".join(missing))
    if len(set(ICONS)) != len(ICONS):
        sys.exit("duplicate icon names")

    os.makedirs(OUT_FONTS, exist_ok=True)
    os.makedirs(OUT_INC, exist_ok=True)
    wanted = {n + ".c" for n, *_ in TEXT_FONTS + CLOCK_FONTS + ICON_FONTS}
    for old in os.listdir(OUT_FONTS):
        if old.endswith(".c") and old not in wanted:
            os.remove(os.path.join(OUT_FONTS, old))

    sizes = []
    for name, src, size, _ in TEXT_FONTS:
        out = build_font(name, src, size, TEXT_RANGES, TEXT_RANGES, cap_cp=ord("H"), x_cp=ord("x"))
        sizes.append((name, bitmap_bytes(out)))
    for name, src, size, _ in CLOCK_FONTS:
        out = build_font(name, src, size, CLOCK_RANGES, CLOCK_RANGES)
        sizes.append((name, bitmap_bytes(out)))
    bad = [i for i in ICONS_LARGE if i not in ICONS]
    if bad:
        sys.exit("ICONS_LARGE not in ICONS: " + ", ".join(bad))
    for name, size, _ in ICON_FONTS:
        subset = ICONS_LARGE if size >= 80 else ICONS
        icon_ranges = ",".join("0x%X" % c for c in sorted(cps[i] for i in subset))
        out = build_font(name, "MaterialIcons-Regular.ttf", size, icon_ranges,
                         "<%d icons from md_icons.h>" % len(subset))
        sizes.append((name, bitmap_bytes(out)))

    write_headers(cps)
    write_licenses()
    total = 0
    for name, b in sizes:
        print("%-28s %8d bytes" % (name, b))
        total += b
    print("%-28s %8d bytes (%.1f KiB, glyph data approx.)" % ("total", total, total / 1024))


if __name__ == "__main__":
    main()
