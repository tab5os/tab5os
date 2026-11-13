# t5ui: the app UI API

Nov 12, 2026 · API level 2 · header `t5/t5ui.h` (`t5_gui/t5ui.h`, icons in `t5_gui/t5ui_icons.h`)

## Why

Level-1 native apps draw with LVGL 9 directly. They call `lv_*` functions and compile in LVGL's types, enum values and macros. Some also keep LVGL's own data: a `lv_style_t`, an embedded `lv_font_t`, a custom `lv_obj_class_t`. That ties every such app to LVGL 9's ABI, so when LVGL's next major version comes, the OS couldn't move to it without breaking those apps.

t5ui is a UI API that belongs to Tab5 OS, with the UI library behind it as a swappable backend. Today the backend is LVGL 9 plus Tab5's Material widgets (md). Moving to another library means writing a new backend. The API and the apps built on it stay as they are.

## The model

The API is a small set of calls that doesn't grow. New abilities come as new numbers.

| Part | Calls | Grows by |
| --- | --- | --- |
| Objects | `t5ui_create(parent, kind)`, `t5ui_delete`, `t5ui_child`, `t5ui_coords`... | new `T5UI_*` kinds |
| Properties | `t5ui_set_int / _color / _str / _strf / _obj`, the getters | new `T5UI_P_*` properties |
| Parts and states | `t5ui_set_int_at(obj, p, T5UI_AT(part, states), v)` | new parts and states |
| Styles | `t5ui_style_create`, `_set_int / _color`, `_add`, `_remove` | (the look properties) |
| Animations | `t5ui_animate(obj, p, from, to, ms, delay, ease)` | (any int property) |
| Events | `t5ui_on(obj, T5UI_EV_*, cb, user)` | new `T5UI_EV_*` events |
| Commands | `t5ui_cmd(obj, T5UI_CMD_*, a, b, c, text)` | new `T5UI_CMD_*` commands |
| Drawing | canvases, `T5UI_EV_DRAW`, `t5ui_draw_*` | |
| Other | timers, `t5ui_async`, `t5ui_touches`, theme, dialogs, menus, snackbars | |

```c
t5ui_obj_t *b = t5ui_create(row, T5UI_MD_BUTTON_CONTAINED);
t5ui_set_str(b, T5UI_P_TEXT, "Type");
t5ui_set_int(b, T5UI_P_GROW, 1);
t5ui_set_color_at(b, T5UI_P_BG_COLOR, T5UI_AT(T5UI_PART_MAIN, T5UI_STATE_PRESSED), 0x3355ff);
t5ui_on(b, T5UI_EV_CLICK, type_clicked, NULL);
```

**Numbers are forever.** Every kind, property, part, state, event and command has an explicit number in the header. A number never changes meaning and is never reused. The host test pins a sample of them.

**Each property has one type**: int (also bools and enums), colour (`0xRRGGBB`), string (copied) or object. The core checks the type, so `t5ui_set_int(obj, T5UI_P_TEXT, 1)` returns `T5_ERR_NOT_SUPPORTED` before any backend sees it.

**Look properties** (sizes, spacing, layout, colours, borders, shadows, text style, transforms, lines and arcs) can also be set for a part and states, and in styles. The other properties are widget properties: values, text, flags.

**Kinds that don't have a property** return `T5_ERR_NOT_SUPPORTED`, and a getter returns the caller's default. An app can try a property and fall back. A future backend that can't do something says so the same way, instead of crashing or doing something else.

**Material choices fixed at creation are separate kinds**, such as `T5UI_MD_BUTTON_TEXT / _OUTLINED / _CONTAINED`, `T5UI_MD_CHIP_ACTION / _FILTER / _CHOICE`, and `T5UI_MD_APPBAR` vs `T5UI_MD_APPBAR_BACK`. Everything else about them is a property.

**Some kinds hold others:**
- A `T5UI_MD_PAGE` puts content under its app bar.
- `T5UI_MD_TABS` holds `T5UI_MD_TAB`s, and content goes in a tab.
- A `T5UI_MD_ROW_SWITCH` goes in a `T5UI_MD_ROW`.
- `T5UI_MD_APPBAR_ACTION` goes in an app bar or a page.
- A `T5UI_MD_LIST` builds its own rows. Set `T5UI_P_COUNT`, and fill each visible row in `T5UI_EV_BIND` by setting `T5UI_P_TEXT`, `_SECONDARY` and `_ICON` on `t5ui_event_target(e)`.

## How it's built

```
app ──► t5ui.h ──► t5ui_core.c ──► t5ui_backend.h ──► t5ui_lvgl9.c ──► LVGL 9 + md
                   (checks:          (t5ui_be_set,       (the only file
                    types, parts,     _get, _create,      that meets LVGL)
                    kinds, events)    _on, _cmd...)
```

