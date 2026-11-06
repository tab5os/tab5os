#!/usr/bin/env python3
"""Bake Material Design 2 elevation shadows into LVGL 9 nine-slice images.

Each MD2 shadow is three black CSS box-shadow layers (umbra, penumbra,
ambient). For every elevation and shape this script renders those layers
around a box exactly as CSS would (offset, spread with CSS radius rules,
Gaussian blur with sigma = blur / 2), composites them, cuts out the box
interior (keeping a 1 px overlap under the surface edge), trims the result to
a nine-slice and optionally applies 4x4 Bayer dithering to the alpha so the
gradient does not band on RGB565 framebuffers.

Outputs (deterministic):
  firmware/components/md/assets/shadows/md_shadow_*.c   lv_image_dsc_t, ARGB8888
  firmware/components/md/assets/shadows/md_shadows.c    lookup table + md_shadow_get()
  firmware/components/md/include/md/md_shadows.h        md_shadow_t API

Usage:
  python3 tools/assets/shadows.py                    regenerate C sources
  python3 tools/assets/shadows.py --preview DIR      also write per-shadow PNGs
  python3 tools/assets/shadows.py --no-dither        disable alpha dithering
  python3 tools/assets/shadows.py --verify-mdc       re-download the MDC scss and check the tables

Stdlib only. Density is fixed at 2 px per dp.
"""

import argparse
import hashlib
import math
import os
import re
import struct
import sys
import urllib.request
import zlib

DP = 2  # px per dp

# Material Components Web @material/elevation 14.0.0, _elevation-theme.scss
# https://unpkg.com/@material/elevation@14.0.0/_elevation-theme.scss
# (sha256 3530bcbde901f0387800a1078e20dd267855ced12cad3c36bbfe1719e0ebd2cd)
# Values are "x y blur spread" in dp (CSS px at 1x).
MDC_VERSION = "14.0.0"
MDC_URL = "https://unpkg.com/@material/elevation@%s/_elevation-theme.scss" % MDC_VERSION
MDC_SHA256 = "3530bcbde901f0387800a1078e20dd267855ced12cad3c36bbfe1719e0ebd2cd"
UMBRA_OPACITY = 0.2
PENUMBRA_OPACITY = 0.14
AMBIENT_OPACITY = 0.12
UMBRA = {1: (0, 2, 1, -1), 2: (0, 3, 1, -2), 4: (0, 2, 4, -1), 6: (0, 3, 5, -1),
         8: (0, 5, 5, -3), 16: (0, 8, 10, -5), 24: (0, 11, 15, -7)}
PENUMBRA = {1: (0, 1, 1, 0), 2: (0, 2, 2, 0), 4: (0, 4, 5, 0), 6: (0, 6, 10, 0),
            8: (0, 8, 10, 1), 16: (0, 16, 24, 2), 24: (0, 24, 38, 3)}
AMBIENT = {1: (0, 1, 3, 0), 2: (0, 1, 5, 0), 4: (0, 1, 10, 0), 6: (0, 1, 18, 0),
           8: (0, 3, 14, 2), 16: (0, 6, 30, 5), 24: (0, 9, 46, 8)}

ELEVATIONS = [1, 2, 4, 6, 8, 16, 24]

# Shapes: (key, corner radius dp or None for fully round, round diameter dp, elevations)
# Flash budget: square surfaces reuse the 4 dp set (the corner difference is
# lost in a 2 px blur of the dark theme's few shadows), and FABs only bake
# their resting elevation (raised and pressed map to it).
SHAPES = [
    ("r4", 4, None, ELEVATIONS),     # cards, menus, dialogs, snackbars; also square surfaces
    ("round40", None, 40, [6]),      # mini FAB
    ("round56", None, 56, [6]),      # FAB
]

# Nine-slice centre width/height. With dithering the centre is a 4 px tile
# (one Bayer period) that must be tiled, not scaled; without dithering 1 px.
TILE_DITHER = 4

CF = {"argb8888": "LV_COLOR_FORMAT_ARGB8888", "a8": "LV_COLOR_FORMAT_A8"}
BPP = {"argb8888": 4, "a8": 1}

BAYER4 = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
MD = os.path.join(REPO, "firmware", "components", "md")
OUT_DIR = os.path.join(MD, "assets", "shadows")
OUT_INC = os.path.join(MD, "include", "md")
PREVIEW_PNG = os.path.join(HERE, "preview", "shadows.png")


