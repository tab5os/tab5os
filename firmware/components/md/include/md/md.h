/* SPDX-License-Identifier: Apache-2.0
 * Material Design 2 on LVGL (plan 4.5–4.8): theme, type, elevation, ripple
 * and the base widgets the shell and apps build on. "Material-inspired":
 * it follows the MD2 spec without being a Google product.
 *
 * All calls are LVGL calls: UI task, or under t5_ui_lock().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "md/md_tokens.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- theme (plan 4.6) ----------------------------------------------------- */
/* Build the theme and install it on disp (dark, purple/teal placeholders). */
void md_init(lv_display_t *disp);
const md_scheme_t *md_scheme(void);
/* Runtime accent switch: restyles every object. */
void md_set_accent(md_accent_t primary, md_accent_t secondary);
md_accent_t md_get_accent(void);
/* Light/dark switch. Built in, hidden in the UI until the light theme is
 * tuned (plan 4.6). */
void md_set_dark(bool dark);
bool md_is_dark(void);

static inline lv_color_t md_color(md_rgb_t c) { return lv_color_hex(c); }

/* ---- type ----------------------------------------------------------------- */
const lv_font_t *md_font(md_type_role_t role);
/* Material Icons font: 18, 24 (default) or 40 dp. */
const lv_font_t *md_icon_font(int size_dp);
/* Roboto Mono, 12 dp: code and terminal text. Not an MD2 role. */
const lv_font_t *md_font_mono(void);
/* Font, line height, tracking and colour for a text role at an emphasis
 * (scheme->on_surface.high / medium / disabled). Uppercase roles need their
 * text set with md_label_set_text(). */
void md_text_style(lv_obj_t *label, md_type_role_t role, lv_opa_t emphasis);
lv_obj_t *md_label_create(lv_obj_t *parent, const char *text, md_type_role_t role);
void md_label_set_text(lv_obj_t *label, const char *text, md_type_role_t role);
lv_obj_t *md_icon_create(lv_obj_t *parent, const char *icon, int size_dp);

/* ---- elevation -------------------------------------------------------------- */
/* Dark: surface colour with the white overlay for dp. Light: plain surface
 * plus a baked shadow. shadow forces a baked shadow on dark too (dialogs,
 * menus, FABs). */
void md_set_elevation(lv_obj_t *obj, int dp, bool shadow);

/* ---- ripple (plan 4.7) ------------------------------------------------------- */
/* Ink circle from the touch point, clipped to the widget; round widgets
 * ripple from their centre. Falls back to a plain fade when the probe says
 * the frame budget is tight, and always in low-power mode. */
void md_ripple_attach(lv_obj_t *obj);
/* Return true when a full ripple is affordable. NULL = always. */
typedef bool (*md_perf_probe_t)(void);
void md_ripple_set_probe(md_perf_probe_t probe);

/* ---- motion ---------------------------------------------------------------- */
void md_anim_set_curve(lv_anim_t *a, md_curve_t curve);

/* ---- widgets (base set for the rendering gate; Phase 6 completes it) -------- */
typedef enum {
    MD_BUTTON_TEXT,
    MD_BUTTON_OUTLINED,
    MD_BUTTON_CONTAINED,
} md_button_variant_t;

lv_obj_t *md_button_create(lv_obj_t *parent, const char *text, md_button_variant_t variant);
lv_obj_t *md_fab_create(lv_obj_t *parent, const char *icon, bool mini);
lv_obj_t *md_card_create(lv_obj_t *parent);

/* Top app bar: full width, dense (48 dp) in landscape. Returns the bar;
 * actions are added from the right. */
lv_obj_t *md_appbar_create(lv_obj_t *parent, const char *title, const char *nav_icon,
                           lv_event_cb_t nav_cb, void *user_data);
lv_obj_t *md_appbar_add_action(lv_obj_t *bar, const char *icon, lv_event_cb_t cb, void *user_data);
void md_appbar_set_title(lv_obj_t *bar, const char *title);
lv_obj_t *md_appbar_title(lv_obj_t *bar);       /* its title label */

/* Recycled list: only the visible rows (plus a few) exist; bind fills a row
 * for an index. Row height 48, 64 or 72 dp. */
