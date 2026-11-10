# Asset tools

These scripts generate the Material Design 2 assets for the `md` component:
fonts, icons and baked shadows. The generated C files are committed, so a
firmware build does not need these tools. Density is fixed at 2 px per dp.

Both scripts are deterministic, so running them again gives the same bytes. If
`git diff` is empty after a regeneration, nothing changed.

## Regenerate

```sh
python3 tools/assets/fonts.py      # needs node/npx and network access (or a warm cache)
python3 tools/assets/shadows.py    # stdlib only, takes about 7 s
```

| Option | What it does |
| --- | --- |
| `fonts.py --offline` | Uses only `tools/assets/.cache/`. Fails if a file is missing or its hash is wrong. |
| `shadows.py --preview DIR` | Writes a full-resolution PNG of each shadow on `#FFFFFF` and on `#121212`, quantised to RGB565. |
| `shadows.py --no-dither` | Turns off the 4×4 Bayer alpha dithering. The centre band shrinks from 4 px to 1 px. |
| `shadows.py --format argb8888` | Writes ARGB8888 instead of the default `LV_COLOR_FORMAT_A8` masks, four times the size (see "Shadow size"). |
| `shadows.py --cutoff N --tol N` | Sets the crop and nine-slice tolerances in alpha/255. Both default to 1. |
| `shadows.py --verify-mdc` | Downloads the MDC scss again and checks it against the tables in the script. |

Every `shadows.py` run also rewrites `tools/assets/preview/shadows.png`, a
combined preview at 1 px per dp. It is committed so the look can be reviewed.

## Pinned inputs

| Input | Version / pin | Licence |
| --- | --- | --- |
| Roboto Regular and Medium (`src/hinted/*.ttf`) | googlefonts/roboto at commit `38062f4b4a0be4346d07a928408da21602545e9e`, font version 2.001047 (release v2.138). SHA-256 values are in `fonts.py`. | Apache-2.0 |
| Material Icons Regular (`font/MaterialIcons-Regular.ttf` and `.codepoints`) | google/material-design-icons at commit `10ecc14790612ca8fd164e3f4ffe9c16fc50d5b7`, font version 1.017 | Apache-2.0 |
| lv_font_conv | 1.5.3, run with `npx -y lv_font_conv@1.5.3` | MIT |
| MD2 shadow values | `@material/elevation` 14.0.0, `_elevation-theme.scss` (copied into `shadows.py` with its URL and SHA-256) | MIT |

Downloads are cached in `tools/assets/.cache/`, which git ignores. Every file
is checked against its pinned SHA-256.

## Outputs

| Path | Contents |
| --- | --- |
| `firmware/components/md/assets/fonts/md_font_*.c` | 8 Roboto fonts, the standby clock font (digits and `:` `-` only, 200 px) and 3 icon fonts. LVGL `lv_font_fmt_txt`, bpp 4, uncompressed. |
| `firmware/components/md/include/md/md_fonts.h` | `LV_FONT_DECLARE` for each font, plus a table mapping fonts to MD2 type roles. |
| `firmware/components/md/include/md/md_icons.h` | `MD_ICON_<NAME>` as a UTF-8 string literal for each icon in the subset. |
| `firmware/components/md/assets/LICENSES/` | Roboto and Material Icons licence texts, plus a README naming each one's source and version. |
| `firmware/components/md/assets/shadows/md_shadow_*.c` | Nine-slice shadow images as `lv_image_dsc_t`. |
| `firmware/components/md/assets/shadows/md_shadows.c` | The lookup table and `md_shadow_get()`. |
| `firmware/components/md/include/md/md_shadows.h` | `md_shadow_t` and the drawing rules for the nine-slices. |

### Fonts

Text fonts cover ASCII, Latin-1 and `• … – — ‘ ’ “ ” €`. Each size is the
MD2 type scale size × 2: Headline 4 → 68 px, Headline 5 → 48, Headline 6 →
40 Medium, Subtitle 1 and Body 1 → 32, Body 2 → 28, Button → 28 Medium,
Caption → 24, Overline → 24 Medium. The styles set line heights, not the fonts.

Icon fonts come in 36, 48 and 80 px (18, 24 and 40 dp). To add an icon, put
its name from the codepoints file in `ICONS` in `fonts.py` and run the script
again.

`fonts.py` patches lv_font_conv's output to match the LVGL 9.6 built-in fonts:

- a plain `#include "lvgl.h"`
- `cap_height` and `x_height`, taken from the H and x glyphs
- `.static_bitmap = 1`
- no per-file enable guard

The recorded options line holds no absolute paths.

### Shadows

For each elevation, the script draws the three MD2 layers (umbra, penumbra and
ambient) around a box, using CSS `box-shadow` semantics:

- offsets and spread in dp × 2
- spread corner radii follow the CSS rules
- Gaussian blur with sigma = blur ÷ 2

It then:

1. composites the layers as black over black,
2. cuts out the box interior, keeping a 1 px overlap under the surface edge,
3. trims the outer edge where alpha < 1/255,
4. shrinks the inner part of each corner until the centre band matches the
   true shadow to within 1/255. A check that rebuilt a 420 × 300 box and
   compared it with a direct render found a largest error below 1/255.

Dithering quantises alpha to 31 levels. One level is one 5-bit RGB565 step on
white. A 4×4 Bayer pattern spreads the steps, so the stretched centre band is
4 px wide (one Bayer period) and must be tiled, not scaled.

Shapes baked:

- 0 dp and 4 dp corners at 1, 2, 4, 6, 8, 16 and 24 dp elevation
- round 40 dp and 56 dp FABs at 6 and 8 dp elevation

`md_shadow_get(elevation_dp, radius_dp)` returns the nearest one.

#### Shadow size

MD2 shadows are large at 2 px per dp. The 24 dp ambient layer has a 92 px blur
(sigma 46 px), so its corner slices alone need about 180 px a side. Here is the
pixel data in each format:

| Variant | ARGB8888 | A8 |
| --- | --- | --- |
| 1–8 dp, 0 and 4 dp corners (10 images) | 287 KB | 72 KB |
| 16 dp, 0 and 4 dp corners | 447 KB | 112 KB |
| 24 dp, 0 and 4 dp corners | 1,067 KB | 267 KB |
| FAB, round 40 and 56 dp (4 images) | 398 KB | 100 KB |
| **Total** | **2,199 KB** | **550 KB** |

That is well over the 200 KB target. A8 is the better format here. The image
is pure black, and LVGL 9.6's software renderer draws an A8 image as a mask
filled with `image_recolor`. That is its cheapest blend path, and it reads a
quarter of the bytes.

**What ships (2026-11-05):** A8, and fewer variants: the 4 dp set at every
elevation (square surfaces use it too) and the FABs at their resting 6 dp
only. 272 KiB of pixel data in total; the whole OS image went from 5.1 MB to
3.05 MB with it.
