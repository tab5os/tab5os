/* SPDX-License-Identifier: Apache-2.0
 * t5ui (t5_gui/t5ui.h) on LVGL 9 and md. This file is the only place where
 * the app UI API meets LVGL: moving to another LVGL means rewriting this,
 * not the apps.
 *
 * Handles are the LVGL objects themselves, cast: apps never look inside.
 * Callbacks reach the app through small records (cb, user) registered as
 * LVGL event user data; each is freed when its object is deleted. Buffers
 * the OS allocates for an app's object (images, canvases) go the same way.
 * Timers are the app's resource (t5_res), so they end with it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "misc/cache/instance/lv_image_cache.h"   /* lv_image_cache_drop: not in lvgl.h */
#include "md/md.h"
#include "t5_app/app.h"
#include "t5_core/res.h"
#include "t5_gui/t5ui.h"
#include "t5_ui/t5_ui.h"   /* t5_ui_async, t5_ui_touch_get */

#define O(o) ((lv_obj_t *)(o))
#define H(o) ((t5ui_obj_t *)(o))

/* ---- conversions ---------------------------------------------------------------- */
static int32_t sz(int32_t v)
{
    if (v == T5UI_CONTENT) return LV_SIZE_CONTENT;
    if (v >= 0x40000000 && v < 0x40010000) return lv_pct(v & 0xffff);
    return v;
}

static lv_color_t col(t5ui_color_t c)
{
    return lv_color_hex(c & 0xffffff);
}

static const lv_font_t *font(t5ui_text_t t)
{
    if (t == T5UI_TEXT_MONO) return md_font_mono();
    return md_font((md_type_role_t)((unsigned)t < MD_TYPE_COUNT ? t : T5UI_TEXT_BODY1));
}

_Static_assert((int)T5UI_TEXT_HEADLINE4 == (int)MD_TYPE_HEADLINE4 && (int)T5UI_TEXT_OVERLINE == (int)MD_TYPE_OVERLINE,
               "t5ui text styles follow md's type roles");

/* ---- basics --------------------------------------------------------------------- */
int32_t t5ui_dp(int32_t dp)
{
    return T5_DP(dp);
}

void t5ui_screen_size(int32_t *w, int32_t *h)
{
    if (w) *w = lv_display_get_horizontal_resolution(NULL);
    if (h) *h = lv_display_get_vertical_resolution(NULL);
}

uint32_t t5ui_ticks_ms(void)
{
    return lv_tick_get();
}

t5_err_t t5ui_async(t5ui_fn_t fn, void *arg)
{
    return fn ? t5_ui_async(fn, arg) : T5_ERR_INVALID_ARG;
}

_Static_assert(sizeof(t5ui_touch_t) == sizeof(t5_ui_touch_t) && T5UI_TOUCH_MAX == T5_UI_TOUCH_MAX,
               "t5ui_touch_t is t5_ui_touch_t");

int t5ui_touches(t5ui_touch_t *out, int max)
{
    return t5_ui_touch_get((t5_ui_touch_t *)out, max);
}

t5ui_color_t t5ui_theme_color(t5ui_color_role_t role)
{
    const md_colors_t *c = &md_scheme()->c;
    switch (role) {
    case T5UI_COLOR_PRIMARY: return c->primary;
    case T5UI_COLOR_PRIMARY_VARIANT: return c->primary_variant;
    case T5UI_COLOR_SECONDARY: return c->secondary;
    case T5UI_COLOR_BACKGROUND: return c->background;
    case T5UI_COLOR_SURFACE: return c->surface;
    case T5UI_COLOR_ERROR: return c->error;
    case T5UI_COLOR_ON_PRIMARY: return c->on_primary;
    case T5UI_COLOR_ON_SECONDARY: return c->on_secondary;
    case T5UI_COLOR_ON_BACKGROUND: return c->on_background;
    case T5UI_COLOR_ON_SURFACE: return c->on_surface;
    case T5UI_COLOR_ON_ERROR: return c->on_error;
    default: return c->on_surface;
    }
}

bool t5ui_theme_dark(void)
{
    return md_scheme()->dark;
}

t5ui_opa_t t5ui_theme_emphasis(t5ui_emphasis_t e)
{
    const md_emphasis_t *m = &md_scheme()->on_surface;
    return e == T5UI_EMPHASIS_MEDIUM ? m->medium : e == T5UI_EMPHASIS_DISABLED ? m->disabled : m->high;
}

t5ui_color_t t5ui_mix(t5ui_color_t fg, t5ui_color_t bg, t5ui_opa_t opa)
{
    return md_mix(fg, bg, opa);
}

/* ---- callback records ------------------------------------------------------------ */
struct t5ui_event {
    lv_event_t *e;
    void *user;
    t5ui_ev_t kind;
    t5ui_paint_t *paint;
};

struct t5ui_paint {
    lv_layer_t *layer;
    int32_t ox, oy;           /* the target's top left on the layer */
};

typedef struct {
    t5ui_event_cb_t cb;
    void *user;
    t5ui_ev_t kind;
} rec_t;

static const lv_event_code_t CODES[T5UI_EV_COUNT] = {
    [T5UI_EV_CLICK] = LV_EVENT_CLICKED,       [T5UI_EV_LONG_PRESS] = LV_EVENT_LONG_PRESSED,
    [T5UI_EV_PRESS] = LV_EVENT_PRESSED,       [T5UI_EV_RELEASE] = LV_EVENT_RELEASED,
    [T5UI_EV_VALUE] = LV_EVENT_VALUE_CHANGED, [T5UI_EV_FOCUS] = LV_EVENT_FOCUSED,
    [T5UI_EV_DEFOCUS] = LV_EVENT_DEFOCUSED,   [T5UI_EV_SCROLL] = LV_EVENT_SCROLL,
    [T5UI_EV_RESIZE] = LV_EVENT_SIZE_CHANGED, [T5UI_EV_DELETE] = LV_EVENT_DELETE,
    [T5UI_EV_DRAW] = LV_EVENT_DRAW_POST,
};

