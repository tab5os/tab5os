#!/usr/bin/env python3
"""Deflate the big arrays in the generated MD2 assets for the firmware image.

usage: zpack.py OUT_DIR ASSET.c...

The font and shadow sources under firmware/components/md/assets/ hold about
850 KB of glyph bitmaps and shadow masks. The firmware build runs this script
on them: each output file keeps everything except the big array, which
becomes a zlib stream, and the descriptor that pointed at it gets NULL. The
md component inflates every stream into PSRAM once, in md_init()
(md_zasset.c), and points the descriptors at the copies. Rendering is
unchanged: with SPIRAM_XIP_FROM_PSRAM the original arrays were in PSRAM too.

OUT_DIR also gets md_zassets_table.c, the list md_init() walks.

The simulator builds the original files; it never sees this.
Deterministic: same inputs, same bytes.
"""
import os
import re
import sys
import zlib

ARRAY = re.compile(
    r"static\s+(?:LV_ATTRIBUTE_MEM_ALIGN\s+)?(?:LV_ATTRIBUTE_LARGE_CONST\s+)?const\s+uint8_t\s+"
    r"(?P<name>\w+)\[\]\s*=\s*\{(?P<body>.*?)\};",
    re.S,
)
NUM = re.compile(r"0x[0-9a-fA-F]+|\b\d+\b")


def c_bytes(data, indent="    ", per_line=24):
    lines = []
    for i in range(0, len(data), per_line):
        lines.append(indent + ",".join("0x%02x" % b for b in data[i:i + per_line]) + ",")
    return "\n".join(lines)


def array_bytes(body):
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)
    vals = [int(x, 0) for x in NUM.findall(body)]
    if any(v > 255 for v in vals):
        raise ValueError("array value over 255")
    return bytes(vals)


def pack_font(src, sym):
    m = ARRAY.search(src)
    if not m or m.group("name") != "glyph_bitmap":
        raise ValueError("no glyph_bitmap array")
    raw = array_bytes(m.group("body"))
    z = zlib.compress(raw, 9)
    zname = "md_z_%s" % sym
    out = src[:m.start()] + "static const uint8_t %s[] = {\n%s\n};" % (zname, c_bytes(z)) + src[m.end():]
    # The descriptor gets the inflated copy at md_init(), so it must be writable.
    out, n = re.subn(r"static const lv_font_fmt_txt_dsc_t font_dsc", "static lv_font_fmt_txt_dsc_t font_dsc", out)
    if n != 1:
        raise ValueError("font_dsc not found")
    out, n = re.subn(r"\.glyph_bitmap = glyph_bitmap,", ".glyph_bitmap = NULL,", out)
    if n != 1:
        raise ValueError(".glyph_bitmap not found")
    out += (
        "\n#include \"md_zasset.h\"\n"
        "const md_zasset_t md_za_%s = { %s, sizeof %s, %du, (const void **)&font_dsc.glyph_bitmap };\n"
        % (sym, zname, zname, len(raw))
    )
    return out, len(raw), len(z)


def pack_shadow(src, sym):
    m = ARRAY.search(src)
    if not m:
        raise ValueError("no shadow map array")
    name = m.group("name")
    raw = array_bytes(m.group("body"))
    z = zlib.compress(raw, 9)
    zname = "md_z_%s" % sym
    out = src[:m.start()] + "static const uint8_t %s[] = {\n%s\n};" % (zname, c_bytes(z)) + src[m.end():]
    dsc = name[:-len("_map")] if name.endswith("_map") else None
    out, n = re.subn(r"const lv_image_dsc_t %s = \{" % re.escape(dsc), "lv_image_dsc_t %s = {" % dsc, out)
    if n != 1:
        raise ValueError("image descriptor %s not found" % dsc)
    out, n = re.subn(r"\.data_size = sizeof\(%s\)," % re.escape(name), ".data_size = %du," % len(raw), out)
    if n != 1:
        raise ValueError(".data_size not found")
    out, n = re.subn(r"\.data = %s," % re.escape(name), ".data = NULL,", out)
    if n != 1:
        raise ValueError(".data not found")
    out += (
        "\n#include \"md_zasset.h\"\n"
        "const md_zasset_t md_za_%s = { %s, sizeof %s, %du, (const void **)&%s.data };\n"
        % (sym, zname, zname, len(raw), dsc)
    )
    return out, len(raw), len(z)


def main(argv):
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    out_dir = argv[1]
    os.makedirs(out_dir, exist_ok=True)
    syms = []
    total_raw = total_z = 0
    for path in argv[2:]:
        base = os.path.splitext(os.path.basename(path))[0]
        with open(path, encoding="utf-8") as f:
            src = f.read()
        if base == "md_shadows":
            # The lookup table: its externs must match the now-writable descriptors.
            out = src.replace("extern const lv_image_dsc_t", "extern lv_image_dsc_t")
        elif base.startswith("md_font_"):
            out, r, z = pack_font(src, base)
            syms.append(base)
            total_raw, total_z = total_raw + r, total_z + z
        elif base.startswith("md_shadow_"):
            out, r, z = pack_shadow(src, base)
            syms.append(base)
            total_raw, total_z = total_raw + r, total_z + z
        else:
            out = src
        dst = os.path.join(out_dir, os.path.basename(path))
        write_if_changed(dst, "/* zpack.py output, from %s. Do not edit. */\n" % os.path.basename(path) + out)
    table = ["/* zpack.py output. Do not edit. */", "#include <stddef.h>", "#include \"md_zasset.h\"", ""]
    table += ["extern const md_zasset_t md_za_%s;" % s for s in syms]
    table += ["", "const md_zasset_t *const md_zassets[] = {"]
    table += ["    &md_za_%s," % s for s in syms]
    table += ["    NULL,", "};", "/* %d assets: %d bytes inflated, %d deflated */" % (len(syms), total_raw, total_z), ""]
    write_if_changed(os.path.join(out_dir, "md_zassets_table.c"), "\n".join(table))
    return 0


def write_if_changed(path, text):
    try:
        with open(path, encoding="utf-8") as f:
            if f.read() == text:
                return
    except FileNotFoundError:
        pass
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