- **`t5ui_core.c`** knows every property's type and whether it's a look property, and checks each call's arguments. It doesn't know what draws.
- **`t5ui_backend.h`** is the contract: the `t5ui_be_*` calls receive only well-formed arguments. A backend implements those, plus the rest of `t5ui.h` (tree, timers, theme, drawing).
- **`t5ui_lvgl9.c`**:
  - Maps look properties to LVGL style properties, both local and in `lv_style_t`.
  - Maps widget properties to LVGL and md calls, by kind.
  - Gives each object it makes a small node: kind, user pointer, pixel buffer, animations. md keeps its own data in LVGL's `user_data`, so the node can't go there.
  - Turns md callbacks (back arrows, list binding and taps) into t5ui events.
  - Gained a few md accessors for this: the app bar title, row labels and icon, text field label, list count and row height.

Moving to LVGL 10 means writing `t5ui_lvgl10.c` against the same `t5ui_backend.h` and changing one line in `t5_gui/CMakeLists.txt`.

A host test (`test/host/test_t5ui.c`):
- Compiles the header with no LVGL on the include path, so a header change that pulls LVGL back in fails CI.
- Pins the numbers.
- Runs the core against a fake backend, checking that badly typed, unknown or misplaced calls never reach it.

## What it covers

- **Plain kinds:** box, panel, label, button, image, canvas, switch, checkbox, slider, arc, bar, spinner, LED, textarea, dropdown, roller, chart, table, keyboard.
- **Material kinds:** text, icon, the three buttons, FAB and mini FAB, card, divider, app bars and actions, list rows with switches, sections, switch, checkbox, slider, text and password fields, the three chips, tabs, page, recycling list.
- **Properties:**
  - Geometry and alignment.
  - Flex layout (flow, placement, grow, floating).
  - Padding, gaps and margins.
  - Background, gradient, border, outline, shadow, radius, corner clipping, opacity, Material elevation.
  - Translate, scale, rotate.
  - Text, colour, style, alignment, spacing, decoration, long mode.
  - Secondary text, icon and icon size, placeholder, helper and error, title.
  - Lines and arcs; image recolour, scale, rotation.
  - Hidden, clickable, scrolling and scrollbars, checked, disabled, checkable, bubbling.
  - Value and range, arc angles, selection, options, counts, table size, textarea modes, keyboard target and mode, chart type and grid, list row height, spinner speed.
- **Commands:** chart series and points, table cells, text insert and delete, list refresh, scroll to and by, dropdown open and close, focus.
- **Drawing:** pixels (RGB565, RGB888, ARGB8888, A8) in images, persistent canvases, and lines, rectangles, arcs, text and pixels on a canvas or over any object (`T5UI_EV_DRAW`).

## The OS's own UI

The OS draws its own UI with t5ui as well:
- the shell: home screen, app drawer, Quick Settings, standby, status and gesture bars, on-screen keyboard
- every system app
- the file picker, the app-not-responding dialog and the permission prompt
- Python's `t5.ui` and its console

None of these files call LVGL or md any more. Moving the OS to another UI library therefore means a new backend, not a rewrite of the OS.

**`t5_gui/t5ui_sys.h`** adds what only the system may do. It isn't in the SDK or the app ABI.
- Layers above every app: `t5ui_sys_layer`.
- A screen of its own: `t5ui_sys_screen_create`, for the home screen.
- Setting the theme's accent.
- Snapshots, for the app-not-responding picture.
- Text shown from the caller's buffer without a copy, for the Python console's scrollback.
- The name of what draws the UI, for the Licences page.
- The marked boundary with the platform code under the UI: `t5ui_sys_obj` and `t5ui_sys_native`, and `t5ui_sys_native_input` for the on-screen keyboard.

**Still on LVGL, because they're what the UI runs on rather than UI:**
- the display and touch drivers, the hardware plane and PPA (`t5_ui`)
- the LVGL exports and app screens for level-1 apps (`t5_app/wrappers.c`, `lifecycle.c`)
- the shell's edge-swipe input hook (`gesture.c`, marked PLATFORM)
- the diagnostics console's LVGL test harness (`t5_diag/diag_ui.c`)
- the simulator's stand-in apps

**Checks:**
- The simulator renders the shell's 17 golden scenes pixel for pixel as before the port.
- The firmware builds with no warnings.
- Every ported file is free of `lv_*` and `md_*` calls.

## What stays

- **Level-1 apps** keep calling LVGL 9: those exports are released and frozen. When the OS moves to a newer LVGL, they need a compatibility layer that implements the frozen LVGL 9 calls on top of it. Apps that keep LVGL's own data are the hard part of that layer. t5ui apps don't need it.
- **Level-2 apps** may still import `lv_*`, but then they're tied to LVGL 9 like level-1 apps. New apps should use t5ui only. The Keyboard & Trackpad example does: it imports 16 t5ui calls and no `lv_*` or `md_*` symbol.
- **Python apps** use `t5.ui`, which was never LVGL-shaped, so nothing changes for them.

## Not yet

- Custom fonts, and decoding image files: decode first, then use `t5ui_image_pixels`.
- Grid layout. Grid would be new look properties, `T5UI_P_GRID_*`.
- Porting the other native examples, and documenting t5ui in the SDK guide.
