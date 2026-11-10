#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Draw the example apps' launcher icons (apps/examples/<app>/icon.png).

Each icon is 112 px square, the size the launcher draws it (56 dp at 2 px
per dp), so the device shows it pixel for pixel: a deep two-tone gradient in
the app's hue with a light Material glyph on it. The launcher crops icons to
a circle, so the corners don't matter. The deep 800 to 900 tones keep a row
of them in the same family as the shell's own tonal badges rather than
shouting over them.

The glyphs come from the 80 px Material Icons subset already compiled into
the firmware (md_font_icons_80.c, 4 bits per pixel), so this needs nothing
beyond the standard library. Calculator draws its own glyph (+, -, x, =),
as the subset has none.

    python3 tools/assets/app_icons.py
"""
import argparse
import os
import re
import struct
import sys
import zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FONT = os.path.join(ROOT, "firmware", "components", "md", "assets", "fonts", "md_font_icons_80.c")
ICONS_H = os.path.join(ROOT, "firmware", "components", "md", "include", "md", "md_icons.h")
EXAMPLES = os.path.join(ROOT, "apps", "examples")

SIZE = 112   # px: 56 dp
EM = 80      # the font's em square, px

# app dir: (glyph, Material palette 800, 900 and 100)
APPS = {
    "ble_explorer": ("BLUETOOTH", 0x283593, 0x1A237E, 0xC5CAE9),     # indigo
    "calculator": (None, 0xD84315, 0xBF360C, 0xFFCCBC),              # deep orange
    "logger": ("SENSORS", 0x2E7D32, 0x1B5E20, 0xC8E6C9),             # green
    "music": ("MUSIC_NOTE", 0xAD1457, 0x880E4F, 0xF8BBD0),           # pink
    "rs485_console": ("CABLE", 0x00695C, 0x004D40, 0xB2DFDB),        # teal
    "ui_gallery": ("DASHBOARD", 0x4527A0, 0x311B92, 0xD1C4E9),       # deep purple
    "uvc_monitor": ("VIDEOCAM", 0xC62828, 0xB71C1C, 0xFFCDD2),       # red
    "weather": ("WB_SUNNY", 0x0277BD, 0x01579B, 0xB3E5FC),           # light blue
}


def load_font():
    """{codepoint: (box_w, box_h, ofs_x, ofs_y, coverage rows 0..1)} from the LVGL font source."""
    src = open(FONT).read()
    body = src.split("glyph_bitmap[] = {", 1)[1].split("};", 1)[0]
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    bitmap = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)", body))
    dsc = [tuple(int(v) for v in m) for m in re.findall(
        r"\.bitmap_index = (\d+), \.adv_w = \d+, \.box_w = (\d+), \.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)", src)]
    start = int(re.search(r"\.range_start = (\d+)", src).group(1))
    first_id = int(re.search(r"\.glyph_id_start = (\d+)", src).group(1))
    ulist = src.split("unicode_list_0[] = {", 1)[1].split("};", 1)[0]
    offsets = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)", ulist)]
    glyphs = {}
    for i, off in enumerate(offsets):
        idx, w, h, ox, oy = dsc[first_id + i]
        nib = []
        for b in bitmap[idx:idx + (w * h + 1) // 2]:
            nib += [b >> 4, b & 15]
        cov = [[nib[y * w + x] / 15 for x in range(w)] for y in range(h)]
        glyphs[start + off] = (w, h, ox, oy, cov)
    return glyphs


def codepoint(name):
    m = re.search(rf'#define MD_ICON_{name}\s+"([^"]+)"', open(ICONS_H).read())
    raw = m.group(1).encode().decode("unicode_escape").encode("latin-1").decode("utf-8")
    return ord(raw)


def glyph_mask(glyphs, name):
    """The glyph's coverage over the em square, EM x EM."""
    w, h, ox, oy, cov = glyphs[codepoint(name)]
    mask = [[0.0] * EM for _ in range(EM)]
    top = EM - (oy + h)   # boxes sit on the baseline at the em square's bottom
    for y in range(h):
        for x in range(w):
            if 0 <= top + y < EM and 0 <= ox + x < EM:
                mask[top + y][ox + x] = cov[y][x]
    return mask


def calculator_mask():
    """+ and - over x and =, drawn with 4x4 supersampling, in Material's
    proportions: 4 px strokes on a 24 px grid, scaled to the em square."""
    k = EM / 24
    bars = []   # (x0, y0, x1, y1) rectangles in 24 px units, then rotated ones
    def plus(cx, cy):
        bars.extend([(cx - 3.5, cy - 0.9, cx + 3.5, cy + 0.9), (cx - 0.9, cy - 3.5, cx + 0.9, cy + 3.5)])
    def minus(cx, cy):
        bars.append((cx - 3.5, cy - 0.9, cx + 3.5, cy + 0.9))
    def equals(cx, cy):
        bars.extend([(cx - 3.5, cy - 2.6, cx + 3.5, cy - 0.8), (cx - 3.5, cy + 0.8, cx + 3.5, cy + 2.6)])
    plus(7.5, 7.5)
    minus(16.5, 7.5)
    equals(16.5, 16.5)
    times = (7.5, 16.5)

    def inside(px, py):
        u, v = px / k, py / k
        for x0, y0, x1, y1 in bars:
            if x0 <= u <= x1 and y0 <= v <= y1:
                return True
        # x: the plus bars turned 45 degrees about their centre
        du, dv = u - times[0], v - times[1]
        ru, rv = (du + dv) * 0.7071, (dv - du) * 0.7071
        return (abs(ru) <= 3.5 and abs(rv) <= 0.9) or (abs(rv) <= 3.5 and abs(ru) <= 0.9)

    ss = 4
    mask = [[0.0] * EM for _ in range(EM)]
    for y in range(EM):
        for x in range(EM):
            n = sum(inside(x + (i + 0.5) / ss, y + (j + 0.5) / ss) for i in range(ss) for j in range(ss))
            mask[y][x] = n / (ss * ss)
    return mask


def rgb(c):
    return (c >> 16) & 255, (c >> 8) & 255, c & 255


def mix(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def draw(mask, top, bottom, ink):
    """RGB rows: a top-to-bottom gradient with the glyph centred over it."""
    top, bottom, ink = rgb(top), rgb(bottom), rgb(ink)
    pad = (SIZE - EM) // 2
    rows = []
    for y in range(SIZE):
        bg = mix(top, bottom, y / (SIZE - 1))
        row = []
        for x in range(SIZE):
            gy, gx = y - pad, x - pad
            a = mask[gy][gx] if 0 <= gy < EM and 0 <= gx < EM else 0.0
            row.append(mix(bg, ink, a))
        rows.append(row)
    return rows


def png(rows):
    """An 8-bit RGB PNG, no filtering (flat colour compresses well as is)."""
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + bytes(c for px in row for c in px) for row in rows)
    ihdr = struct.pack(">IIBBBBB", len(rows[0]), len(rows), 8, 2, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def main():
    argparse.ArgumentParser(description=__doc__.split("\n\n")[0]).parse_args()
    glyphs = load_font()
    for app, (glyph, top, bottom, ink) in APPS.items():
        mask = calculator_mask() if glyph is None else glyph_mask(glyphs, glyph)
        data = png(draw(mask, top, bottom, ink))
        path = os.path.join(EXAMPLES, app, "icon.png")
        with open(path, "wb") as f:
            f.write(data)
        print(f"wrote {os.path.relpath(path, ROOT)} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