# ---------------------------------------------------------------- rendering

def css_spread_radius(r, s):
    """Corner radius of a box-shadow grown by spread s (CSS Backgrounds 3, 7.1.1)."""
    if r <= 0:
        return 0.0
    if s < 0:
        return max(r + s, 0.0)
    if s == 0 or r >= s:
        return r + s
    return r + s * (1 + (r / s - 1) ** 3)


def rrect_coverage(w, h, x0, y0, x1, y1, r, ss=8):
    """Anti-aliased coverage of rounded rect [x0,x1)x[y0,y1) radius r, as rows.

    Edges lie on integer pixel boundaries, so only the corners need sampling.
    """
    cols_cache = {}
    rows = [[0.0] * w for _ in range(h)]
    r = min(r, (x1 - x0) / 2.0, (y1 - y0) / 2.0)
    for y in range(max(0, y0), min(h, y1)):
        row = rows[y]
        for x in range(max(0, x0), min(w, x1)):
            row[x] = 1.0
    if r <= 0:
        return rows
    rc = int(math.ceil(r))
    corners = [(x0 + r, y0 + r, x0, y0, 1, 1), (x1 - r, y0 + r, x1 - 1, y0, -1, 1),
               (x0 + r, y1 - r, x0, y1 - 1, 1, -1), (x1 - r, y1 - r, x1 - 1, y1 - 1, -1, -1)]
    for cx, cy, px, py, dx, dy in corners:
        for j in range(rc):
            for i in range(rc):
                x = px + dx * i
                y = py + dy * j
                key = (i, j)
                if key not in cols_cache:
                    # sample the top-left corner in local coords (pixel i,j from the corner)
                    inside = 0
                    for sy in range(ss):
                        fy = j + (sy + 0.5) / ss
                        for sx in range(ss):
                            fx = i + (sx + 0.5) / ss
                            ddx = r - fx
                            ddy = r - fy
                            if ddx <= 0 or ddy <= 0 or ddx * ddx + ddy * ddy <= r * r:
                                inside += 1
                    cols_cache[key] = inside / float(ss * ss)
                if 0 <= x < w and 0 <= y < h:
                    rows[y][x] = cols_cache[key]
    return rows


def gauss_cdf_table(sigma, n):
    """G[t + n] = integral of the unit Gaussian up to t + 0.5, for t in [-n, n]."""
    if sigma <= 0:
        return [1.0 if t >= 0 else 0.0 for t in range(-n, n + 1)]
    k = 1.0 / (sigma * math.sqrt(2.0))
    return [0.5 * (1.0 + math.erf((t + 0.5) * k)) for t in range(-n, n + 1)]


def blur_sparse(vec, G, n, out_len):
    """Convolve a mostly piecewise-constant vector with a Gaussian (via its CDF)."""
    acc = [0.0] * out_len
    prev = 0.0
    for k, v in enumerate(vec):
        d = v - prev
        prev = v
        if d > 1e-12 or d < -1e-12:
            # out[y] += d * G[y - k + n]
            start = n - k
            seg = G[start:start + out_len]
            acc = [a + d * g for a, g in zip(acc, seg)]
    return acc


def blur2d(rows, sigma):
    h = len(rows)
    w = len(rows[0])
    n = max(w, h) + 2
    G = gauss_cdf_table(sigma, n)
    # vertical pass per distinct column
    cache = {}
    cols = []
    for x in range(w):
        col = tuple(rows[y][x] for y in range(h))
        if col not in cache:
            cache[col] = blur_sparse(col, G, n, h)
        cols.append(cache[col])
    # horizontal pass per row
    out = []
    for y in range(h):
        row = [c[y] for c in cols]
        out.append(blur_sparse(row, G, n, w))
    return out


