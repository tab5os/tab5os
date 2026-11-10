# UI guide

Apps use LVGL 9.6 with a Material Design 2 layer (`md_*`) that already matches the shell. Use `md_*` first and raw LVGL for what it doesn't cover. Include `<t5/ui.h>`.

## Screen and units

| | Landscape | Portrait |
| --- | --- | --- |
| Pixels | 1280 x 720 | 720 x 1280 |
| dp | 640 x 360 | 360 x 640 |

Density is fixed at 2 px per dp. Always write `T5_DP(n)`, so a size means the same thing everywhere. A literal `1` is right only for a one-pixel hairline.

The shell's bars take space from your screen, as padding on `t5_app_screen()`: the status bar the top 24 dp and the gesture bar the bottom 16. There's no title bar unless you draw one. `t5_shell_get_insets()` says how much, `T5_EV_UI_INSETS` when it changes, and `t5_shell_set_fullscreen(true)` gives it all back until you leave the foreground. Keep important content away from the edges, since two system gestures start there: swipe down from the top for quick settings and swipe up from the bottom to go home. See [Shell bars](#shell-bars).

Touch targets are at least 48 dp. The `md_*` controls already are.

## A page

Most apps want one of these two shapes.

```c
/* Scrolling page: the simplest good default */
lv_obj_t *page = md_page_create(t5_app_screen(app), "Title", back, s, NULL);
lv_obj_t *body = md_page_body(page);
lv_obj_set_style_pad_all(body, T5_DP(16), 0);
lv_obj_set_style_pad_row(body, T5_DP(8), 0);
md_label_create(body, "Hello", MD_TYPE_BODY1);
```

```c
/* App bar plus a body that fills the rest */
lv_obj_t *scr = t5_app_screen(app);
lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
lv_obj_set_style_pad_gap(scr, 0, 0);
lv_obj_t *bar = md_appbar_create(scr, "Title", MD_ICON_ARROW_BACK, back, s);
md_appbar_add_action(bar, MD_ICON_REFRESH, refresh, s);

lv_obj_t *body = lv_obj_create(scr);
lv_obj_remove_style_all(body);
lv_obj_set_width(body, LV_PCT(100));
lv_obj_set_flex_grow(body, 1);
```

`lv_obj_remove_style_all` is the right start for layout containers: a plain `lv_obj` comes with background, border and padding you'd then pay to draw.

## Navigation

There's no navigation stack. Do one of these:

- **Sub-page as an overlay on the same screen:** create an `md_page`, mark it floating, delete it on back. Weather does this. Everything stays under `t5_app_screen`, so it's tracked.
- **Swap the content:** `lv_obj_clean(body)` and rebuild.
- **Tabs:** `md_tabs_create` and `md_tabs_add`.
- **Drawer:** `md_drawer_open`, `md_drawer_add_item`.

```c
s->page = md_page_create(s->root, "Places", places_back, s, NULL);
lv_obj_set_floating(s->page, true);
lv_obj_set_pos(s->page, 0, 0);
```

Avoid creating additional LVGL screens. They aren't tracked, so they aren't cleaned up. Everything should be under `t5_app_screen()` or on `lv_layer_top()`, which is tracked. Remember to bump your [generation counter](Threading.md#results-that-arrive-too-late) when deleting a page that background work can update.

## Widgets

| Need | Call |
| --- | --- |
| Text | `md_label_create(parent, "x", MD_TYPE_BODY1)` |
| Button | `md_button_create(parent, "Save", MD_BUTTON_CONTAINED)`. Also `MD_BUTTON_OUTLINED`, `MD_BUTTON_TEXT` |
| Floating action button | `md_fab_create(parent, MD_ICON_ADD, false)` |
| Card | `md_card_create(parent)` |
| Text field | `md_textfield_create(parent, "Label", false)`, then `md_textfield_text(field)` |
| Switch, slider, checkbox | `md_switch_create`, `md_slider_create`, `md_checkbox_create` |
| Chips | `md_chip_create(parent, "Tag", NULL, MD_CHIP_FILTER)` |
| Short static list | `md_row_create(parent, icon, primary, secondary)` with `md_row_switch`, `md_row_trailing_text`, `md_row_set_clickable`; `md_section_create` for headers |
| Long list | `md_list_create` ([below](#lists)) |
| Divider | `md_divider_create(parent)` |
| Icon | `md_icon_create(parent, MD_ICON_WIFI, 24)`, 18, 24 or 40 dp |
| Menu | `md_menu_show(anchor, items, n, cb, s)` |
| Modal | `md_dialog_show("Title", "Text", "OK", "Cancel", cb, s)`; `cb` gets 0, 1, or -1 if dismissed |
| Message | `md_snackbar_show("Saved", NULL, NULL, NULL)`, UI task. `t5_shell_toast("Saved")` from any task |

Everything else, such as `lv_chart`, `lv_arc`, `lv_bar`, `lv_roller`, `lv_dropdown`, `lv_spinner`, `lv_table`, `lv_canvas`, `lv_calendar`, works as in the LVGL docs. Respect the flag-setter difference in [the ABI section](Manifest-and-Packaging.md#the-abi).

## Events

```c
lv_obj_add_event_cb(button, on_click, LV_EVENT_CLICKED, state);

static void on_click(lv_event_t *e)
{
    state_t *s = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_target_obj(e);
}
```

Switches, sliders, checkboxes and chips send `LV_EVENT_VALUE_CHANGED`. A text field's `lv_textarea` (`md_textfield_ta`) sends `LV_EVENT_READY` when Enter is pressed.

## Layout

Use flex or grid, never fixed positions, so rotation and text changes keep working.

```c
lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
lv_obj_set_flex_grow(child, 1);
```

Sizes: `LV_PCT(100)`, `LV_SIZE_CONTENT` or `T5_DP(n)`. Positions are only known after a layout: call `lv_obj_update_layout(obj)` before reading them or before `lv_obj_scroll_to_y`.

## Text and fonts

| Role | Size (dp) | Python | Use |
| --- | --- | --- | --- |
| `MD_TYPE_HEADLINE4` | 34 | `h4` | Big numbers |
| `MD_TYPE_HEADLINE5` | 24 | `h5` | Page headings |
| `MD_TYPE_HEADLINE6` | 20 | `h6` | App bars, dialog titles |
| `MD_TYPE_SUBTITLE1` | 16 | `subtitle1` | List primary text |
| `MD_TYPE_BODY1` | 16 | `body1` | Body text |
| `MD_TYPE_BODY2` | 14 | `body2` | Secondary text |
| `MD_TYPE_BUTTON` | 14, medium | `button` | Button labels |
| `MD_TYPE_CAPTION` | 12 | `caption` | Small print |
| `MD_TYPE_OVERLINE` | 12, medium | `overline` | Section labels |

`md_font_mono()` is Roboto Mono at 12 dp for code and logs.

**Coverage:** ASCII, Latin-1 (`é`, `°`, `ü`...) and a little punctuation: en and em dashes, curly quotes, bullet, ellipsis and euro. Other characters don't render, and that includes CJK, Cyrillic, Greek, Arabic and emoji.

For other scripts, compile a font with `lv_font_conv` into your app as a `const lv_font_t` and set it with `lv_obj_set_style_text_font`. It takes app memory and loses the role styling, so subset it. `apps/examples/calculator` does this for maths symbols (−, √, π, superscripts): its `gen_fonts.py` builds two Roboto sizes from the same pinned source and converter settings as the OS fonts, about 125 KB together.

For dynamic text, `lv_label_set_text_fmt` and `snprintf` work. Set text only when it changes, because every set redraws the label ([Performance](Performance.md#updating-widgets)). Every role is sentence case: write labels the way they should appear ("Save changes", not "SAVE CHANGES").

## Colour

Use the theme, so accents chosen in Settings apply.

```c
const md_scheme_t *c = md_scheme();
lv_obj_set_style_text_color(label, md_color(c->c.secondary), 0);
lv_obj_set_style_text_opa(label, c->on_surface.medium, 0);
```

Fields: `c->c.primary`, `.secondary`, `.background`, `.surface`, `.error`, `.on_primary`, `.on_surface`; emphasis `c->on_surface.high`, `.medium`, `.disabled`.

Dark is the only theme people can pick at the moment.

Elevation on the dark theme is a lighter surface: `md_set_elevation(obj, 4, false)`. Don't hand-roll shadows with `lv_obj_set_style_shadow_*`. See [Performance](Performance.md#what-a-pixel-costs).

## Icons

The 160 `MD_ICON_*` names in `md/md_icons.h` are all there is. An unknown name is a compile error, which is helpful. Icons are text: `lv_label_set_text(label, MD_ICON_WIFI)` works on a label with `md_icon_font(24)`.

Your app's own launcher icon is a PNG named in the manifest, not one of these. See [App icon](Manifest-and-Packaging.md#app-icon).

## Lists

`md_row_*` builds real objects, which is right for a settings page. For more than about 30 rows, or any list that grows, use the recycled list:

```c
lv_obj_t *list = md_list_create(parent, count, MD_LIST_ROW_2_DP, bind, clicked, state);
md_list_set_count(list, new_count);
md_list_refresh(list);        /* the data changed but the count didn't */
```

Row heights are `MD_LIST_ROW_1_DP` (48), `_2_DP` (64) and `_3_DP` (72). `bind` fills `row->primary`, `row->secondary`, `row->icon`. It's called while scrolling, so it must be fast and it must not allocate or do I/O. Data for it should already be in memory. There's a [recipe](Cookbook.md#long-list).

## Images

LVGL here has **no PNG or JPEG decoder and no file-system driver**. `lv_image_set_src(img, "S:/x.png")` will not work. Options:

| Source | How |
| --- | --- |
| A built-in bitmap | A `const lv_image_dsc_t` in your code, from LVGL's image converter |
| A raw file | Read RGB565 from `t5_app_install_dir`, or a [picked file](Storage.md#letting-the-person-choose), into `malloc`'d memory, and fill an `lv_image_dsc_t` |
| A JPEG | `t5_jpeg_decode` on a worker task, into a buffer of `t5_jpeg_size` size |
| Drawing | `lv_draw_buf_create` with `lv_canvas` |

```c
/* The decode buffer must hold the size t5_jpeg_size reports. */
uint16_t w, h;
if (t5_jpeg_size(jpeg, len, &w, &h) == T5_OK) {
    uint16_t *px = malloc((size_t)w * h * 2);
    if (px && t5_jpeg_decode(jpeg, len, px, (size_t)w * h * 2, &w, &h) == T5_OK) {
        s->dsc = (lv_image_dsc_t){ .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                                               .w = w, .h = h, .stride = (uint32_t)w * 2 },
                                   .data_size = (uint32_t)w * h * 2, .data = (const uint8_t *)px };
        /* then, on the UI task: lv_image_set_src(s->img, &s->dsc); */
    }
}
```

If you replace pixels in place behind an `lv_image`, tell LVGL with `lv_image_cache_drop(&dsc)` and invalidate the object. For a video-like stream, decode into the hidden half of two buffers and swap the source, as UVC Monitor does.

## Text entry

Tapping a text area brings up the on-screen keyboard, and the container pans if it would be covered. A USB keyboard replaces it. Call `t5_shell_keyboard_hide()` when you've accepted the input.

## Motion

Follow MD2's timing: `md_anim_set_curve(&a, MD_CURVE_STANDARD)`, 150 to 300 ms. Animate small things. Respect `t5_ui_low_power()` and skip decorative motion when it's true: the OS already drops ripples and caps the display at 30 fps. See [what's cheap in motion](Performance.md#animation).

## Two ways to build a UI

Everything under `t5_app_screen()` is ordinary LVGL 9.6, so you can mix freely:

| Path | Use it for | Example |
| --- | --- | --- |
| MD2 components (`md_*`) | Anything a phone app has: app bars, lists, buttons, switches, sliders, chips, text fields, cards, tabs, dialogs, snackbars, drawers. They follow the theme, the accent colour and dp sizing for you | The Material tab of `apps/examples/ui_gallery` |
| Raw LVGL (`lv_*`) | Whatever MD2 doesn't have (arcs, charts, meters, spinners, rollers, tables), or a look of your own: create LVGL objects and style them with `lv_obj_set_style_*` | The Custom tab |
| Your own widget class | A reusable control that draws itself | The gauge in the Custom tab (below) |
| A canvas | Free drawing into a pixel buffer: plots, sketches, generated images | The Canvas tab |

Take colours from `md_scheme()` and sizes from `T5_DP()` in raw LVGL too, so it matches the rest of the device in both themes.

## Custom widgets

`lv_obj_class_t` is part of the ABI (its layout is locked) and `lv_obj_class` (the base class), `lv_obj_class_create_obj`, `lv_obj_class_init_obj` and `lv_obj_event_base` are exported, so an app can define a widget class. One rule: **leave `instance_size` at 0** and keep your data outside the object. `lv_obj_t`'s own layout isn't part of the ABI, so a struct that embeds it could break on a later OS.

```c
typedef struct { int32_t value; } gauge_t;
static lv_obj_class_t gauge_class;               /* filled in at run time */

static void gauge_ctor(const lv_obj_class_t *c, lv_obj_t *obj) { lv_obj_set_user_data(obj, calloc(1, sizeof(gauge_t))); }
static void gauge_dtor(const lv_obj_class_t *c, lv_obj_t *obj) { free(lv_obj_get_user_data(obj)); }
static void gauge_event(const lv_obj_class_t *c, lv_event_t *e)
{
    if (lv_obj_event_base(&gauge_class, e) != LV_RESULT_OK) return;   /* the base class first */
    if (lv_event_get_code(e) == LV_EVENT_DRAW_MAIN) {
        lv_layer_t *layer = lv_event_get_layer(e);
        /* lv_draw_arc / lv_draw_rect / lv_draw_label / lv_draw_line into layer,
         * inside lv_obj_get_content_coords(obj) */
    }
}

static void on_create(t5_app_t *app)
{
    gauge_class.base_class = &lv_obj_class;
    gauge_class.constructor_cb = gauge_ctor;
    gauge_class.destructor_cb = gauge_dtor;
    gauge_class.event_cb = gauge_event;
    gauge_class.width_def = gauge_class.height_def = T5_DP(160);
    gauge_class.instance_size = 0;               /* lv_obj_t's size: the rule above */
    lv_obj_t *g = lv_obj_class_create_obj(&gauge_class, t5_app_screen(app));
    lv_obj_class_init_obj(g);
}
```

Call `lv_obj_invalidate(obj)` when the data changes so it redraws. The user data is the widget's, so don't also use it for something else on that object. Objects of your class are deleted with your screen like any other, which runs the destructor. The full gauge is in `apps/examples/ui_gallery/main.c`.

## Canvas

```c
lv_draw_buf_t *buf = lv_draw_buf_create(T5_DP(300), T5_DP(160), LV_COLOR_FORMAT_RGB565, 0);
lv_obj_t *cv = lv_canvas_create(t5_app_screen(app));
lv_canvas_set_draw_buf(cv, buf);
lv_canvas_fill_bg(cv, md_color(md_scheme()->c.surface), LV_OPA_COVER);
lv_layer_t layer;
lv_canvas_init_layer(cv, &layer);
lv_draw_line_dsc_t ln;
lv_draw_line_dsc_init(&ln);
ln.p1 = (lv_point_precise_t){ 0, 0 };
ln.p2 = (lv_point_precise_t){ 100, 80 };
lv_draw_line(&layer, &ln);                       /* any lv_draw_* call */
lv_canvas_finish_layer(cv, &layer);
```

The buffer is your app's memory (freed at exit). RGB565 at 300 × 160 dp is 192 KB; keep canvases to the size you show. Redraw from an `lv_timer`, not a task, or take `t5_ui_lock()` first.

## Shell bars

The shell draws the **status bar** and the **gesture bar**, nothing else: there's no title bar, and your screen starts right under the status bar. If you want a title, a back arrow or actions, build them yourself with `md_appbar_create()` and `md_appbar_add_action()`, or an `md_page`.

The **gesture bar** is a handle in a 16 dp strip at the bottom of every app screen, showing where to swipe up to go home. In **full screen** all bars go and your content gets the whole panel; the handle shows over your app for 2.5 s when full screen starts, and whenever a finger rests on the bottom edge (until 2.5 s after it lifts), so people can always find the way out. Design full-screen UIs so the bottom 16 dp can carry a brief overlay.

| Call | Does |
| --- | --- |
| `t5_shell_set_fullscreen(on)`, `t5_shell_fullscreen()` | Full screen on or off (`[shell] fullscreen = true` starts that way) |
| `t5_shell_get_insets(&in)` | What the bars take, in dp; `T5_EV_UI_INSETS` when it changes |
| `t5_shell_activity_start/update/end` | An ongoing activity in the status bar and quick settings ([SDK guide](../sdk.md#status-bar-and-activities)) |
| `t5_ambient_set(&card)`, `t5_ambient_clear()` | Your app's card on the standby screen, which shows on USB power when the device dims. Needs `ambient = true` ([SDK guide](../sdk.md#standby)) |

