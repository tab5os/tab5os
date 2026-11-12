/* SPDX-License-Identifier: Apache-2.0
 * t5ui: the app UI API (API level 2; in the SDK through t5/t5ui.h).
 *
 * Everything an app compiles in from here is Tab5 OS's own: opaque
 * handles, plain integers for colours (0xRRGGBB) and opacity (0..255), and
 * Tab5 enums. The OS draws with LVGL (and its Material widgets, md), but
 * no LVGL type, macro or struct layout reaches an app through this header,
 * so the OS can move to another LVGL major version (or another renderer)
 * and apps built against t5ui keep working unchanged. Native apps built
 * against level 1 call LVGL 9 directly; those calls stay frozen at LVGL 9
 * and keep working through a compatibility layer, but new apps should use
 * t5ui only (docs/t5ui.md).
 *
 * Threads: like LVGL, everything here runs on the UI task: in the app's
 * callbacks (on_create, events, timers), or through t5_ui_async from
 * another task. Objects belong to the app: they go when the app exits.
 *
 * Coordinates are pixels (t5ui_dp converts density-independent units). A
 * size may also be T5UI_PCT(n), a percentage of the parent's content area,
 * or T5UI_CONTENT, fit to the children or text.
 */
#pragma once
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- basic types ------------------------------------------------------------ */
typedef struct t5ui_obj t5ui_obj_t;          /* any element on screen */
typedef struct t5ui_event t5ui_event_t;      /* valid during its callback only */
typedef struct t5ui_timer t5ui_timer_t;
typedef struct t5ui_paint t5ui_paint_t;      /* a drawing target, during its callback only */
typedef uint32_t t5ui_color_t;               /* 0xRRGGBB */
typedef uint8_t t5ui_opa_t;                  /* 0 transparent .. 255 opaque */

#define T5UI_RGB(r, g, b) ((t5ui_color_t)(((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b)))
#define T5UI_OPA_COVER 255
#define T5UI_OPA_TRANSP 0

/* Sizes: pixels, or one of these. */
#define T5UI_PCT(n) ((int32_t)(0x40000000 | ((n) & 0xffff)))    /* 0..1000 percent of the parent */
#define T5UI_CONTENT ((int32_t)0x7ffffffe)                      /* fit the content */
#define T5UI_RADIUS_CIRCLE 0x7fff

typedef struct {
    int32_t x1, y1, x2, y2;                  /* inclusive */
} t5ui_rect_t;

/* Density-independent units to pixels (2 px per dp on the Tab5). */
int32_t t5ui_dp(int32_t dp);
/* The display in pixels, as the app sees it (rotation applied). */
void t5ui_screen_size(int32_t *w, int32_t *h);
uint32_t t5ui_ticks_ms(void);
/* Run fn(arg) on the UI task soon (from any task): how other tasks touch
 * the UI. T5_ERR_NO_MEM if it can't be queued. */
typedef void (*t5ui_fn_t)(void *arg);
t5_err_t t5ui_async(t5ui_fn_t fn, void *arg);

/* Every finger on the screen for the app in front, in screen pixels, with
 * an id that stays the same while that finger is down. Returns how many
 * (0..T5UI_TOUCH_MAX); 0 when the app isn't the resumed foreground app. */
typedef struct {
    int16_t x, y;
    uint8_t id;
} t5ui_touch_t;
#define T5UI_TOUCH_MAX 5
int t5ui_touches(t5ui_touch_t *out, int max);