def render_shadow(elev, radius_px, box_w, box_h, margin):
    """Full composited shadow alpha (0..1) on a canvas with the box at (margin, margin)."""
    w = box_w + 2 * margin
    h = box_h + 2 * margin
    total = [[1.0] * w for _ in range(h)]  # running product of (1 - a_l)
    for table, opacity in ((UMBRA, UMBRA_OPACITY), (PENUMBRA, PENUMBRA_OPACITY), (AMBIENT, AMBIENT_OPACITY)):
        ox, oy, blur, spread = (v * DP for v in table[elev])
        sigma = blur / 2.0
        rl = css_spread_radius(radius_px, spread)
        x0, y0 = margin - spread + ox, margin - spread + oy
        x1, y1 = margin + box_w + spread + ox, margin + box_h + spread + oy
        mask = rrect_coverage(w, h, x0, y0, x1, y1, rl)
        b = blur2d(mask, sigma)
        for y in range(h):
            t = total[y]
            by = b[y]
            total[y] = [tv * (1.0 - opacity * min(1.0, max(0.0, bv))) for tv, bv in zip(t, by)]
    alpha = [[1.0 - v for v in row] for row in total]
    # Cut out the box interior, keeping a 1 px overlap under the surface edge.
    inner = rrect_coverage(w, h, margin + 1, margin + 1, margin + box_w - 1, margin + box_h - 1,
                           max(radius_px - 1, 0))
    return [[a * (1.0 - c) for a, c in zip(ar, cr)] for ar, cr in zip(alpha, inner)]


def layer_extent(elev):
    """Generous half-extent (px) of the shadow beyond the box and its blur reach."""
    ext = 0
    reach = 0
    for table in (UMBRA, PENUMBRA, AMBIENT):
        ox, oy, blur, spread = (v * DP for v in table[elev])
        sigma = blur / 2.0
        ext = max(ext, abs(ox) + abs(oy) + max(spread, 0) + int(math.ceil(3.5 * sigma)) + 2)
        reach = max(reach, max(spread, 0) + int(math.ceil(3.5 * sigma)) + 2)
    return ext, reach


def bake(elev, radius_px, round_d, cutoff, tol, tile):
    """Return (alpha rows, meta) for one nine-slice."""
    ext, reach = layer_extent(elev)
    if round_d is not None:
        d = round_d * DP
        box_w = box_h = d
        r = d / 2.0
        inner = d // 2
    else:
        r = radius_px
        inner = int(math.ceil(r)) + reach
        box_w = box_h = 2 * inner + 1
    margin = ext
    a = render_shadow(elev, r, box_w, box_h, margin)
    h = len(a)
    w = len(a[0])
    bx = by = margin
    xc = bx + inner  # stretch column / row in the full render
    yc = by + inner

    # Outer crop: drop edge rows/cols that are below the cutoff everywhere.
    def col_max(x):
        return max(a[y][x] for y in range(h))

    def row_max(y):
        return max(a[y])

    left = 0
    while left < bx and col_max(left) < cutoff:
        left += 1
    right = w - 1
    while right > bx + box_w - 1 and col_max(right) < cutoff:
        right -= 1
    top = 0
    while top < by and row_max(top) < cutoff:
        top += 1
    bottom = h - 1
    while bottom > by + box_h - 1 and row_max(bottom) < cutoff:
        bottom -= 1

    if round_d is not None:
        xs = list(range(left, right + 1))
        ys = list(range(top, bottom + 1))
        cl = xc - left
        ct = yc - top
        cw = ch = 1
    else:
        # Inner crop: the columns between the corner and the centre that match
        # the centre column within `tol` are replaced by the stretch tile.
        ri = int(math.ceil(r))
        centre_col = [a[y][xc] for y in range(h)]
        xa = xc
        while xa - 1 > bx + ri and max(abs(a[y][xa - 1] - centre_col[y]) for y in range(h)) < tol:
            xa -= 1
        centre_row = a[yc]
        ya = yc
        while ya - 1 > by + ri and max(abs(u - v) for u, v in zip(a[ya - 1], centre_row)) < tol:
            ya -= 1
        yb = yc
        while yb + 1 < by + box_h - 1 - ri and max(abs(u - v) for u, v in zip(a[yb + 1], centre_row)) < tol:
            yb += 1
        cw = ch = tile
        # Centre tile: copies of the centre column/row (identical within tol).
        xs = list(range(left, xa)) + [xc] * cw + list(range(2 * xc - xa + 1, right + 1))
        ys = list(range(top, ya)) + [yc] * ch + list(range(yb + 1, bottom + 1))
        cl = xa - left
        ct = ya - top
    img = [[a[y][x] for x in xs] for y in ys]
    iw, ih = len(xs), len(ys)
    meta = {
        "w": iw, "h": ih,
        "left": cl, "top": ct, "right": iw - cl - cw, "bottom": ih - ct - ch,
        "box_x": bx - left, "box_y": by - top,
        "radius_px": int(round(r)) if round_d is None else 0x7FFF,
    }
    if round_d is not None:
        meta["box_min_w"], meta["box_min_h"] = box_w, box_h
    else:
        # Smallest box the nine-slice can draw: the parts of the corner slices
        # that lie inside the box, plus the centre tile.
        margin_r = right - (bx + box_w - 1)
        margin_b = bottom - (by + box_h - 1)
        meta["box_min_w"] = (cl - meta["box_x"]) + cw + (meta["right"] - margin_r)
        meta["box_min_h"] = (ct - meta["box_y"]) + ch + (meta["bottom"] - margin_b)
    return img, meta


