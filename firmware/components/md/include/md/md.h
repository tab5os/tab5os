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
/* Statistics for the rendering gate: presses and how many used the fallback. */
void md_ripple_stats(uint32_t *presses, uint32_t *fallbacks);

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
void md_list_refresh(lv_obj_t *list);
uint32_t md_list_rows_alive(lv_obj_t *list);   /* recycled row objects */

/* Modal dialog with a fading scrim (150 ms in, 75 ms out). cb gets the
 * button index (0 = first) or -1 when dismissed by tapping the scrim. */
typedef void (*md_dialog_cb_t)(int button, void *user_data);
lv_obj_t *md_dialog_show(const char *title, const char *text, const char *btn_confirm,
                         const char *btn_dismiss, md_dialog_cb_t cb, void *user_data);
void md_dialog_close(lv_obj_t *dialog);

/* ---- rendering gate (plan 4.8) ---------------------------------------------- */
/* The test screen from the design: app bar, 100-row list, card grid, FAB and
 * an open dialog. Returns the screen (not loaded). */
lv_obj_t *md_gate_create(void);
/* Scripted load for measurements, driven by a timer:
 *   MD_GATE_SCROLL      scroll the list up and down continuously
 *   MD_GATE_TRANSITION  slide between two full screens continuously
 *   MD_GATE_DIALOG      open and close the dialog continuously
 *   MD_GATE_IDLE        stop */
typedef enum { MD_GATE_IDLE, MD_GATE_SCROLL, MD_GATE_TRANSITION, MD_GATE_DIALOG } md_gate_mode_t;
void md_gate_run(lv_obj_t *gate, md_gate_mode_t mode);
const char *md_gate_mode_name(md_gate_mode_t m);

#ifdef __cplusplus
}
#endif