/* ---- theme -------------------------------------------------------------------- */
typedef enum {
    T5UI_COLOR_PRIMARY,
    T5UI_COLOR_PRIMARY_VARIANT,
    T5UI_COLOR_SECONDARY,
    T5UI_COLOR_BACKGROUND,
    T5UI_COLOR_SURFACE,
    T5UI_COLOR_ERROR,
    T5UI_COLOR_ON_PRIMARY,
    T5UI_COLOR_ON_SECONDARY,
    T5UI_COLOR_ON_BACKGROUND,
    T5UI_COLOR_ON_SURFACE,
    T5UI_COLOR_ON_ERROR,
    T5UI_COLOR_COUNT,
} t5ui_color_role_t;
/* The current theme's colour for role (follows dark mode and the accent). */
t5ui_color_t t5ui_theme_color(t5ui_color_role_t role);
bool t5ui_theme_dark(void);
/* Text emphasis on a surface: high, medium, disabled (0..255 opacity). */
typedef enum { T5UI_EMPHASIS_HIGH, T5UI_EMPHASIS_MEDIUM, T5UI_EMPHASIS_DISABLED } t5ui_emphasis_t;
t5ui_opa_t t5ui_theme_emphasis(t5ui_emphasis_t e);
/* fg over bg at opa. */
t5ui_color_t t5ui_mix(t5ui_color_t fg, t5ui_color_t bg, t5ui_opa_t opa);

/* Text styles (fonts, sizes, weights and letter spacing come with them). */
typedef enum {
    T5UI_TEXT_HEADLINE4,
    T5UI_TEXT_HEADLINE5,
    T5UI_TEXT_HEADLINE6,
    T5UI_TEXT_SUBTITLE1,
    T5UI_TEXT_BODY1,
    T5UI_TEXT_BODY2,
    T5UI_TEXT_BUTTON,
    T5UI_TEXT_CAPTION,
    T5UI_TEXT_OVERLINE,
    T5UI_TEXT_MONO,                          /* fixed width, body size */
    T5UI_TEXT_COUNT,
} t5ui_text_t;

/* ---- the object tree ---------------------------------------------------------- */
/* The app's screen (from on_create on). */
struct t5_app;
t5ui_obj_t *t5ui_root(struct t5_app *app);
/* A plain container: no background, border or padding. */
t5ui_obj_t *t5ui_box(t5ui_obj_t *parent);
/* A surface: the theme's surface colour, rounded, padded. */
t5ui_obj_t *t5ui_panel(t5ui_obj_t *parent);
void t5ui_delete(t5ui_obj_t *obj);
void t5ui_clean(t5ui_obj_t *obj);            /* delete the children */
t5ui_obj_t *t5ui_parent(t5ui_obj_t *obj);
t5ui_obj_t *t5ui_child(t5ui_obj_t *obj, int32_t index);   /* negative: from the end; NULL if none */
uint32_t t5ui_child_count(t5ui_obj_t *obj);
void t5ui_set_user(t5ui_obj_t *obj, void *user);
void *t5ui_user(t5ui_obj_t *obj);

/* ---- geometry and layout ------------------------------------------------------ */
void t5ui_size(t5ui_obj_t *obj, int32_t w, int32_t h);
void t5ui_width(t5ui_obj_t *obj, int32_t w);
void t5ui_height(t5ui_obj_t *obj, int32_t h);
void t5ui_min_size(t5ui_obj_t *obj, int32_t min_w, int32_t min_h);   /* 0: none */
void t5ui_pos(t5ui_obj_t *obj, int32_t x, int32_t y);
typedef enum {
    T5UI_ALIGN_TOP_LEFT, T5UI_ALIGN_TOP_MID, T5UI_ALIGN_TOP_RIGHT,
    T5UI_ALIGN_LEFT_MID, T5UI_ALIGN_CENTER, T5UI_ALIGN_RIGHT_MID,
    T5UI_ALIGN_BOTTOM_LEFT, T5UI_ALIGN_BOTTOM_MID, T5UI_ALIGN_BOTTOM_RIGHT,
} t5ui_align_t;
/* Place obj in its parent (outside any flex layout). */
void t5ui_align(t5ui_obj_t *obj, t5ui_align_t where, int32_t dx, int32_t dy);
void t5ui_center(t5ui_obj_t *obj);
/* Where obj is on screen, and its content area (after padding). Lays out
 * first if anything changed. */
void t5ui_coords(t5ui_obj_t *obj, t5ui_rect_t *out);
void t5ui_content_coords(t5ui_obj_t *obj, t5ui_rect_t *out);

