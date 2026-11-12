# t5ui: the app UI API

Nov 12, 2026 · API level 2 · header `t5/t5ui.h` (`t5_gui/t5ui.h`, icons in `t5_gui/t5ui_icons.h`)

## Why

Level-1 native apps draw with LVGL 9 directly: they call `lv_*` functions and compile in LVGL's types, enum values and macros, and some keep LVGL's own data (a `lv_style_t`, an embedded `lv_font_t`, a custom `lv_obj_class_t`). That ties every such app to LVGL 9's ABI. When LVGL's next major version comes, the OS couldn't move to it without breaking those apps.

t5ui puts a Tab5 OS API between apps and the renderer. Everything an app compiles in from `t5/t5ui.h` is Tab5's own:

| What | t5ui | Not |
| --- | --- | --- |
| Objects, events, timers, paint targets | opaque handles (`t5ui_obj_t *`...) | `lv_obj_t`, `lv_event_t` |
| Colours, opacity | `0xRRGGBB`, `0..255` | `lv_color_t` |
| Sizes | pixels, `T5UI_PCT(n)`, `T5UI_CONTENT` (Tab5 encodings) | `LV_PCT`, `LV_SIZE_CONTENT` |
| Fonts | text styles (`T5UI_TEXT_BODY1`...) | `lv_font_t` |
| Images, canvases | `t5ui_pixels_t` (copied by the OS) | `lv_image_dsc_t`, `lv_draw_buf_t` |
| Drawing | `t5ui_draw_*` on a `t5ui_paint_t` | `lv_layer_t`, draw descriptors |
| Icons | `T5UI_ICON_*` UTF-8 strings (Material Icons codepoints) | `LV_SYMBOL_*` |

Only `firmware/components/t5_gui/t5ui.c` meets LVGL. Moving the OS to another LVGL means rewriting that file. Apps built against t5ui keep running unchanged.

A host test (`test/host/test_t5ui.c`) compiles the header with no LVGL on the include path, so a header change that pulls LVGL back in fails CI.

## What it covers

- **Tree and layout:** `t5ui_root`, `t5ui_box`, `t5ui_panel`, flex layout (`t5ui_flex`, `t5ui_flex_place`, `t5ui_grow`), padding, gaps, margins, alignment, coordinates.
- **Appearance and state:** background, border, radius, shadow, Material elevation, opacity, text colour, style and alignment, strokes; hidden, clickable, scrolling, checked, disabled.
- **Events and timers:** `t5ui_on(obj, T5UI_EV_CLICK, cb, user)` and the other events, `t5ui_timer`, `t5ui_async` from other tasks.
- **Material widgets:** buttons, FABs, cards, dividers, app bars, list rows (with switches and trailing text), sections, switches, checkboxes, sliders, arcs, progress bars, text fields and areas, chips, tabs, pages, recycling lists, dialogs, menus, snackbars.
- **Images and drawing:** pixels (RGB565, RGB888, ARGB8888, A8), canvases, and drawing lines, rectangles, arcs, text and pixels, on a canvas or on any object (`T5UI_EV_DRAW`).
- **Charts:** line and bar, up to 8 series.
- **Theme and input:** theme colours and emphasis, dark mode, every finger on the screen (`t5ui_touches`).

The native examples used 147 distinct LVGL and md calls between them. t5ui covers what they did with them, apart from custom widget classes: compose objects and draw on them with `T5UI_EV_DRAW` instead.

## What stays

- **Level-1 apps** keep calling LVGL 9: those exports are released and frozen. When the OS moves to a newer LVGL, they need a compatibility layer that implements the frozen LVGL 9 calls on top of it. Apps that keep LVGL's own data (styles, fonts, image descriptors, widget classes) are the hard part of that layer. t5ui apps don't need it.
- **Level-2 apps** may still import `lv_*` (the exports exist for level 1), but then they are tied to LVGL 9 like level-1 apps. New apps should use t5ui only. The Keyboard & Trackpad example does: its ELF imports no `lv_*` or `md_*` symbol.
- **Python apps** use `t5.ui`, which was never LVGL-shaped, so nothing changes for them.

## Not yet

- Animations, custom fonts, and decoding image files (no PNG, JPEG or BMP decoder is built in; decode first, then use `t5ui_image_pixels`).
- Porting the other native examples and documenting t5ui in the SDK guide.