def quantize(img, dither, levels=31):
    out = []
    for y, row in enumerate(img):
        b = BAYER4[y & 3]
        q = []
        for x, a in enumerate(row):
            a = min(1.0, max(0.0, a))
            if dither:
                v = a * levels
                base = int(v)
                if v - base > (b[x & 3] + 0.5) / 16.0:
                    base += 1
                q.append(int(round(min(base, levels) * 255.0 / levels)))
            else:
                q.append(int(round(a * 255.0)))
        out.append(q)
    return out


# ---------------------------------------------------------------- emitters

def write(path, text, binary=False):
    with open(path, "wb" if binary else "w", **({} if binary else {"encoding": "utf-8", "newline": "\n"})) as f:
        f.write(text)


def emit_image(name, alpha8, meta, desc, fmt):
    w, h = meta["w"], meta["h"]
    data = bytearray()
    for row in alpha8:
        if fmt == "a8":
            data += bytes(row)
        else:
            for a in row:
                data += bytes((0, 0, 0, a))  # B, G, R, A
    lines = []
    for i in range(0, len(data), 32):
        lines.append("    " + ",".join("0x%02x" % b for b in data[i:i + 32]) + ",")
    text = """\
/* Generated by tools/assets/shadows.py. Do not edit. */
/* %s */
#include "lvgl.h"

static LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST const uint8_t %s_map[] = {
%s
};

const lv_image_dsc_t %s = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = %s,
        .flags = 0,
        .w = %d,
        .h = %d,
        .stride = %d,
    },
    .data_size = sizeof(%s_map),
    .data = %s_map,
};
""" % (desc, name, "\n".join(lines), name, CF[fmt], w, h, w * BPP[fmt], name, name)
    write(os.path.join(OUT_DIR, name + ".c"), text)
    return len(data)