/* Flex layout: children in a row or column. */
typedef enum { T5UI_ROW, T5UI_COLUMN, T5UI_ROW_WRAP, T5UI_COLUMN_WRAP } t5ui_flow_t;
typedef enum {
    T5UI_PLACE_START, T5UI_PLACE_END, T5UI_PLACE_CENTER,
    T5UI_PLACE_SPACE_EVENLY, T5UI_PLACE_SPACE_AROUND, T5UI_PLACE_SPACE_BETWEEN,
} t5ui_place_t;
void t5ui_flex(t5ui_obj_t *obj, t5ui_flow_t flow);
/* main: along the flow; cross: across it; track: of the lines when wrapping. */
void t5ui_flex_place(t5ui_obj_t *obj, t5ui_place_t main, t5ui_place_t cross, t5ui_place_t track);
/* Share the free space along the flow among children with grow > 0. */
void t5ui_grow(t5ui_obj_t *obj, uint8_t grow);
void t5ui_pad(t5ui_obj_t *obj, int32_t top, int32_t right, int32_t bottom, int32_t left);
void t5ui_pad_all(t5ui_obj_t *obj, int32_t pad);
void t5ui_gap(t5ui_obj_t *obj, int32_t row_gap, int32_t column_gap);
void t5ui_margin(t5ui_obj_t *obj, int32_t top, int32_t right, int32_t bottom, int32_t left);
/* Lay out now (coordinates are otherwise settled before the next frame). */
void t5ui_layout_now(t5ui_obj_t *obj);

/* ---- appearance --------------------------------------------------------------- */
/* Drop every style, theme included (a bare box). */
void t5ui_unstyle(t5ui_obj_t *obj);
void t5ui_bg(t5ui_obj_t *obj, t5ui_color_t color, t5ui_opa_t opa);
void t5ui_border(t5ui_obj_t *obj, int32_t width, t5ui_color_t color, t5ui_opa_t opa);
void t5ui_radius(t5ui_obj_t *obj, int32_t radius);
void t5ui_shadow(t5ui_obj_t *obj, int32_t width, t5ui_color_t color, t5ui_opa_t opa);
/* Material elevation: shadow (and the dark theme's surface tint). */
void t5ui_elevation(t5ui_obj_t *obj, int32_t dp);
void t5ui_opacity(t5ui_obj_t *obj, t5ui_opa_t opa);
void t5ui_text_color(t5ui_obj_t *obj, t5ui_color_t color, t5ui_opa_t opa);
void t5ui_text_style(t5ui_obj_t *obj, t5ui_text_t style);
typedef enum { T5UI_TEXT_LEFT, T5UI_TEXT_CENTER, T5UI_TEXT_RIGHT } t5ui_text_align_t;
void t5ui_text_align(t5ui_obj_t *obj, t5ui_text_align_t a);
/* Line, arc and chart strokes. */
void t5ui_stroke(t5ui_obj_t *obj, int32_t width, t5ui_color_t color, t5ui_opa_t opa);

/* ---- state -------------------------------------------------------------------- */
void t5ui_hide(t5ui_obj_t *obj, bool hidden);
bool t5ui_hidden(t5ui_obj_t *obj);
void t5ui_clickable(t5ui_obj_t *obj, bool on);
typedef enum { T5UI_SCROLL_NONE, T5UI_SCROLL_HOR, T5UI_SCROLL_VER, T5UI_SCROLL_ALL } t5ui_scroll_t;
void t5ui_scrollable(t5ui_obj_t *obj, t5ui_scroll_t dirs);
void t5ui_scroll_to(t5ui_obj_t *obj, int32_t x, int32_t y, bool animate);
/* Make obj visible in its scrolling ancestors. */
void t5ui_scroll_into_view(t5ui_obj_t *obj, bool animate);
void t5ui_set_checked(t5ui_obj_t *obj, bool on);       /* switches, checkboxes, toggle buttons */
bool t5ui_checked(t5ui_obj_t *obj);
void t5ui_set_disabled(t5ui_obj_t *obj, bool on);
bool t5ui_disabled(t5ui_obj_t *obj);
/* Redraw obj on the next frame (after the app changed what it draws). */
void t5ui_invalidate(t5ui_obj_t *obj);