typedef struct {
    lv_obj_t *row;
    lv_obj_t *primary;      /* label */
    lv_obj_t *secondary;    /* label, NULL for one-line rows */
    lv_obj_t *icon;         /* leading icon label */
} md_list_row_t;
typedef void (*md_list_bind_cb_t)(md_list_row_t *row, uint32_t index, void *user_data);
typedef void (*md_list_click_cb_t)(uint32_t index, void *user_data);
lv_obj_t *md_list_create(lv_obj_t *parent, uint32_t count, int row_dp, md_list_bind_cb_t bind,
                         md_list_click_cb_t click, void *user_data);
void md_list_set_count(lv_obj_t *list, uint32_t count);
uint32_t md_list_count(lv_obj_t *list);
/* Change the row height (48, 64 or 72 dp): the rows are made again. */
void md_list_set_row_dp(lv_obj_t *list, int row_dp);
/* Called with the row's index on a long press (no click follows). */
void md_list_set_long_press(lv_obj_t *list, md_list_click_cb_t cb);
void md_list_refresh(lv_obj_t *list);
uint32_t md_list_rows_alive(lv_obj_t *list);   /* recycled row objects */

/* Modal dialog with a fading scrim (150 ms in, 75 ms out). cb gets the
 * button index (0 = first) or -1 when dismissed by tapping the scrim. */
typedef void (*md_dialog_cb_t)(int button, void *user_data);
lv_obj_t *md_dialog_show(const char *title, const char *text, const char *btn_confirm,
                         const char *btn_dismiss, md_dialog_cb_t cb, void *user_data);
void md_dialog_close(lv_obj_t *dialog);

/* ---- snapshot compositing (md_freeze.c) -------------------------------------- */
/* A frozen screen is rendered once into a PSRAM snapshot and hidden; the
 * display's bottom layer draws the snapshot where the screen is, at its
 * opacity, until it is thawed. Screen load animations do this to both
 * screens for their duration (on the device every lv_screen_load_anim()
 * call does), and dialogs to the screen behind them while they're open
 * (the scrim and shadow drawn into the snapshot: md_freeze_draw_into). */
bool md_freeze_screen(lv_obj_t *scr);
void md_thaw_screen(lv_obj_t *scr);
void md_thaw_screens(void);
bool md_screens_frozen(void);
void md_screen_load_anim(lv_obj_t *scr, lv_screen_load_anim_t anim, uint32_t time, uint32_t delay, bool auto_del);
/* Show a frozen screen under a scrim of `color` at `opa` (0 = none). */
void md_freeze_dim(lv_obj_t *scr, lv_color_t color, lv_opa_t opa);
/* Draw into a frozen screen's snapshot once (a scrim, a shadow), so every
 * frame draws the snapshot as an opaque image, a PPA blit, instead of
 * redrawing those on top. Drawn by LVGL, so translucent fills and images go
 * to the PPA. False if the screen isn't frozen. */
typedef void (*md_freeze_draw_fn_t)(lv_layer_t *layer, void *arg);
bool md_freeze_draw_into(lv_obj_t *scr, md_freeze_draw_fn_t fn, void *arg);
/* Called when scr goes live again, by whatever thaws it (a transition
 * thaws every frozen screen). Cleared by the thaw. */
typedef void (*md_freeze_thaw_cb_t)(lv_obj_t *scr, void *arg);
void md_freeze_set_thaw_cb(lv_obj_t *scr, md_freeze_thaw_cb_t cb, void *arg);
/* The display's "settled" probe: true when a full-screen change has been
 * copied into every framebuffer. Dialogs start their fade after it, so the
 * fade's frames don't also carry those copies. NULL = always settled. */
void md_freeze_set_settled_probe(md_perf_probe_t probe);
bool md_freeze_settled(void);

/* ---- the rest of the widget set (plan 6.1, 6.2) ------------------------------ */
typedef void (*md_action_cb_t)(void *user_data);

lv_obj_t *md_divider_create(lv_obj_t *parent);

/* Filled text field (56 dp) with a floating label and optional helper or
 * error text. Returns the field; md_textfield_ta() is the lv_textarea in it
 * (focus it to type; the shell shows the on-screen keyboard). */