def emit_table(entries, dither, tile, fmt):
    decls = "\n".join("extern const lv_image_dsc_t %s;" % e["name"] for e in entries)
    rows = []
    for e in entries:
        m = e["meta"]
        rows.append("    { %-4s%-5s { &%s, %d, %d, %d, %d, %d, %d, %d, %d, %d } }," % (
            str(e["elev"]) + ",", str(e["shape"]) + ",", e["name"], m["left"], m["top"], m["right"], m["bottom"],
            m["box_x"], m["box_y"], m["radius_px"], m["box_min_w"], m["box_min_h"]))
    write(os.path.join(OUT_DIR, "md_shadows.c"), """\
/* Generated by tools/assets/shadows.py. Do not edit. */
#include <stdlib.h>
#include "md/md_shadows.h"

%s

typedef struct {
    int16_t elevation_dp;
    int16_t shape; /* corner radius dp, or -diameter dp for round */
    md_shadow_t shadow;
} md_shadow_entry_t;

static const md_shadow_entry_t s_shadows[] = {
%s
};

#define SHADOW_COUNT ((int)(sizeof(s_shadows) / sizeof(s_shadows[0])))

static int shape_for_radius(int radius_dp)
{
    if (radius_dp < 12) return 4;    /* square surfaces share the 4 dp set */
    if (radius_dp <= 20) return -40;
    return -56; /* includes LV_RADIUS_CIRCLE */
}

const md_shadow_t *md_shadow_get(int elevation_dp, int radius_dp)
{
    if (elevation_dp <= 0) return NULL;
    int shape = shape_for_radius(radius_dp);
    const md_shadow_t *best = NULL;
    int best_d = 0x7fff;
    for (int i = 0; i < SHADOW_COUNT; i++) {
        if (s_shadows[i].shape != shape) continue;
        int d = abs(s_shadows[i].elevation_dp - elevation_dp);
        /* ties go to the lower elevation */
        if (d < best_d) {
            best_d = d;
            best = &s_shadows[i].shadow;
        }
    }
    return best;
}
""" % (decls, "\n".join(rows)))

    elev_list = ", ".join(str(e) for e in ELEVATIONS)
    write(os.path.join(OUT_INC, "md_shadows.h"), """\
/* Generated by tools/assets/shadows.py. Do not edit. */
/*
 * Baked Material Design 2 elevation shadows: the MDC Web @material/elevation
 * %s umbra, penumbra and ambient layers, composited into nine-slice images.
 * Format %s, 2 px per dp.
 * Baked: elevations %s dp with 0 dp (square) and 4 dp
 * corners; elevations 6 and 8 dp for round 40 dp and 56 dp FABs.
 *
 * To draw the shadow of a surface whose box is (x, y, w, h) px on screen:
 *   - Draw the shadow first, then the surface. The box interior is
 *     transparent (cut out, keeping a 1 px overlap under the surface edge).
 *   - Place the image at (x - box_x, y - box_y). The drawn size is
 *     (w + img_w - box_min_w) x (h + img_h - box_min_h).
 *   - Draw the four corner slices unscaled. Fill the extra width with the
 *     centre column band [left, img_w - right) and the extra height with the
 *     centre row band [top, img_h - bottom). %s
 *     A band can be its own lv_image_dsc_t sharing this pixel data: point
 *     .data at the band's first pixel and keep the same stride.
%s *   - w and h must be at least box_min_w and box_min_h. Round shadows are
 *     for a box of exactly box_min_w x box_min_h and are drawn unscaled.
 */
#pragma once

#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const lv_image_dsc_t *img;
    int16_t left, top, right, bottom; /* slice insets px */
    int16_t box_x, box_y;             /* box origin inside img */
    int16_t radius_px;                /* corner radius px, LV_RADIUS_CIRCLE (0x7FFF) for round */
    int16_t box_min_w, box_min_h;     /* smallest box the nine-slice can draw, px */
} md_shadow_t;

/* Width and height of the centre band of every rectangular shadow, px. */
#define MD_SHADOW_CENTRE_PX %d

/*
 * Nearest baked shadow for an elevation (dp) and corner radius (dp).
 * radius_dp 0..11 -> 4 dp corners (square surfaces too), 12..20 -> round 40 dp (mini FAB),
 * > 20 (including LV_RADIUS_CIRCLE) -> round 56 dp (FAB). Elevation ties go to
 * the lower baked level. Returns NULL for elevation <= 0.
 */
const md_shadow_t *md_shadow_get(int elevation_dp, int radius_dp);

#ifdef __cplusplus
}
#endif
""" % (MDC_VERSION, ("ARGB8888 (black; alpha 4x4 Bayer dithered to 31 levels for RGB565)" if dither else "ARGB8888 (black)") if fmt == "argb8888"
       else ("A8 (alpha mask, 4x4 Bayer dithered to 31 levels for RGB565)" if dither else "A8 (alpha mask)"), elev_list,
       ("The bands are\n *     MD_SHADOW_CENTRE_PX wide and carry a 4x4 ordered dither, so tile them;\n *     do not scale them.") if dither else "The bands are 1 px;\n *     stretch or tile them.",
       "" if fmt == "argb8888" else " *   - A8: LVGL draws the image as a mask filled with the image recolor\n *     colour, so set image_recolor (draw dsc .recolor) to black.\n",
       tile))


# ---------------------------------------------------------------- PNG preview

def png_bytes(w, h, rgb_rows):
    raw = bytearray()
    for row in rgb_rows:
        raw.append(0)
        raw += row
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def to565(v):
    """Round-trip an 8-bit channel value through 5 bits (R/B) worst case."""
    q = (v * 31 + 127) // 255
    return (q * 255 + 15) // 31