static void free_user(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

/* Free p when obj goes. */
static void free_with(lv_obj_t *obj, void *p)
{
    lv_obj_add_event_cb(obj, free_user, LV_EVENT_DELETE, p);
}

static void tramp(lv_event_t *e)
{
    rec_t *r = lv_event_get_user_data(e);
    t5ui_event_t ev = { .e = e, .user = r->user, .kind = r->kind };
    t5ui_paint_t paint;
    if (r->kind == T5UI_EV_DRAW) {
        lv_obj_t *obj = lv_event_get_current_target_obj(e);
        lv_area_t a;
        lv_obj_get_coords(obj, &a);
        paint = (t5ui_paint_t){ lv_event_get_layer(e), a.x1, a.y1 };
        ev.paint = &paint;
    }
    r->cb(&ev);
    if (r->kind == T5UI_EV_DELETE) free(r);    /* its own record goes last */
}

static rec_t *rec_new(t5ui_event_cb_t cb, void *user, t5ui_ev_t kind)
{
    rec_t *r = malloc(sizeof *r);
    if (r) *r = (rec_t){ cb, user, kind };
    return r;
}

t5_err_t t5ui_on(t5ui_obj_t *obj, t5ui_ev_t ev, t5ui_event_cb_t cb, void *user)
{
    if (!obj || !cb || (unsigned)ev >= T5UI_EV_COUNT) return T5_ERR_INVALID_ARG;
    rec_t *r = rec_new(cb, user, ev);
    if (!r) return T5_ERR_NO_MEM;
    lv_obj_add_event_cb(O(obj), tramp, CODES[ev], r);
    if (ev != T5UI_EV_DELETE) free_with(O(obj), r);
    if (ev == T5UI_EV_DRAW) lv_obj_invalidate(O(obj));
    return T5_OK;
}

t5ui_obj_t *t5ui_event_obj(t5ui_event_t *e)
{
    return e ? H(lv_event_get_current_target_obj(e->e)) : NULL;
}

void *t5ui_event_user(t5ui_event_t *e)
{
    return e ? e->user : NULL;
}

t5ui_ev_t t5ui_event_kind(t5ui_event_t *e)
{
    return e ? e->kind : T5UI_EV_COUNT;
}

bool t5ui_event_point(t5ui_event_t *e, int32_t *x, int32_t *y)
{
    lv_indev_t *in = lv_indev_active();
    if (!e || !in) return false;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    if (x) *x = p.x;
    if (y) *y = p.y;
    return true;
}

t5ui_paint_t *t5ui_event_paint(t5ui_event_t *e)
{
    return e ? e->paint : NULL;
}

/* An md callback (lv_event_cb_t with a user pointer) reaching an app's
 * t5ui_event_cb_t: the record rides as user data, freed with owner. */
static void *md_cb_rec(lv_obj_t *owner, t5ui_event_cb_t cb, void *user)
{
    if (!cb) return NULL;
    rec_t *r = rec_new(cb, user, T5UI_EV_CLICK);
    if (r) free_with(owner, r);
    return r;
}

/* ---- the tree ------------------------------------------------------------------- */
t5ui_obj_t *t5ui_root(struct t5_app *app)
{
    return H(t5_app_screen((t5_app_t *)app));
}

t5ui_obj_t *t5ui_box(t5ui_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(O(parent));
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    return H(o);
}

t5ui_obj_t *t5ui_panel(t5ui_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(O(parent));
    lv_obj_set_style_bg_color(o, col(md_scheme()->c.surface), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, T5_DP(MD_SHAPE_MEDIUM_DP), 0);
    lv_obj_set_style_pad_all(o, T5_DP(16), 0);
    return H(o);
}

void t5ui_delete(t5ui_obj_t *obj)
{
    if (obj) lv_obj_delete(O(obj));
}

void t5ui_clean(t5ui_obj_t *obj)
{
    if (obj) lv_obj_clean(O(obj));
}

t5ui_obj_t *t5ui_parent(t5ui_obj_t *obj)
{
    return obj ? H(lv_obj_get_parent(O(obj))) : NULL;
}

t5ui_obj_t *t5ui_child(t5ui_obj_t *obj, int32_t index)
{
    return obj ? H(lv_obj_get_child(O(obj), index)) : NULL;
}

uint32_t t5ui_child_count(t5ui_obj_t *obj)
{
    return obj ? lv_obj_get_child_count(O(obj)) : 0;
}

void t5ui_set_user(t5ui_obj_t *obj, void *user)
{
    lv_obj_set_user_data(O(obj), user);
}

void *t5ui_user(t5ui_obj_t *obj)
{
    return lv_obj_get_user_data(O(obj));
}

/* ---- geometry and layout ------------------------------------------------------- */
void t5ui_size(t5ui_obj_t *obj, int32_t w, int32_t h)
{
    lv_obj_set_size(O(obj), sz(w), sz(h));
}

void t5ui_width(t5ui_obj_t *obj, int32_t w)
{
    lv_obj_set_width(O(obj), sz(w));
}

void t5ui_height(t5ui_obj_t *obj, int32_t h)
{
    lv_obj_set_height(O(obj), sz(h));
}

void t5ui_min_size(t5ui_obj_t *obj, int32_t min_w, int32_t min_h)
{
    lv_obj_set_style_min_width(O(obj), sz(min_w), 0);
    lv_obj_set_style_min_height(O(obj), sz(min_h), 0);
}

void t5ui_pos(t5ui_obj_t *obj, int32_t x, int32_t y)
{
    lv_obj_set_pos(O(obj), x, y);
}

void t5ui_align(t5ui_obj_t *obj, t5ui_align_t where, int32_t dx, int32_t dy)
{
    static const lv_align_t A[] = { LV_ALIGN_TOP_LEFT,    LV_ALIGN_TOP_MID,      LV_ALIGN_TOP_RIGHT,
                                    LV_ALIGN_LEFT_MID,    LV_ALIGN_CENTER,       LV_ALIGN_RIGHT_MID,
                                    LV_ALIGN_BOTTOM_LEFT, LV_ALIGN_BOTTOM_MID,   LV_ALIGN_BOTTOM_RIGHT };
    if ((unsigned)where >= sizeof A / sizeof A[0]) return;
    lv_obj_align(O(obj), A[where], dx, dy);
}

void t5ui_center(t5ui_obj_t *obj)
{
    lv_obj_center(O(obj));
}

static void rect_from(const lv_area_t *a, t5ui_rect_t *out)
{
    *out = (t5ui_rect_t){ a->x1, a->y1, a->x2, a->y2 };
}

void t5ui_coords(t5ui_obj_t *obj, t5ui_rect_t *out)
{
    lv_obj_update_layout(O(obj));
    lv_area_t a;
    lv_obj_get_coords(O(obj), &a);
    rect_from(&a, out);
}

void t5ui_content_coords(t5ui_obj_t *obj, t5ui_rect_t *out)
{
    lv_obj_update_layout(O(obj));
    lv_area_t a;
    lv_obj_get_content_coords(O(obj), &a);
    rect_from(&a, out);
}

void t5ui_flex(t5ui_obj_t *obj, t5ui_flow_t flow)
{
    static const lv_flex_flow_t F[] = { LV_FLEX_FLOW_ROW, LV_FLEX_FLOW_COLUMN, LV_FLEX_FLOW_ROW_WRAP,
                                        LV_FLEX_FLOW_COLUMN_WRAP };
    if ((unsigned)flow < 4) lv_obj_set_flex_flow(O(obj), F[flow]);
}

static lv_flex_align_t place(t5ui_place_t p)
{
    switch (p) {
    case T5UI_PLACE_END: return LV_FLEX_ALIGN_END;
    case T5UI_PLACE_CENTER: return LV_FLEX_ALIGN_CENTER;
    case T5UI_PLACE_SPACE_EVENLY: return LV_FLEX_ALIGN_SPACE_EVENLY;
    case T5UI_PLACE_SPACE_AROUND: return LV_FLEX_ALIGN_SPACE_AROUND;
    case T5UI_PLACE_SPACE_BETWEEN: return LV_FLEX_ALIGN_SPACE_BETWEEN;
    default: return LV_FLEX_ALIGN_START;
    }
}

void t5ui_flex_place(t5ui_obj_t *obj, t5ui_place_t main, t5ui_place_t cross, t5ui_place_t track)
{
    lv_obj_set_flex_align(O(obj), place(main), place(cross), place(track));
}

void t5ui_grow(t5ui_obj_t *obj, uint8_t grow)
{
    lv_obj_set_flex_grow(O(obj), grow);
}

void t5ui_pad(t5ui_obj_t *obj, int32_t top, int32_t right, int32_t bottom, int32_t left)
{
    lv_obj_set_style_pad_top(O(obj), top, 0);
    lv_obj_set_style_pad_right(O(obj), right, 0);
    lv_obj_set_style_pad_bottom(O(obj), bottom, 0);
    lv_obj_set_style_pad_left(O(obj), left, 0);
}

void t5ui_pad_all(t5ui_obj_t *obj, int32_t pad)
{
    lv_obj_set_style_pad_all(O(obj), pad, 0);
}

void t5ui_gap(t5ui_obj_t *obj, int32_t row_gap, int32_t column_gap)
{
    lv_obj_set_style_pad_row(O(obj), row_gap, 0);
    lv_obj_set_style_pad_column(O(obj), column_gap, 0);
}

void t5ui_margin(t5ui_obj_t *obj, int32_t top, int32_t right, int32_t bottom, int32_t left)
{
    lv_obj_set_style_margin_top(O(obj), top, 0);
    lv_obj_set_style_margin_right(O(obj), right, 0);
    lv_obj_set_style_margin_bottom(O(obj), bottom, 0);
    lv_obj_set_style_margin_left(O(obj), left, 0);
}

void t5ui_layout_now(t5ui_obj_t *obj)
{
    lv_obj_update_layout(O(obj));
}

/* ---- appearance ---------------------------------------------------------------- */
void t5ui_unstyle(t5ui_obj_t *obj)
{
    lv_obj_remove_style_all(O(obj));
}

void t5ui_bg(t5ui_obj_t *obj, t5ui_color_t color, t5ui_opa_t opa)
{
    lv_obj_set_style_bg_color(O(obj), col(color), 0);
    lv_obj_set_style_bg_opa(O(obj), opa, 0);
}

void t5ui_border(t5ui_obj_t *obj, int32_t width, t5ui_color_t color, t5ui_opa_t opa)
{
    lv_obj_set_style_border_width(O(obj), width, 0);
    lv_obj_set_style_border_color(O(obj), col(color), 0);
    lv_obj_set_style_border_opa(O(obj), opa, 0);
}

void t5ui_radius(t5ui_obj_t *obj, int32_t radius)
{
    lv_obj_set_style_radius(O(obj), radius >= T5UI_RADIUS_CIRCLE ? LV_RADIUS_CIRCLE : radius, 0);
}

void t5ui_shadow(t5ui_obj_t *obj, int32_t width, t5ui_color_t color, t5ui_opa_t opa)
{
    lv_obj_set_style_shadow_width(O(obj), width, 0);
    lv_obj_set_style_shadow_color(O(obj), col(color), 0);
    lv_obj_set_style_shadow_opa(O(obj), opa, 0);
}

void t5ui_elevation(t5ui_obj_t *obj, int32_t dp)
{
    md_set_elevation(O(obj), (int)dp, true);
}

void t5ui_opacity(t5ui_obj_t *obj, t5ui_opa_t opa)
{
    lv_obj_set_style_opa(O(obj), opa, 0);
}

void t5ui_text_color(t5ui_obj_t *obj, t5ui_color_t color, t5ui_opa_t opa)
{
    lv_obj_set_style_text_color(O(obj), col(color), 0);
    lv_obj_set_style_text_opa(O(obj), opa, 0);
}

void t5ui_text_style(t5ui_obj_t *obj, t5ui_text_t style)
{
    if (style == T5UI_TEXT_MONO) {
        lv_obj_set_style_text_font(O(obj), md_font_mono(), 0);
        return;
    }
    md_text_style(O(obj), (md_type_role_t)style, md_scheme()->on_surface.high);
}

void t5ui_text_align(t5ui_obj_t *obj, t5ui_text_align_t a)
{
    lv_obj_set_style_text_align(O(obj), a == T5UI_TEXT_CENTER ? LV_TEXT_ALIGN_CENTER
                                        : a == T5UI_TEXT_RIGHT ? LV_TEXT_ALIGN_RIGHT
                                                               : LV_TEXT_ALIGN_LEFT, 0);
}

void t5ui_stroke(t5ui_obj_t *obj, int32_t width, t5ui_color_t color, t5ui_opa_t opa)
{
    lv_obj_t *o = O(obj);
    if (lv_obj_check_type(o, &lv_arc_class)) {
        lv_obj_set_style_arc_width(o, width, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(o, col(color), LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(o, opa, LV_PART_INDICATOR);
        return;
    }
    lv_obj_set_style_line_width(o, width, LV_PART_ITEMS);
    lv_obj_set_style_line_color(o, col(color), LV_PART_ITEMS);
    lv_obj_set_style_line_opa(o, opa, LV_PART_ITEMS);
    lv_obj_set_style_line_width(o, width, 0);
    lv_obj_set_style_line_color(o, col(color), 0);
    lv_obj_set_style_line_opa(o, opa, 0);
}

/* ---- state ----------------------------------------------------------------------- */
void t5ui_hide(t5ui_obj_t *obj, bool hidden)
{
    lv_obj_set_hidden(O(obj), hidden);
}

bool t5ui_hidden(t5ui_obj_t *obj)
{
    return lv_obj_has_flag(O(obj), LV_OBJ_FLAG_HIDDEN);
}

void t5ui_clickable(t5ui_obj_t *obj, bool on)
{
    lv_obj_set_clickable(O(obj), on);
}

void t5ui_scrollable(t5ui_obj_t *obj, t5ui_scroll_t dirs)
{
    lv_obj_set_scrollable(O(obj), dirs != T5UI_SCROLL_NONE);
    if (dirs != T5UI_SCROLL_NONE) {
        lv_obj_set_scroll_dir(O(obj), dirs == T5UI_SCROLL_HOR ? LV_DIR_HOR : dirs == T5UI_SCROLL_VER ? LV_DIR_VER : LV_DIR_ALL);
    }
}

void t5ui_scroll_to(t5ui_obj_t *obj, int32_t x, int32_t y, bool animate)
{
    lv_obj_scroll_to(O(obj), x, y, animate ? LV_ANIM_ON : LV_ANIM_OFF);
}

void t5ui_scroll_into_view(t5ui_obj_t *obj, bool animate)
{
    lv_obj_scroll_to_view_recursive(O(obj), animate ? LV_ANIM_ON : LV_ANIM_OFF);
}

void t5ui_set_checked(t5ui_obj_t *obj, bool on)
{
    lv_obj_set_state(O(obj), LV_STATE_CHECKED, on);
}

bool t5ui_checked(t5ui_obj_t *obj)
{
    return lv_obj_has_state(O(obj), LV_STATE_CHECKED);
}

void t5ui_set_disabled(t5ui_obj_t *obj, bool on)
{
    lv_obj_set_state(O(obj), LV_STATE_DISABLED, on);
}

bool t5ui_disabled(t5ui_obj_t *obj)
{
    return lv_obj_has_state(O(obj), LV_STATE_DISABLED);
}

void t5ui_invalidate(t5ui_obj_t *obj)
{
    lv_obj_invalidate(O(obj));
}

/* ---- timers -------------------------------------------------------------------- */
struct t5ui_timer {
    lv_timer_t *t;
    t5ui_timer_cb_t cb;
    void *user;
};

static void timer_tramp(lv_timer_t *lt)
{
    t5ui_timer_t *t = lv_timer_get_user_data(lt);
    t->cb(t, t->user);
}

static void timer_release(void *h)
{
    t5ui_timer_delete(h);
}

t5ui_timer_t *t5ui_timer(uint32_t period_ms, t5ui_timer_cb_t cb, void *user)
{
    if (!cb) return NULL;
    t5ui_timer_t *t = malloc(sizeof *t);
    if (!t) return NULL;
    *t = (t5ui_timer_t){ .cb = cb, .user = user };
    t->t = lv_timer_create(timer_tramp, period_ms, t);
    if (!t->t) {
        free(t);
        return NULL;
    }
    t5_res_add(T5_RES_TIMER, t, timer_release);       /* ends with the app */
    return t;
}

void t5ui_timer_period(t5ui_timer_t *t, uint32_t period_ms)
{
    if (t) lv_timer_set_period(t->t, period_ms);
}

void t5ui_timer_delete(t5ui_timer_t *t)
{
    if (!t) return;
    t5_res_remove(t);
    lv_timer_delete(t->t);
    free(t);
}

/* ---- text ---------------------------------------------------------------------- */
t5ui_obj_t *t5ui_label(t5ui_obj_t *parent, const char *text, t5ui_text_t style)
{
    if (style == T5UI_TEXT_MONO) {
        lv_obj_t *l = md_label_create(O(parent), text ? text : "", MD_TYPE_BODY1);
        lv_obj_set_style_text_font(l, md_font_mono(), 0);
        return H(l);
    }
    return H(md_label_create(O(parent), text ? text : "", (md_type_role_t)style));
}

void t5ui_label_set(t5ui_obj_t *label, const char *text)
{
    lv_label_set_text(O(label), text ? text : "");
}

void t5ui_label_setf(t5ui_obj_t *label, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    lv_label_set_text(O(label), buf);
}

void t5ui_label_long(t5ui_obj_t *label, t5ui_long_t mode)
{
    lv_label_set_long_mode(O(label), mode == T5UI_LONG_DOTS     ? LV_LABEL_LONG_MODE_DOTS
                                     : mode == T5UI_LONG_SCROLL ? LV_LABEL_LONG_MODE_SCROLL_CIRCULAR
                                     : mode == T5UI_LONG_CLIP   ? LV_LABEL_LONG_MODE_CLIP
                                                                : LV_LABEL_LONG_MODE_WRAP);
}

void t5ui_text_size(const char *text, t5ui_text_t style, int32_t max_w, int32_t *w, int32_t *h)
{
    lv_point_t p = { 0, 0 };
    lv_text_get_size(&p, text ? text : "", font(style), 0, 0, max_w > 0 ? max_w : LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (w) *w = p.x;
    if (h) *h = p.y;
}

t5ui_obj_t *t5ui_icon(t5ui_obj_t *parent, const char *icon, int32_t size_dp)
{
    return H(md_icon_create(O(parent), icon ? icon : "", (int)size_dp));
}

/* ---- Material widgets --------------------------------------------------------- */
t5ui_obj_t *t5ui_button(t5ui_obj_t *parent, const char *text, t5ui_button_t kind)
{
    return H(md_button_create(O(parent), text ? text : "", (md_button_variant_t)kind));
}

t5ui_obj_t *t5ui_fab(t5ui_obj_t *parent, const char *icon, bool mini)
{
    return H(md_fab_create(O(parent), icon, mini));
}

t5ui_obj_t *t5ui_card(t5ui_obj_t *parent)
{
    return H(md_card_create(O(parent)));
}

t5ui_obj_t *t5ui_divider(t5ui_obj_t *parent)
{
    return H(md_divider_create(O(parent)));
}

static void md_tramp(lv_event_t *e)
{
    rec_t *r = lv_event_get_user_data(e);
    if (!r) return;
    t5ui_event_t ev = { .e = e, .user = r->user, .kind = T5UI_EV_CLICK };
    r->cb(&ev);
}

t5ui_obj_t *t5ui_appbar(t5ui_obj_t *parent, const char *title, const char *nav_icon, t5ui_event_cb_t nav, void *user)
{
    rec_t *r = nav ? rec_new(nav, user, T5UI_EV_CLICK) : NULL;
    lv_obj_t *bar = md_appbar_create(O(parent), title ? title : "", nav_icon, r ? md_tramp : NULL, r);
    if (r) free_with(bar, r);
    return H(bar);
}

t5ui_obj_t *t5ui_appbar_action(t5ui_obj_t *bar, const char *icon, t5ui_event_cb_t cb, void *user)
{
    void *r = md_cb_rec(O(bar), cb, user);
    return H(md_appbar_add_action(O(bar), icon, r ? md_tramp : NULL, r));
}

void t5ui_appbar_title(t5ui_obj_t *bar, const char *title)
{
    md_appbar_set_title(O(bar), title ? title : "");
}

/* A list's row being bound (t5ui_row_text then sets its labels). */
static md_list_row_t *s_binding;

t5ui_obj_t *t5ui_row(t5ui_obj_t *parent, const char *icon, const char *primary, const char *secondary)
{
    return H(md_row_create(O(parent), icon, primary ? primary : "", secondary));
}

void t5ui_row_text(t5ui_obj_t *row, const char *primary, const char *secondary)
{
    if (s_binding && O(row) == s_binding->row) {
        if (primary && s_binding->primary) lv_label_set_text(s_binding->primary, primary);
        if (secondary && s_binding->secondary) lv_label_set_text(s_binding->secondary, secondary);
        return;
    }
    if (primary) md_row_set_text(O(row), primary);
    if (secondary) md_row_set_secondary(O(row), secondary);
}

void t5ui_row_on_click(t5ui_obj_t *row, t5ui_event_cb_t cb, void *user)
{
    void *r = md_cb_rec(O(row), cb, user);
    if (r) md_row_set_clickable(O(row), md_tramp, r);
}

t5ui_obj_t *t5ui_row_switch(t5ui_obj_t *row, bool on)
{
    return H(md_row_switch(O(row), on));
}

t5ui_obj_t *t5ui_row_trailing(t5ui_obj_t *row, const char *text)
{
    return H(md_row_trailing_text(O(row), text ? text : ""));
}

t5ui_obj_t *t5ui_section(t5ui_obj_t *parent, const char *title)
{
    return H(md_section_create(O(parent), title ? title : ""));
}

t5ui_obj_t *t5ui_switch(t5ui_obj_t *parent, bool on)
{
    return H(md_switch_create(O(parent), on));
}

t5ui_obj_t *t5ui_checkbox(t5ui_obj_t *parent, const char *text, bool checked)
{
    return H(md_checkbox_create(O(parent), text ? text : "", checked));
}

t5ui_obj_t *t5ui_slider(t5ui_obj_t *parent, int32_t min, int32_t max, int32_t value)
{
    return H(md_slider_create(O(parent), min, max, value));
}

t5ui_obj_t *t5ui_arc(t5ui_obj_t *parent, int32_t min, int32_t max, int32_t value)
{
    lv_obj_t *a = lv_arc_create(O(parent));
    lv_arc_set_range(a, min, max);
    lv_arc_set_value(a, value);
    lv_obj_set_style_arc_color(a, col(md_scheme()->c.primary), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(a, col(md_scheme()->c.primary), LV_PART_KNOB);
    return H(a);
}

t5ui_obj_t *t5ui_bar(t5ui_obj_t *parent, int32_t min, int32_t max, int32_t value)
{
    lv_obj_t *b = lv_bar_create(O(parent));
    lv_bar_set_range(b, min, max);
    lv_bar_set_value(b, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(b, col(md_scheme()->c.primary), LV_PART_INDICATOR);
    return H(b);
}

void t5ui_set_value(t5ui_obj_t *obj, int32_t value, bool animate)
{
    lv_obj_t *o = O(obj);
    if (lv_obj_check_type(o, &lv_slider_class)) lv_slider_set_value(o, value, animate ? LV_ANIM_ON : LV_ANIM_OFF);
    else if (lv_obj_check_type(o, &lv_arc_class)) lv_arc_set_value(o, value);
    else if (lv_obj_check_type(o, &lv_bar_class)) lv_bar_set_value(o, value, animate ? LV_ANIM_ON : LV_ANIM_OFF);
}

int32_t t5ui_value(t5ui_obj_t *obj)
{
    lv_obj_t *o = O(obj);
    if (lv_obj_check_type(o, &lv_slider_class)) return lv_slider_get_value(o);
    if (lv_obj_check_type(o, &lv_arc_class)) return lv_arc_get_value(o);
    if (lv_obj_check_type(o, &lv_bar_class)) return lv_bar_get_value(o);
    return 0;
}

t5ui_obj_t *t5ui_textfield(t5ui_obj_t *parent, const char *label, bool password)
{
    return H(md_textfield_create(O(parent), label ? label : "", password));
}

void t5ui_textfield_helper(t5ui_obj_t *field, const char *text, bool error)
{
    md_textfield_set_helper(O(field), text, error);
}

t5ui_obj_t *t5ui_textarea(t5ui_obj_t *parent, bool one_line, const char *placeholder)
{
    lv_obj_t *t = lv_textarea_create(O(parent));
    lv_textarea_set_one_line(t, one_line);
    if (placeholder) lv_textarea_set_placeholder_text(t, placeholder);
    return H(t);
}

const char *t5ui_text(t5ui_obj_t *obj)
{
    lv_obj_t *o = O(obj);
    if (lv_obj_check_type(o, &lv_textarea_class)) return lv_textarea_get_text(o);
    if (lv_obj_check_type(o, &lv_label_class)) return lv_label_get_text(o);
    return md_textfield_text(o);
}

void t5ui_set_text(t5ui_obj_t *obj, const char *text)
{
    lv_obj_t *o = O(obj);
    if (lv_obj_check_type(o, &lv_textarea_class)) lv_textarea_set_text(o, text ? text : "");
    else if (lv_obj_check_type(o, &lv_label_class)) lv_label_set_text(o, text ? text : "");
    else md_textfield_set_text(o, text ? text : "");
}

t5ui_obj_t *t5ui_chip(t5ui_obj_t *parent, const char *text, const char *icon, t5ui_chip_t kind)
{
    return H(md_chip_create(O(parent), text ? text : "", icon, (md_chip_kind_t)kind));
}

t5ui_obj_t *t5ui_tabs(t5ui_obj_t *parent)
{
    return H(md_tabs_create(O(parent)));
}

t5ui_obj_t *t5ui_tabs_add(t5ui_obj_t *tabs, const char *name)
{
    return H(md_tabs_add(O(tabs), name ? name : ""));
}

t5ui_obj_t *t5ui_page(t5ui_obj_t *parent, const char *title, t5ui_event_cb_t back, void *user)
{
    rec_t *r = back ? rec_new(back, user, T5UI_EV_CLICK) : NULL;
    lv_obj_t *bar = NULL;
    lv_obj_t *page = md_page_create(O(parent), title ? title : "", r ? md_tramp : NULL, r, &bar);
    if (r) free_with(page, r);
    lv_obj_set_user_data(page, bar);           /* t5ui_page_bar */
    return H(page);
}

t5ui_obj_t *t5ui_page_body(t5ui_obj_t *page)
{
    return H(md_page_body(O(page)));
}

t5ui_obj_t *t5ui_page_bar(t5ui_obj_t *page)
{
    return H(lv_obj_get_user_data(O(page)));
}

typedef struct {
    t5ui_list_bind_cb_t bind;
    t5ui_list_click_cb_t click;
    void *user;
} list_ctx_t;

static void list_bind(md_list_row_t *row, uint32_t index, void *user)
{
    list_ctx_t *c = user;
    s_binding = row;
    c->bind(H(row->row), index, c->user);
    s_binding = NULL;
}

static void list_click(uint32_t index, void *user)
{
    list_ctx_t *c = user;
    if (c->click) c->click(index, c->user);
}

t5ui_obj_t *t5ui_list(t5ui_obj_t *parent, uint32_t count, int32_t row_dp, t5ui_list_bind_cb_t bind,
                      t5ui_list_click_cb_t click, void *user)
{
    if (!bind) return NULL;
    list_ctx_t *c = malloc(sizeof *c);
    if (!c) return NULL;
    *c = (list_ctx_t){ bind, click, user };
    lv_obj_t *l = md_list_create(O(parent), count, (int)row_dp, list_bind, list_click, c);
    if (!l) {
        free(c);
        return NULL;
    }
    free_with(l, c);
    return H(l);
}

void t5ui_list_count(t5ui_obj_t *list, uint32_t count)
{
    md_list_set_count(O(list), count);
}

void t5ui_list_refresh(t5ui_obj_t *list)
{
    md_list_refresh(O(list));
}

_Static_assert(sizeof(t5ui_answer_cb_t) == sizeof(md_dialog_cb_t), "answer callbacks match md's");

t5ui_obj_t *t5ui_dialog(const char *title, const char *text, const char *confirm, const char *dismiss,
                        t5ui_answer_cb_t cb, void *user)
{
    return H(md_dialog_show(title, text, confirm, dismiss, (md_dialog_cb_t)cb, user));
}

void t5ui_dialog_close(t5ui_obj_t *dialog)
{
    md_dialog_close(O(dialog));
}

t5ui_obj_t *t5ui_menu(t5ui_obj_t *anchor, const char *const *items, int n, t5ui_answer_cb_t cb, void *user)
{
    return H(md_menu_show(O(anchor), items, n, (md_menu_cb_t)cb, user));
}

void t5ui_snackbar(const char *text, const char *action, t5ui_action_cb_t cb, void *user)
{
    md_snackbar_show(text, action, (md_action_cb_t)cb, user);
}

/* ---- images --------------------------------------------------------------------- */
static lv_color_format_t px_format(uint8_t f, uint8_t *bpp)
{
    switch (f) {
    case T5UI_PX_RGB565: *bpp = 2; return LV_COLOR_FORMAT_RGB565;
    case T5UI_PX_RGB888: *bpp = 3; return LV_COLOR_FORMAT_RGB888;
    case T5UI_PX_ARGB8888: *bpp = 4; return LV_COLOR_FORMAT_ARGB8888;
    case T5UI_PX_A8: *bpp = 1; return LV_COLOR_FORMAT_A8;
    default: *bpp = 0; return LV_COLOR_FORMAT_UNKNOWN;
    }
}

/* A copy of an app's pixels as an LVGL draw buffer, freed with its object. */
static void buf_free(lv_event_t *e)
{
    lv_draw_buf_destroy(lv_event_get_user_data(e));
}

static lv_draw_buf_t *buf_from(const t5ui_pixels_t *px)
{
    uint8_t bpp;
    lv_color_format_t cf = px_format(px->format, &bpp);
    if (!bpp || !px->w || !px->h || !px->data) return NULL;
    uint32_t stride = px->stride ? px->stride : (uint32_t)px->w * bpp;
    lv_draw_buf_t *b = lv_draw_buf_create(px->w, px->h, cf, LV_STRIDE_AUTO);
    if (!b) return NULL;
    for (uint32_t y = 0; y < px->h; y++) {
        memcpy(b->data + y * b->header.stride, (const uint8_t *)px->data + y * stride, (size_t)px->w * bpp);
    }
    return b;
}

static void buf_destroy_later(void *b)
{
    lv_draw_buf_destroy(b);
}

t5ui_obj_t *t5ui_image(t5ui_obj_t *parent)
{
    return H(lv_image_create(O(parent)));
}

/* Drop the buffer an image showed before (its free callback on the object). */
static void drop_buf(lv_obj_t *o)
{
    for (uint32_t i = 0; i < lv_obj_get_event_count(o); i++) {
        lv_event_dsc_t *d = lv_obj_get_event_dsc(o, i);
        if (lv_event_dsc_get_cb(d) != buf_free) continue;
        lv_draw_buf_t *b = lv_event_dsc_get_user_data(d);
        lv_obj_remove_event_dsc(o, d);
        lv_image_cache_drop(b);
        lv_draw_buf_destroy(b);
        return;
    }
}

t5_err_t t5ui_image_pixels(t5ui_obj_t *img, const t5ui_pixels_t *px)
{
    if (!img || !px || px->size < sizeof *px) return T5_ERR_INVALID_ARG;
    lv_draw_buf_t *b = buf_from(px);
    if (!b) return T5_ERR_NO_MEM;
    lv_obj_t *o = O(img);
    lv_image_set_src(o, b);
    drop_buf(o);                               /* the previous one isn't shown any more */
    lv_obj_add_event_cb(o, buf_free, LV_EVENT_DELETE, b);
    return T5_OK;
}

/* ---- drawing -------------------------------------------------------------------- */
t5ui_obj_t *t5ui_canvas(t5ui_obj_t *parent, int32_t w, int32_t h)
{
    if (w <= 0 || h <= 0) return NULL;
    lv_draw_buf_t *b = lv_draw_buf_create((uint32_t)w, (uint32_t)h, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
    if (!b) return NULL;
    lv_obj_t *c = lv_canvas_create(O(parent));
    lv_canvas_set_draw_buf(c, b);
    lv_canvas_fill_bg(c, lv_color_black(), LV_OPA_TRANSP);
    lv_obj_add_event_cb(c, buf_free, LV_EVENT_DELETE, b);
    return H(c);
}

void t5ui_canvas_fill(t5ui_obj_t *canvas, t5ui_color_t color, t5ui_opa_t opa)
{
    lv_canvas_fill_bg(O(canvas), col(color), opa);
}

t5ui_paint_t *t5ui_canvas_begin(t5ui_obj_t *canvas)
{
    t5ui_paint_t *p = malloc(sizeof *p + sizeof(lv_layer_t));
    if (!p) return NULL;
    p->layer = (lv_layer_t *)(p + 1);
    p->ox = p->oy = 0;
    lv_canvas_init_layer(O(canvas), p->layer);
    return p;
}

void t5ui_canvas_end(t5ui_obj_t *canvas, t5ui_paint_t *p)
{
    if (!p) return;
    lv_canvas_finish_layer(O(canvas), p->layer);
    free(p);
}

void t5ui_draw_line(t5ui_paint_t *p, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t width,
                    t5ui_color_t color, t5ui_opa_t opa)
{
    if (!p) return;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = x1 + p->ox;
    d.p1.y = y1 + p->oy;
    d.p2.x = x2 + p->ox;
    d.p2.y = y2 + p->oy;
    d.width = width;
    d.color = col(color);
    d.opa = opa;
    d.round_start = d.round_end = 1;
    lv_draw_line(p->layer, &d);
}

static lv_area_t area_of(t5ui_paint_t *p, const t5ui_rect_t *r)
{
    return (lv_area_t){ r->x1 + p->ox, r->y1 + p->oy, r->x2 + p->ox, r->y2 + p->oy };
}

void t5ui_draw_rect(t5ui_paint_t *p, const t5ui_rect_t *r, int32_t radius, t5ui_color_t fill, t5ui_opa_t fill_opa,
                    int32_t border_w, t5ui_color_t border, t5ui_opa_t border_opa)
{
    if (!p || !r) return;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = radius >= T5UI_RADIUS_CIRCLE ? LV_RADIUS_CIRCLE : radius;
    d.bg_color = col(fill);
    d.bg_opa = fill_opa;
    d.border_width = border_w;
    d.border_color = col(border);
    d.border_opa = border_opa;
    lv_area_t a = area_of(p, r);
    lv_draw_rect(p->layer, &d, &a);
}

void t5ui_draw_arc(t5ui_paint_t *p, int32_t cx, int32_t cy, int32_t radius, int32_t start_deg, int32_t end_deg,
                   int32_t width, t5ui_color_t color, t5ui_opa_t opa, bool rounded)
{
    if (!p || radius <= 0) return;
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center.x = cx + p->ox;
    d.center.y = cy + p->oy;
    d.radius = (uint16_t)radius;
    d.start_angle = start_deg;
    d.end_angle = end_deg;
    d.width = width;
    d.color = col(color);
    d.opa = opa;
    d.rounded = rounded;
    lv_draw_arc(p->layer, &d);
}

void t5ui_draw_text(t5ui_paint_t *p, const t5ui_rect_t *r, const char *text, t5ui_text_t style, t5ui_color_t color,
                    t5ui_text_align_t align)
{
    if (!p || !r || !text) return;
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = text;
    d.font = font(style);
    d.color = col(color);
    d.align = align == T5UI_TEXT_CENTER ? LV_TEXT_ALIGN_CENTER : align == T5UI_TEXT_RIGHT ? LV_TEXT_ALIGN_RIGHT
                                                                                          : LV_TEXT_ALIGN_LEFT;
    d.text_local = 1;                          /* copied: the app's string may go */
    lv_area_t a = area_of(p, r);
    lv_draw_label(p->layer, &d, &a);
}

void t5ui_draw_pixels(t5ui_paint_t *p, int32_t x, int32_t y, const t5ui_pixels_t *px)
{
    if (!p || !px || px->size < sizeof *px) return;
    uint8_t bpp;
    lv_color_format_t cf = px_format(px->format, &bpp);
    if (!bpp) return;
    lv_draw_buf_t *b = buf_from(px);           /* the app's buffer may go before the layer renders */
    if (!b) return;
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = b;
    lv_area_t a = { x + p->ox, y + p->oy, x + p->ox + px->w - 1, y + p->oy + px->h - 1 };
    lv_draw_image(p->layer, &d, &a);
    (void)cf;
    /* The draw task reads the copy while the layer renders: within this
     * frame for a draw event, in t5ui_canvas_end for a canvas. Async calls
     * run after both. */
    lv_async_call(buf_destroy_later, b);
}

/* ---- charts --------------------------------------------------------------------- */
t5ui_obj_t *t5ui_chart(t5ui_obj_t *parent, t5ui_chart_t kind, uint32_t points)
{
    lv_obj_t *c = lv_chart_create(O(parent));
    lv_chart_set_type(c, kind == T5UI_CHART_BAR ? LV_CHART_TYPE_BAR : LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(c, points ? points : 10);
    lv_chart_set_update_mode(c, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_size(c, 0, 0, LV_PART_INDICATOR);     /* no dots on lines */
    return H(c);
}

static lv_chart_series_t *series_at(lv_obj_t *c, int index)
{
    lv_chart_series_t *s = NULL;
    for (int i = 0; i <= index; i++) {
        s = lv_chart_get_series_next(c, s);
        if (!s) return NULL;
    }
    return s;
}

int t5ui_chart_series(t5ui_obj_t *chart, t5ui_color_t color)
{
    lv_obj_t *c = O(chart);
    int n = 0;
    for (lv_chart_series_t *s = lv_chart_get_series_next(c, NULL); s; s = lv_chart_get_series_next(c, s)) n++;
    if (n >= 8) return T5_ERR_NO_MEM;
    return lv_chart_add_series(c, col(color), LV_CHART_AXIS_PRIMARY_Y) ? n : T5_ERR_NO_MEM;
}

void t5ui_chart_range(t5ui_obj_t *chart, int32_t min, int32_t max)
{
    lv_chart_set_axis_range(O(chart), LV_CHART_AXIS_PRIMARY_Y, min, max);
}

void t5ui_chart_push(t5ui_obj_t *chart, int series, int32_t value)
{
    lv_chart_series_t *s = series_at(O(chart), series);
    if (s) lv_chart_set_next_value(O(chart), s, value);
}

void t5ui_chart_set(t5ui_obj_t *chart, int series, uint32_t index, int32_t value)
{
    lv_chart_series_t *s = series_at(O(chart), series);
    if (!s) return;
    lv_chart_set_series_value_by_id(O(chart), s, index, value);
    lv_chart_refresh(O(chart));
}

void t5ui_chart_points(t5ui_obj_t *chart, uint32_t points)
{
    lv_chart_set_point_count(O(chart), points);
}

void t5ui_chart_grid(t5ui_obj_t *chart, uint8_t rows, uint8_t columns)
{
    lv_chart_set_div_line_count(O(chart), rows, columns);
}