lv_obj_t *md_textfield_create(lv_obj_t *parent, const char *label, bool password);
lv_obj_t *md_textfield_ta(lv_obj_t *field);
const char *md_textfield_text(lv_obj_t *field);
void md_textfield_set_text(lv_obj_t *field, const char *text);
void md_textfield_set_helper(lv_obj_t *field, const char *text, bool error);
void md_textfield_set_label(lv_obj_t *field, const char *label);
const char *md_textfield_label(lv_obj_t *field);

/* Selection controls, sized to the spec with 48 dp touch targets.
 * LV_EVENT_VALUE_CHANGED as for the plain LVGL widgets. */
lv_obj_t *md_switch_create(lv_obj_t *parent, bool on);
lv_obj_t *md_slider_create(lv_obj_t *parent, int32_t min, int32_t max, int32_t value);
lv_obj_t *md_checkbox_create(lv_obj_t *parent, const char *text, bool checked);

/* Chips (32 dp). Filter chips toggle and show a check; choice chips are
 * exclusive among their siblings. LV_EVENT_VALUE_CHANGED on change,
 * LV_EVENT_CLICKED always. */
typedef enum { MD_CHIP_ACTION, MD_CHIP_FILTER, MD_CHIP_CHOICE } md_chip_kind_t;
lv_obj_t *md_chip_create(lv_obj_t *parent, const char *text, const char *icon, md_chip_kind_t kind);
bool md_chip_selected(lv_obj_t *chip);
void md_chip_set_selected(lv_obj_t *chip, bool on);

/* Tabs (48 dp bar) over swipeable pages. md_tabs_add returns the page. */
lv_obj_t *md_tabs_create(lv_obj_t *parent);
lv_obj_t *md_tabs_add(lv_obj_t *tabs, const char *name);

/* Static list rows for settings-style pages (48 dp, or 64 dp with a
 * secondary line). Not recycled: use md_list for long lists. */
lv_obj_t *md_row_create(lv_obj_t *parent, const char *icon, const char *primary, const char *secondary);
void md_row_set_clickable(lv_obj_t *row, lv_event_cb_t cb, void *user_data);
void md_row_set_text(lv_obj_t *row, const char *primary);
void md_row_set_secondary(lv_obj_t *row, const char *text);
void md_row_set_icon(lv_obj_t *row, const char *icon);   /* adds one to a row made without */
lv_obj_t *md_row_label(lv_obj_t *row, bool secondary);
lv_obj_t *md_row_icon(lv_obj_t *row);           /* NULL if it has none */
lv_obj_t *md_row_switch(lv_obj_t *row, bool on);               /* trailing switch */
lv_obj_t *md_row_trailing_text(lv_obj_t *row, const char *text);
lv_obj_t *md_section_create(lv_obj_t *parent, const char *title);   /* overline header */

/* A full-size page: app bar (back arrow when back_cb is set) over a
 * scrolling column, md_page_body(). */
lv_obj_t *md_page_create(lv_obj_t *parent, const char *title, lv_event_cb_t back_cb, void *user_data,
                         lv_obj_t **bar_out);
lv_obj_t *md_page_body(lv_obj_t *page);

/* Menu (elevation 8) under an anchor, kept on screen. cb gets the item
 * index, or -1 when dismissed by tapping outside. */
typedef void (*md_menu_cb_t)(int item, void *user_data);
lv_obj_t *md_menu_show(lv_obj_t *anchor, const char *const *items, int n, md_menu_cb_t cb, void *user_data);
void md_menu_close(lv_obj_t *menu);

/* Snackbar: one at a time at the bottom, 4 s (10 s with an action). */
lv_obj_t *md_snackbar_show(const char *text, const char *action, md_action_cb_t cb, void *user_data);
void md_snackbar_dismiss(void);

/* Navigation drawer (256 dp, elevation 16) sliding in from the left over a
 * scrim. Add items, or put anything in md_drawer_panel(). Tapping an item or
 * the scrim closes it. */
lv_obj_t *md_drawer_open(const char *title, const char *subtitle);
lv_obj_t *md_drawer_panel(lv_obj_t *drawer);
lv_obj_t *md_drawer_add_item(lv_obj_t *drawer, const char *icon, const char *text, bool selected, md_action_cb_t cb,
                             void *user_data);
void md_drawer_close(lv_obj_t *drawer);

#ifdef __cplusplus
}
#endif