def compose(canvas, cw, ch, sx, sy, alpha8, meta, bw, bh, surface, radius_px, tile, round_shape):
    """Draw the shadow nine-slice for a box at (sx, sy, bw, bh), then the surface."""
    w, h = meta["w"], meta["h"]
    L, T, R, B = meta["left"], meta["top"], meta["right"], meta["bottom"]
    dw = bw + (w - meta["box_min_w"])
    dh = bh + (h - meta["box_min_h"])
    ox, oy = sx - meta["box_x"], sy - meta["box_y"]
    cwid = w - L - R
    chei = h - T - B

    def src_x(dx):
        if round_shape or dx < L:
            return dx
        if dx >= dw - R:
            return dx - (dw - w)
        return L + (dx - L) % cwid

    def src_y(dy):
        if round_shape or dy < T:
            return dy
        if dy >= dh - B:
            return dy - (dh - h)
        return T + (dy - T) % chei

    for dy in range(dh):
        y = oy + dy
        if not (0 <= y < ch):
            continue
        srow = alpha8[src_y(dy)]
        crow = canvas[y]
        for dx in range(dw):
            x = ox + dx
            if 0 <= x < cw:
                a = srow[src_x(dx)]
                if a:
                    p = crow[x]
                    crow[x] = tuple((c * (255 - a) + 127) // 255 for c in p)
    # surface (anti-aliased rounded rect)
    cov = rrect_coverage(bw, bh, 0, 0, bw, bh, radius_px)
    for j in range(bh):
        y = sy + j
        if not (0 <= y < ch):
            continue
        for i in range(bw):
            x = sx + i
            c = cov[j][i]
            if c > 0 and 0 <= x < cw:
                p = canvas[y][x]
                canvas[y][x] = tuple(int(round(s * c + q * (1 - c))) for s, q in zip(surface, p))


def preview(entries, out_dir, tile, combined_path):
    os.makedirs(out_dir, exist_ok=True) if out_dir else None
    bgs = [((0xFF, 0xFF, 0xFF), (0xFF, 0xFF, 0xFF), "light"), ((0x12, 0x12, 0x12), None, "dark")]
    # dark surface overlay %% per elevation (MD2 dark theme)
    overlay = {1: 0.05, 2: 0.07, 4: 0.09, 6: 0.11, 8: 0.12, 16: 0.15, 24: 0.16}
    cell_w, cell_h = 400, 380
    cols = ELEVATIONS
    shape_rows = [s[0] for s in SHAPES]
    W = cell_w * len(cols)
    H = cell_h * len(shape_rows) * len(bgs)
    combined = [[(0, 0, 0)] * W for _ in range(H)]
    by_key = {(e["shape_key"], e["elev"]): e for e in entries}
    for bi, (bg, surf, bgname) in enumerate(bgs):
        for si, sk in enumerate(shape_rows):
            for ci, elev in enumerate(cols):
                e = by_key.get((sk, elev))
                cx0, cy0 = ci * cell_w, (bi * len(shape_rows) + si) * cell_h
                canvas = [[bg] * cell_w for _ in range(cell_h)]
                if e:
                    m = e["meta"]
                    s = surf or tuple(int(round(0x12 + (255 - 0x12) * overlay[elev])) for _ in range(3))
                    if e["round"]:
                        bw = bh = m["box_min_w"]
                    else:
                        bw, bh = max(176, m["box_min_w"]), max(120, m["box_min_h"])
                    sx, sy = (cell_w - bw) // 2, (cell_h - bh) // 2 - 16
                    compose(canvas, cell_w, cell_h, sx, sy, e["alpha8"], m, bw, bh, s,
                            e["meta"]["radius_px"] if not e["round"] else bw / 2.0, tile, e["round"])
                    if out_dir:
                        rows = [bytes(to565(c) for p in r for c in p) for r in canvas]
                        write(os.path.join(out_dir, "%s_%s.png" % (e["name"], bgname)),
                              png_bytes(cell_w, cell_h, rows), binary=True)
                for j in range(cell_h):
                    combined[cy0 + j][cx0:cx0 + cell_w] = canvas[j]
    # Combined preview at 1 px per dp (2x2 box filter) to keep it small.
    small = []
    for y in range(0, H, 2):
        r0, r1 = combined[y], combined[y + 1]
        row = bytearray()
        for x in range(0, W, 2):
            for c in range(3):
                row.append((r0[x][c] + r0[x + 1][c] + r1[x][c] + r1[x + 1][c] + 2) // 4)
        small.append(bytes(row))
    os.makedirs(os.path.dirname(combined_path), exist_ok=True)
    write(combined_path, png_bytes(W // 2, H // 2, small), binary=True)


# ---------------------------------------------------------------- main

def verify_mdc():
    with urllib.request.urlopen(MDC_URL) as r:
        data = r.read()
    got = hashlib.sha256(data).hexdigest()
    text = data.decode("utf-8")
    for var, table in (("umbra-map", UMBRA), ("penumbra-map", PENUMBRA), ("ambient-map", AMBIENT)):
        block = text.split("$%s: (" % var, 1)[1].split(")", 1)[0]
        for lvl, val in re.findall(r"(\d+): '([^']+)'", block):
            lvl = int(lvl)
            if lvl in table:
                nums = tuple(int(v.replace("px", "")) for v in val.split())
                if nums != table[lvl]:
                    sys.exit("MDC mismatch %s %d: %s vs %s" % (var, lvl, nums, table[lvl]))
    for name, val in (("umbra-opacity", UMBRA_OPACITY), ("penumbra-opacity", PENUMBRA_OPACITY),
                      ("ambient-opacity", AMBIENT_OPACITY)):
        m = re.search(r"\$%s: ([0-9.]+)" % name, text)
        if not m or float(m.group(1)) != val:
            sys.exit("MDC mismatch " + name)
    print("MDC %s tables match (sha256 %s%s)" % (MDC_VERSION, got, "" if got == MDC_SHA256 else ", differs from pinned"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--preview", metavar="DIR", help="write per-shadow PNG previews to DIR")
    ap.add_argument("--no-dither", action="store_true", help="disable 4x4 Bayer alpha dithering")
    ap.add_argument("--cutoff", type=float, default=1.0, help="outer crop threshold, alpha/255 (default 1)")
    ap.add_argument("--tol", type=float, default=1.0, help="nine-slice centre tolerance, alpha/255 (default 1)")
    ap.add_argument("--format", choices=sorted(CF), default="a8",
                    help="image colour format (default a8: black alpha mask, 4x smaller than argb8888)")
    ap.add_argument("--verify-mdc", action="store_true", help="download the MDC scss and check the tables")
    args = ap.parse_args()
    if args.verify_mdc:
        verify_mdc()
    dither = not args.no_dither
    tile = TILE_DITHER if dither else 1

    os.makedirs(OUT_DIR, exist_ok=True)
    os.makedirs(OUT_INC, exist_ok=True)
    entries = []
    for key, radius_dp, round_d, elevs in SHAPES:
        for elev in elevs:
            name = "md_shadow_%s_e%d" % (key, elev)
            rpx = 0 if radius_dp is None else radius_dp * DP
            alpha, meta = bake(elev, rpx, round_d, args.cutoff / 255.0, args.tol / 255.0, tile)
            a8 = quantize(alpha, dither)
            shape_desc = ("round %d dp" % round_d) if round_d else ("%d dp corners" % radius_dp)
            desc = "MD2 elevation %d dp, %s, %dx%d px, insets L%d T%d R%d B%d, box at (%d,%d)%s" % (
                elev, shape_desc, meta["w"], meta["h"], meta["left"], meta["top"], meta["right"],
                meta["bottom"], meta["box_x"], meta["box_y"], ", dithered" if dither else "")
            size = emit_image(name, a8, meta, desc, args.format)
            entries.append({"name": name, "elev": elev, "shape": (-round_d if round_d else radius_dp),
                            "shape_key": key, "round": round_d is not None, "meta": meta,
                            "alpha8": a8, "bytes": size})
            print("%-24s %4dx%-4d %8d bytes" % (name, meta["w"], meta["h"], size))
    wanted = {e["name"] + ".c" for e in entries} | {"md_shadows.c"}
    for old in os.listdir(OUT_DIR):
        if old.endswith(".c") and old not in wanted:
            os.remove(os.path.join(OUT_DIR, old))
    emit_table(entries, dither, tile, args.format)
    total = sum(e["bytes"] for e in entries)
    print("total pixel data %d bytes (%.1f KiB)" % (total, total / 1024.0))
    preview(entries, args.preview, tile, PREVIEW_PNG)
    print("preview", os.path.relpath(PREVIEW_PNG, REPO))


if __name__ == "__main__":
    main()