/* ---- events ------------------------------------------------------------------- */
typedef enum {
    T5UI_EV_CLICK,                           /* pressed and released on it */
    T5UI_EV_LONG_PRESS,
    T5UI_EV_PRESS,
    T5UI_EV_RELEASE,
    T5UI_EV_VALUE,                           /* a switch, slider, text field... changed */
    T5UI_EV_FOCUS,
    T5UI_EV_DEFOCUS,
    T5UI_EV_SCROLL,
    T5UI_EV_RESIZE,
    T5UI_EV_DELETE,                          /* obj is about to go: free what user points at */
    T5UI_EV_DRAW,                            /* draw on top of obj: t5ui_event_paint() */
    T5UI_EV_COUNT,
} t5ui_ev_t;
typedef void (*t5ui_event_cb_t)(t5ui_event_t *e);
/* cb(e) when ev happens to obj. Several may be added; each runs once per event. */
t5_err_t t5ui_on(t5ui_obj_t *obj, t5ui_ev_t ev, t5ui_event_cb_t cb, void *user);
t5ui_obj_t *t5ui_event_obj(t5ui_event_t *e);
void *t5ui_event_user(t5ui_event_t *e);
t5ui_ev_t t5ui_event_kind(t5ui_event_t *e);
/* Where the pointer is (screen pixels), for press and click events. */
bool t5ui_event_point(t5ui_event_t *e, int32_t *x, int32_t *y);
/* For T5UI_EV_DRAW: where to draw (coordinates relative to obj's top left). */
t5ui_paint_t *t5ui_event_paint(t5ui_event_t *e);

/* ---- timers ------------------------------------------------------------------- */
typedef void (*t5ui_timer_cb_t)(t5ui_timer_t *t, void *user);
/* Every period_ms on the UI task until deleted (or the app exits). */
t5ui_timer_t *t5ui_timer(uint32_t period_ms, t5ui_timer_cb_t cb, void *user);
void t5ui_timer_period(t5ui_timer_t *t, uint32_t period_ms);
void t5ui_timer_delete(t5ui_timer_t *t);

