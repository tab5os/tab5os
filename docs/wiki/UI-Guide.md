# UI guide

Apps use LVGL 9.6 with a Material Design 2 layer (`md_*`) that already matches the shell. Use `md_*` first and raw LVGL for what it doesn't cover. Include `<t5/ui.h>`.

## Screen and units

| | Landscape | Portrait |
| --- | --- | --- |
| Pixels | 1280 x 720 | 720 x 1280 |
| dp | 640 x 360 | 360 x 640 |

Density is fixed at 2 px per dp. Always write `T5_DP(n)`, so a size means the same thing everywhere. A literal `1` is right only for a one-pixel hairline.

The status bar takes the top 24 dp, as padding on `t5_app_screen()`. `t5_shell_set_fullscreen(true)` gives it back until you leave the foreground. Keep important content away from the edges, since two system gestures start there: swipe down from the top for quick settings and swipe up from the bottom to go home.

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
| `MD_TYPE_BUTTON` | 14, uppercase | `button` | Button labels |
| `MD_TYPE_CAPTION` | 12 | `caption` | Small print |
| `MD_TYPE_OVERLINE` | 12, uppercase | `overline` | Section labels |

`md_font_mono()` is Roboto Mono at 12 dp for code and logs.

**Coverage:** ASCII, Latin-1 (`é`, `°`, `ü`...) and a little punctuation: en and em dashes, curly quotes, bullet, ellipsis and euro. Other characters don't render, and that includes CJK, Cyrillic, Greek, Arabic and emoji.

For other scripts, compile a font with `lv_font_conv` into your app as a `const lv_font_t` and set it with `lv_obj_set_style_text_font`. It takes app memory and loses the role styling, so subset it.

For dynamic text, `lv_label_set_text_fmt` and `snprintf` work. Set text only when it changes, because every set redraws the label ([Performance](Performance.md#updating-widgets)). Uppercase roles need `md_label_set_text(label, text, role)`, which upper-cases for you.

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