/* ---- text --------------------------------------------------------------------- */
t5ui_obj_t *t5ui_label(t5ui_obj_t *parent, const char *text, t5ui_text_t style);
void t5ui_label_set(t5ui_obj_t *label, const char *text);   /* copied */
void t5ui_label_setf(t5ui_obj_t *label, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
typedef enum { T5UI_LONG_WRAP, T5UI_LONG_DOTS, T5UI_LONG_SCROLL, T5UI_LONG_CLIP } t5ui_long_t;
/* What a label too long for its width does (give it a width first). */
void t5ui_label_long(t5ui_obj_t *label, t5ui_long_t mode);
/* The size text takes in style, wrapped at max_w (0: one line). */
void t5ui_text_size(const char *text, t5ui_text_t style, int32_t max_w, int32_t *w, int32_t *h);
/* Material icons by name ("T5UI_ICON_*" strings in t5ui_icons.h), at size_dp. */
t5ui_obj_t *t5ui_icon(t5ui_obj_t *parent, const char *icon, int32_t size_dp);

/* ---- Material widgets ----------------------------------------------------------- */
typedef enum { T5UI_BUTTON_TEXT, T5UI_BUTTON_OUTLINED, T5UI_BUTTON_CONTAINED } t5ui_button_t;
t5ui_obj_t *t5ui_button(t5ui_obj_t *parent, const char *text, t5ui_button_t kind);
/* A floating action button (mini: the small one). */
t5ui_obj_t *t5ui_fab(t5ui_obj_t *parent, const char *icon, bool mini);
t5ui_obj_t *t5ui_card(t5ui_obj_t *parent);
t5ui_obj_t *t5ui_divider(t5ui_obj_t *parent);
/* The top app bar: title, a navigation icon (NULL: none) and its click. */
t5ui_obj_t *t5ui_appbar(t5ui_obj_t *parent, const char *title, const char *nav_icon, t5ui_event_cb_t nav, void *user);
t5ui_obj_t *t5ui_appbar_action(t5ui_obj_t *bar, const char *icon, t5ui_event_cb_t cb, void *user);
void t5ui_appbar_title(t5ui_obj_t *bar, const char *title);
/* A list row: leading icon (NULL: none), primary and secondary text. */
t5ui_obj_t *t5ui_row(t5ui_obj_t *parent, const char *icon, const char *primary, const char *secondary);
void t5ui_row_text(t5ui_obj_t *row, const char *primary, const char *secondary);   /* NULL: leave */
void t5ui_row_on_click(t5ui_obj_t *row, t5ui_event_cb_t cb, void *user);
t5ui_obj_t *t5ui_row_switch(t5ui_obj_t *row, bool on);         /* a trailing switch */
t5ui_obj_t *t5ui_row_trailing(t5ui_obj_t *row, const char *text);
t5ui_obj_t *t5ui_section(t5ui_obj_t *parent, const char *title);
t5ui_obj_t *t5ui_switch(t5ui_obj_t *parent, bool on);
t5ui_obj_t *t5ui_checkbox(t5ui_obj_t *parent, const char *text, bool checked);
t5ui_obj_t *t5ui_slider(t5ui_obj_t *parent, int32_t min, int32_t max, int32_t value);
t5ui_obj_t *t5ui_arc(t5ui_obj_t *parent, int32_t min, int32_t max, int32_t value);   /* a dial */
t5ui_obj_t *t5ui_bar(t5ui_obj_t *parent, int32_t min, int32_t max, int32_t value);   /* progress */
/* Sliders, arcs and bars. */
void t5ui_set_value(t5ui_obj_t *obj, int32_t value, bool animate);
int32_t t5ui_value(t5ui_obj_t *obj);
/* A labelled text field (Material), or a plain text area. */
t5ui_obj_t *t5ui_textfield(t5ui_obj_t *parent, const char *label, bool password);
void t5ui_textfield_helper(t5ui_obj_t *field, const char *text, bool error);
t5ui_obj_t *t5ui_textarea(t5ui_obj_t *parent, bool one_line, const char *placeholder);
/* Text fields and text areas: the text (valid until it changes). */
const char *t5ui_text(t5ui_obj_t *obj);
void t5ui_set_text(t5ui_obj_t *obj, const char *text);
typedef enum { T5UI_CHIP_ACTION, T5UI_CHIP_FILTER, T5UI_CHIP_CHOICE } t5ui_chip_t;
t5ui_obj_t *t5ui_chip(t5ui_obj_t *parent, const char *text, const char *icon, t5ui_chip_t kind);
/* Tabs: add returns the page that shows under that tab. */
t5ui_obj_t *t5ui_tabs(t5ui_obj_t *parent);
t5ui_obj_t *t5ui_tabs_add(t5ui_obj_t *tabs, const char *name);
/* A full-screen page with an app bar and a back arrow; put content in body. */
t5ui_obj_t *t5ui_page(t5ui_obj_t *parent, const char *title, t5ui_event_cb_t back, void *user);
t5ui_obj_t *t5ui_page_body(t5ui_obj_t *page);
t5ui_obj_t *t5ui_page_bar(t5ui_obj_t *page);

/* A long list that recycles its rows: bind fills row index's texts. */
typedef void (*t5ui_list_bind_cb_t)(t5ui_obj_t *row, uint32_t index, void *user);
typedef void (*t5ui_list_click_cb_t)(uint32_t index, void *user);
t5ui_obj_t *t5ui_list(t5ui_obj_t *parent, uint32_t count, int32_t row_dp, t5ui_list_bind_cb_t bind,
                      t5ui_list_click_cb_t click, void *user);
void t5ui_list_count(t5ui_obj_t *list, uint32_t count);
void t5ui_list_refresh(t5ui_obj_t *list);

/* Modal and transient. */
typedef void (*t5ui_answer_cb_t)(int choice, void *user);   /* 0 first button, 1 second, -1 dismissed */
t5ui_obj_t *t5ui_dialog(const char *title, const char *text, const char *confirm, const char *dismiss,
                        t5ui_answer_cb_t cb, void *user);
void t5ui_dialog_close(t5ui_obj_t *dialog);
t5ui_obj_t *t5ui_menu(t5ui_obj_t *anchor, const char *const *items, int n, t5ui_answer_cb_t cb, void *user);
typedef void (*t5ui_action_cb_t)(void *user);
void t5ui_snackbar(const char *text, const char *action, t5ui_action_cb_t cb, void *user);

/* ---- images -------------------------------------------------------------------- */
typedef enum { T5UI_PX_RGB565, T5UI_PX_RGB888, T5UI_PX_ARGB8888, T5UI_PX_A8 } t5ui_px_t;
typedef struct {
    uint32_t size;                           /* sizeof(t5ui_pixels_t) */
    uint16_t w, h;
    uint32_t stride;                         /* bytes per row, 0: packed */
    uint8_t format;                          /* t5ui_px_t */
    const void *data;
} t5ui_pixels_t;
t5ui_obj_t *t5ui_image(t5ui_obj_t *parent);
/* Show pixels (copied: the app's buffer may go). Decode compressed images
 * first (t5/camera.h decodes JPEG). */
t5_err_t t5ui_image_pixels(t5ui_obj_t *img, const t5ui_pixels_t *px);

/* ---- drawing ------------------------------------------------------------------- */
/* A canvas keeps what is drawn on it (an OS-owned w x h buffer). */
t5ui_obj_t *t5ui_canvas(t5ui_obj_t *parent, int32_t w, int32_t h);
void t5ui_canvas_fill(t5ui_obj_t *canvas, t5ui_color_t color, t5ui_opa_t opa);
/* Draw into the canvas between begin and end. */
t5ui_paint_t *t5ui_canvas_begin(t5ui_obj_t *canvas);
void t5ui_canvas_end(t5ui_obj_t *canvas, t5ui_paint_t *p);
/* Drawing on a paint target (a canvas, or a T5UI_EV_DRAW): coordinates are
 * relative to its top left. */
void t5ui_draw_line(t5ui_paint_t *p, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t width,
                    t5ui_color_t color, t5ui_opa_t opa);
void t5ui_draw_rect(t5ui_paint_t *p, const t5ui_rect_t *r, int32_t radius, t5ui_color_t fill, t5ui_opa_t fill_opa,
                    int32_t border_w, t5ui_color_t border, t5ui_opa_t border_opa);
/* An arc around (cx, cy), angles in degrees clockwise from 3 o'clock. */
void t5ui_draw_arc(t5ui_paint_t *p, int32_t cx, int32_t cy, int32_t radius, int32_t start_deg, int32_t end_deg,
                   int32_t width, t5ui_color_t color, t5ui_opa_t opa, bool rounded);
void t5ui_draw_text(t5ui_paint_t *p, const t5ui_rect_t *r, const char *text, t5ui_text_t style, t5ui_color_t color,
                    t5ui_text_align_t align);
void t5ui_draw_pixels(t5ui_paint_t *p, int32_t x, int32_t y, const t5ui_pixels_t *px);

/* ---- charts -------------------------------------------------------------------- */
typedef enum { T5UI_CHART_LINE, T5UI_CHART_BAR } t5ui_chart_t;
t5ui_obj_t *t5ui_chart(t5ui_obj_t *parent, t5ui_chart_t kind, uint32_t points);
/* A series in color: its index (0..7), or a negative t5_err_t. */
int t5ui_chart_series(t5ui_obj_t *chart, t5ui_color_t color);
void t5ui_chart_range(t5ui_obj_t *chart, int32_t min, int32_t max);
/* Shift the series left and add value on the right. */
void t5ui_chart_push(t5ui_obj_t *chart, int series, int32_t value);
void t5ui_chart_set(t5ui_obj_t *chart, int series, uint32_t index, int32_t value);
void t5ui_chart_points(t5ui_obj_t *chart, uint32_t points);
void t5ui_chart_grid(t5ui_obj_t *chart, uint8_t rows, uint8_t columns);

#ifdef __cplusplus
}
#endif
