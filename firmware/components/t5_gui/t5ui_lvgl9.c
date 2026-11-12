/* SPDX-License-Identifier: Apache-2.0
 * The t5ui backend on LVGL 9 and md (see t5ui_backend.h). This file is
 * where t5ui meets LVGL. Moving the OS to another UI library means writing
 * another backend, not changing apps.
 *
 * Handles are the LVGL objects themselves, cast; apps never look inside.
 * Each object t5ui makes carries a node (its kind, the app's user pointer,
 * buffers and animations), found through its LV_EVENT_DELETE callback that
 * frees it. md widgets keep their own data in LVGL's user_data, so t5ui
 * can't use that. Event records (cb, user) ride as LVGL event user data and
 * are freed with the object they're on. Timers and styles are the app's
 * resources (t5_res), so they end with it.
 */
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "misc/cache/instance/lv_image_cache.h"   /* lv_image_cache_drop: not in lvgl.h */
#include "md/md.h"
#include "md/md_icons.h"
#include "t5_app/app.h"
#include "t5_core/os.h"
#include "t5_core/res.h"
#include "t5_ui/t5_ui.h"   /* t5_ui_async, t5_ui_touch_get */
#include "t5_gui/t5ui_sys.h"
#include "t5ui_backend.h"

#define O(o) ((lv_obj_t *)(o))
#define H(o) ((t5ui_obj_t *)(o))

/* ---- nodes ------------------------------------------------------------------------- */
typedef struct anim_rec {
    struct anim_rec *next;
    lv_obj_t *obj;
    t5ui_prop_t prop;
} anim_rec_t;

typedef struct {
    lv_obj_t *obj;
    void *user;
    lv_obj_t *bar;             /* MD_PAGE: its app bar */
    lv_draw_buf_t *buf;        /* IMAGE, CANVAS: the pixels shown */
    anim_rec_t *anims;
    int32_t ymin, ymax;        /* CHART: Y range */
    uint8_t grid_rows, grid_cols;
    uint16_t kind;
    int16_t elevation;
    int16_t ext_click;
    bool helper_error;
} node_t;

static void node_free(lv_event_t *e)
{
    node_t *n = lv_event_get_user_data(e);
    for (anim_rec_t *r = n->anims, *next; r; r = next) {
        next = r->next;
        lv_anim_delete(r, NULL);
        free(r);
    }
    if (n->buf) {
        lv_image_cache_drop(n->buf);
        lv_draw_buf_destroy(n->buf);
    }
    free(n);
}

static node_t *node_of(lv_obj_t *o)
{
    if (!o) return NULL;
    uint32_t count = lv_obj_get_event_count(o);
    for (uint32_t i = 0; i < count; i++) {
        lv_event_dsc_t *d = lv_obj_get_event_dsc(o, i);
        if (lv_event_dsc_get_cb(d) == node_free) return lv_event_dsc_get_user_data(d);
    }
    return NULL;
}

static node_t *node_add(lv_obj_t *o, t5ui_kind_t kind)
{
    node_t *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->obj = o;
    n->kind = (uint16_t)kind;
    n->ymax = 100;
    n->grid_rows = 3;
    n->grid_cols = 5;
    lv_obj_add_event_cb(o, node_free, LV_EVENT_DELETE, n);
    return n;
}

/* An object t5ui didn't make (the root, a part of an md widget) gets a
 * node when the app first needs one. */
static node_t *node_need(lv_obj_t *o)
{
    node_t *n = node_of(o);
    return n ? n : node_add(o, 0);
}

static t5ui_kind_t kind_of(lv_obj_t *o)
{
    node_t *n = node_of(o);
    return n ? (t5ui_kind_t)n->kind : 0;
}

static bool is_md_button(t5ui_kind_t k)
{
    return k == T5UI_MD_BUTTON_TEXT || k == T5UI_MD_BUTTON_OUTLINED || k == T5UI_MD_BUTTON_CONTAINED;
}

static bool is_chip(t5ui_kind_t k)
{
    return k >= T5UI_MD_CHIP_ACTION && k <= T5UI_MD_CHIP_CHOICE;
}

static bool is_field(t5ui_kind_t k)
{
    return k == T5UI_MD_TEXTFIELD || k == T5UI_MD_PASSWORD;
}

static bool is_appbar(t5ui_kind_t k)
{
    return k == T5UI_MD_APPBAR || k == T5UI_MD_APPBAR_BACK;
}

#define IS(o, cls) lv_obj_has_class((o), &(cls))

/* ---- conversions ---------------------------------------------------------------------- */
static int32_t sz(int32_t v)
{
    if (v == T5UI_CONTENT) return LV_SIZE_CONTENT;
    if ((v & 0x7fff0000) == 0x40000000) return lv_pct((int16_t)(v & 0xffff));
    return v;
}

static int32_t unsz(int32_t v)
{
    if (v == LV_SIZE_CONTENT) return T5UI_CONTENT;
    if (LV_COORD_IS_PCT(v)) return T5UI_PCT(LV_COORD_GET_PCT(v));
    return v;
}

static lv_color_t col(t5ui_color_t c)
{
    return lv_color_hex(c & 0xffffff);
}

static t5ui_color_t uncol(lv_color_t c)
{
    return lv_color_to_u32(c) & 0xffffff;
}

static lv_anim_enable_t anim_on(int32_t v)
{
    return v ? LV_ANIM_ON : LV_ANIM_OFF;
}

_Static_assert((int)T5UI_TEXT_HEADLINE4 == (int)MD_TYPE_HEADLINE4 && (int)T5UI_TEXT_OVERLINE == (int)MD_TYPE_OVERLINE,
               "t5ui text styles follow md's type roles");

static const lv_font_t *font(int32_t t)
{
    if (t == T5UI_TEXT_MONO) return md_font_mono();
    return md_font((md_type_role_t)((unsigned)t < MD_TYPE_COUNT ? t : T5UI_TEXT_BODY1));
}

static int32_t unfont(const lv_font_t *f)
{
    if (f == md_font_mono()) return T5UI_TEXT_MONO;
    for (int t = 0; t < MD_TYPE_COUNT; t++) {
        if (md_font((md_type_role_t)t) == f) return t;
    }
    return T5UI_TEXT_BODY1;
}

static const lv_align_t ALIGNS[] = {
    [T5UI_ALIGN_DEFAULT] = LV_ALIGN_DEFAULT,         [T5UI_ALIGN_TOP_LEFT] = LV_ALIGN_TOP_LEFT,
    [T5UI_ALIGN_TOP_MID] = LV_ALIGN_TOP_MID,         [T5UI_ALIGN_TOP_RIGHT] = LV_ALIGN_TOP_RIGHT,
    [T5UI_ALIGN_LEFT_MID] = LV_ALIGN_LEFT_MID,       [T5UI_ALIGN_CENTER] = LV_ALIGN_CENTER,
    [T5UI_ALIGN_RIGHT_MID] = LV_ALIGN_RIGHT_MID,     [T5UI_ALIGN_BOTTOM_LEFT] = LV_ALIGN_BOTTOM_LEFT,
    [T5UI_ALIGN_BOTTOM_MID] = LV_ALIGN_BOTTOM_MID,   [T5UI_ALIGN_BOTTOM_RIGHT] = LV_ALIGN_BOTTOM_RIGHT,
};
static const lv_flex_flow_t FLOWS[] = {
    [T5UI_ROW] = LV_FLEX_FLOW_ROW, [T5UI_COLUMN] = LV_FLEX_FLOW_COLUMN,
    [T5UI_ROW_WRAP] = LV_FLEX_FLOW_ROW_WRAP, [T5UI_COLUMN_WRAP] = LV_FLEX_FLOW_COLUMN_WRAP,
    [T5UI_ROW_REVERSE] = LV_FLEX_FLOW_ROW_REVERSE, [T5UI_COLUMN_REVERSE] = LV_FLEX_FLOW_COLUMN_REVERSE,
};
static const lv_scroll_snap_t SNAPS[] = { LV_SCROLL_SNAP_NONE, LV_SCROLL_SNAP_START, LV_SCROLL_SNAP_END,
                                          LV_SCROLL_SNAP_CENTER };
static const lv_flex_align_t PLACES[] = {
    [T5UI_PLACE_START] = LV_FLEX_ALIGN_START,               [T5UI_PLACE_END] = LV_FLEX_ALIGN_END,
    [T5UI_PLACE_CENTER] = LV_FLEX_ALIGN_CENTER,             [T5UI_PLACE_SPACE_EVENLY] = LV_FLEX_ALIGN_SPACE_EVENLY,
    [T5UI_PLACE_SPACE_AROUND] = LV_FLEX_ALIGN_SPACE_AROUND, [T5UI_PLACE_SPACE_BETWEEN] = LV_FLEX_ALIGN_SPACE_BETWEEN,
};
static const lv_grad_dir_t GRADS[] = { LV_GRAD_DIR_NONE, LV_GRAD_DIR_VER, LV_GRAD_DIR_HOR };
static const lv_text_align_t TALIGNS[] = { LV_TEXT_ALIGN_LEFT, LV_TEXT_ALIGN_CENTER, LV_TEXT_ALIGN_RIGHT };
static const lv_dir_t DIRS[] = { LV_DIR_NONE, LV_DIR_HOR, LV_DIR_VER, LV_DIR_ALL };
static const lv_scrollbar_mode_t SCROLLBARS[] = { LV_SCROLLBAR_MODE_OFF, LV_SCROLLBAR_MODE_ON,
                                                  LV_SCROLLBAR_MODE_ACTIVE, LV_SCROLLBAR_MODE_AUTO };
static const lv_label_long_mode_t LONGS[] = { LV_LABEL_LONG_MODE_WRAP, LV_LABEL_LONG_MODE_DOTS,
                                              LV_LABEL_LONG_MODE_SCROLL_CIRCULAR, LV_LABEL_LONG_MODE_CLIP };
#define N(a) ((int32_t)(sizeof(a) / sizeof((a)[0])))
#define IN(v, a) ((uint32_t)(v) < (uint32_t)N(a))

/* The index of v in table a, or def. */
#define BACK(a, v, def)                                         \
    ({                                                          \
        int32_t r_ = (def);                                     \
        for (int32_t i_ = 0; i_ < N(a); i_++) {                 \
            if ((a)[i_] == (v)) { r_ = i_; break; }             \
        }                                                       \
        r_;                                                     \
    })

static uint32_t sides(int32_t v)
{
    return ((v & T5UI_SIDE_TOP) ? LV_BORDER_SIDE_TOP : 0) | ((v & T5UI_SIDE_BOTTOM) ? LV_BORDER_SIDE_BOTTOM : 0) |
           ((v & T5UI_SIDE_LEFT) ? LV_BORDER_SIDE_LEFT : 0) | ((v & T5UI_SIDE_RIGHT) ? LV_BORDER_SIDE_RIGHT : 0);
}

static int32_t unsides(uint32_t v)
{
    return ((v & LV_BORDER_SIDE_TOP) ? T5UI_SIDE_TOP : 0) | ((v & LV_BORDER_SIDE_BOTTOM) ? T5UI_SIDE_BOTTOM : 0) |
           ((v & LV_BORDER_SIDE_LEFT) ? T5UI_SIDE_LEFT : 0) | ((v & LV_BORDER_SIDE_RIGHT) ? T5UI_SIDE_RIGHT : 0);
}

static int32_t clamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static lv_part_t part_of(uint32_t at)
{
    static const lv_part_t P[] = { LV_PART_MAIN, LV_PART_SCROLLBAR, LV_PART_INDICATOR, LV_PART_KNOB,
                                   LV_PART_SELECTED, LV_PART_ITEMS, LV_PART_CURSOR };
    uint32_t p = T5UI_AT_PART(at);
    return p < sizeof P / sizeof P[0] ? P[p] : LV_PART_MAIN;
}

static lv_state_t states_of(uint32_t at)
{
    uint32_t s = T5UI_AT_STATES(at);
    return ((s & T5UI_STATE_CHECKED) ? LV_STATE_CHECKED : 0) | ((s & T5UI_STATE_FOCUSED) ? LV_STATE_FOCUSED : 0) |
           ((s & T5UI_STATE_PRESSED) ? LV_STATE_PRESSED : 0) | ((s & T5UI_STATE_DISABLED) ? LV_STATE_DISABLED : 0) |
           ((s & T5UI_STATE_EDITED) ? LV_STATE_EDITED : 0);
}

static lv_style_selector_t sel_of(uint32_t at)
{
    return (lv_style_selector_t)part_of(at) | (lv_style_selector_t)states_of(at);
}

/* ---- look properties: LVGL style properties ------------------------------------------- */
enum { CV_PX, CV_SIZE, CV_COLOR, CV_OPA, CV_RADIUS, CV_ALIGN, CV_FLOW, CV_PLACE, CV_GRAD, CV_SIDES, CV_TALIGN,
       CV_DECOR, CV_FONT, CV_BOOL, CV_SCALE, CV_PAD_ALL };

/* The style property behind look property p, and how its value converts. */
static bool look(t5ui_prop_t p, lv_style_prop_t *sp, uint8_t *cv)
{
    switch (p) {
#define M(tp, lp, c) case tp: *sp = lp; *cv = c; return true
    M(T5UI_P_X, LV_STYLE_X, CV_SIZE);
    M(T5UI_P_Y, LV_STYLE_Y, CV_SIZE);
    M(T5UI_P_WIDTH, LV_STYLE_WIDTH, CV_SIZE);
    M(T5UI_P_HEIGHT, LV_STYLE_HEIGHT, CV_SIZE);
    M(T5UI_P_MIN_WIDTH, LV_STYLE_MIN_WIDTH, CV_SIZE);
    M(T5UI_P_MAX_WIDTH, LV_STYLE_MAX_WIDTH, CV_SIZE);
    M(T5UI_P_MIN_HEIGHT, LV_STYLE_MIN_HEIGHT, CV_SIZE);
    M(T5UI_P_MAX_HEIGHT, LV_STYLE_MAX_HEIGHT, CV_SIZE);
    M(T5UI_P_ALIGN, LV_STYLE_ALIGN, CV_ALIGN);
    M(T5UI_P_FLOW, LV_STYLE_FLEX_FLOW, CV_FLOW);
    M(T5UI_P_PLACE_MAIN, LV_STYLE_FLEX_MAIN_PLACE, CV_PLACE);
    M(T5UI_P_PLACE_CROSS, LV_STYLE_FLEX_CROSS_PLACE, CV_PLACE);
    M(T5UI_P_PLACE_TRACK, LV_STYLE_FLEX_TRACK_PLACE, CV_PLACE);
    M(T5UI_P_GROW, LV_STYLE_FLEX_GROW, CV_PX);
    M(T5UI_P_PAD_TOP, LV_STYLE_PAD_TOP, CV_PX);
    M(T5UI_P_PAD_BOTTOM, LV_STYLE_PAD_BOTTOM, CV_PX);
    M(T5UI_P_PAD_LEFT, LV_STYLE_PAD_LEFT, CV_PX);
    M(T5UI_P_PAD_RIGHT, LV_STYLE_PAD_RIGHT, CV_PX);
    M(T5UI_P_PAD_ALL, LV_STYLE_PAD_TOP, CV_PAD_ALL);
    M(T5UI_P_PAD_ROW, LV_STYLE_PAD_ROW, CV_PX);
    M(T5UI_P_PAD_COLUMN, LV_STYLE_PAD_COLUMN, CV_PX);
    M(T5UI_P_MARGIN_TOP, LV_STYLE_MARGIN_TOP, CV_PX);
    M(T5UI_P_MARGIN_BOTTOM, LV_STYLE_MARGIN_BOTTOM, CV_PX);
    M(T5UI_P_MARGIN_LEFT, LV_STYLE_MARGIN_LEFT, CV_PX);
    M(T5UI_P_MARGIN_RIGHT, LV_STYLE_MARGIN_RIGHT, CV_PX);
    M(T5UI_P_BG_COLOR, LV_STYLE_BG_COLOR, CV_COLOR);
    M(T5UI_P_BG_OPA, LV_STYLE_BG_OPA, CV_OPA);
    M(T5UI_P_BG_GRAD_COLOR, LV_STYLE_BG_GRAD_COLOR, CV_COLOR);
    M(T5UI_P_BG_GRAD_DIR, LV_STYLE_BG_GRAD_DIR, CV_GRAD);
    M(T5UI_P_BG_MAIN_STOP, LV_STYLE_BG_MAIN_STOP, CV_OPA);
    M(T5UI_P_BG_GRAD_STOP, LV_STYLE_BG_GRAD_STOP, CV_OPA);
    M(T5UI_P_BORDER_WIDTH, LV_STYLE_BORDER_WIDTH, CV_PX);
    M(T5UI_P_BORDER_COLOR, LV_STYLE_BORDER_COLOR, CV_COLOR);
    M(T5UI_P_BORDER_OPA, LV_STYLE_BORDER_OPA, CV_OPA);
    M(T5UI_P_BORDER_SIDES, LV_STYLE_BORDER_SIDE, CV_SIDES);
    M(T5UI_P_OUTLINE_WIDTH, LV_STYLE_OUTLINE_WIDTH, CV_PX);
    M(T5UI_P_OUTLINE_COLOR, LV_STYLE_OUTLINE_COLOR, CV_COLOR);
    M(T5UI_P_OUTLINE_OPA, LV_STYLE_OUTLINE_OPA, CV_OPA);
    M(T5UI_P_OUTLINE_PAD, LV_STYLE_OUTLINE_PAD, CV_PX);
    M(T5UI_P_SHADOW_WIDTH, LV_STYLE_SHADOW_WIDTH, CV_PX);
    M(T5UI_P_SHADOW_COLOR, LV_STYLE_SHADOW_COLOR, CV_COLOR);
    M(T5UI_P_SHADOW_OPA, LV_STYLE_SHADOW_OPA, CV_OPA);
    M(T5UI_P_SHADOW_OFFSET_X, LV_STYLE_SHADOW_OFFSET_X, CV_PX);
    M(T5UI_P_SHADOW_OFFSET_Y, LV_STYLE_SHADOW_OFFSET_Y, CV_PX);
    M(T5UI_P_SHADOW_SPREAD, LV_STYLE_SHADOW_SPREAD, CV_PX);
    M(T5UI_P_RADIUS, LV_STYLE_RADIUS, CV_RADIUS);
    M(T5UI_P_CLIP_CORNER, LV_STYLE_CLIP_CORNER, CV_BOOL);
    M(T5UI_P_OPA, LV_STYLE_OPA, CV_OPA);
    M(T5UI_P_TRANSLATE_X, LV_STYLE_TRANSLATE_X, CV_SIZE);
    M(T5UI_P_TRANSLATE_Y, LV_STYLE_TRANSLATE_Y, CV_SIZE);
    M(T5UI_P_SCALE, LV_STYLE_TRANSFORM_SCALE_X, CV_SCALE);
    M(T5UI_P_ROTATION, LV_STYLE_TRANSFORM_ROTATION, CV_PX);
    M(T5UI_P_TEXT_COLOR, LV_STYLE_TEXT_COLOR, CV_COLOR);
    M(T5UI_P_TEXT_OPA, LV_STYLE_TEXT_OPA, CV_OPA);
    M(T5UI_P_TEXT_STYLE, LV_STYLE_TEXT_FONT, CV_FONT);
    M(T5UI_P_TEXT_ALIGN, LV_STYLE_TEXT_ALIGN, CV_TALIGN);
    M(T5UI_P_LETTER_SPACE, LV_STYLE_TEXT_LETTER_SPACE, CV_PX);
    M(T5UI_P_LINE_SPACE, LV_STYLE_TEXT_LINE_SPACE, CV_PX);
    M(T5UI_P_TEXT_DECOR, LV_STYLE_TEXT_DECOR, CV_DECOR);
    M(T5UI_P_LINE_WIDTH, LV_STYLE_LINE_WIDTH, CV_PX);
    M(T5UI_P_LINE_COLOR, LV_STYLE_LINE_COLOR, CV_COLOR);
    M(T5UI_P_LINE_OPA, LV_STYLE_LINE_OPA, CV_OPA);
    M(T5UI_P_LINE_ROUNDED, LV_STYLE_LINE_ROUNDED, CV_BOOL);
    M(T5UI_P_ARC_WIDTH, LV_STYLE_ARC_WIDTH, CV_PX);
    M(T5UI_P_ARC_COLOR, LV_STYLE_ARC_COLOR, CV_COLOR);
    M(T5UI_P_ARC_OPA, LV_STYLE_ARC_OPA, CV_OPA);
    M(T5UI_P_ARC_ROUNDED, LV_STYLE_ARC_ROUNDED, CV_BOOL);
    M(T5UI_P_IMAGE_RECOLOR, LV_STYLE_IMAGE_RECOLOR, CV_COLOR);
    M(T5UI_P_IMAGE_RECOLOR_OPA, LV_STYLE_IMAGE_RECOLOR_OPA, CV_OPA);
#undef M
    default: return false;
    }
}

/* Where a look property goes: an object's local style, or a style. */
typedef struct {
    lv_obj_t *obj;
    lv_style_t *style;
    lv_style_selector_t sel;
} dest_t;

static void put(const dest_t *d, lv_style_prop_t sp, lv_style_value_t v)
{
    if (d->obj) lv_obj_set_local_style_prop(d->obj, sp, v, d->sel);
    else lv_style_set_prop(d->style, sp, v);
}

static t5_err_t look_set(const dest_t *d, t5ui_prop_t p, const t5ui_val_t *val)
{
    lv_style_prop_t sp;
    uint8_t cv;
    if (!look(p, &sp, &cv)) return T5_ERR_NOT_SUPPORTED;
    int32_t v = val->i;
    lv_style_value_t sv = { .num = v };
    switch (cv) {
    case CV_PX: break;
    case CV_SIZE: sv.num = sz(v); break;
    case CV_COLOR: sv.color = col(val->c); break;
    case CV_OPA: sv.num = clamp(v, 0, 255); break;
    case CV_RADIUS: sv.num = v >= T5UI_RADIUS_CIRCLE ? LV_RADIUS_CIRCLE : v; break;
    case CV_ALIGN:
        if (!IN(v, ALIGNS)) return T5_ERR_INVALID_ARG;
        sv.num = ALIGNS[v];
        break;
    case CV_FLOW:
        if (!IN(v, FLOWS)) return T5_ERR_INVALID_ARG;
        put(d, LV_STYLE_LAYOUT, (lv_style_value_t){ .num = v ? LV_LAYOUT_FLEX : LV_LAYOUT_NONE });
        if (!v) return T5_OK;
        sv.num = FLOWS[v];
        break;
    case CV_PLACE:
        if (!IN(v, PLACES)) return T5_ERR_INVALID_ARG;
        sv.num = PLACES[v];
        break;
    case CV_GRAD:
        if (!IN(v, GRADS)) return T5_ERR_INVALID_ARG;
        sv.num = GRADS[v];
        break;
    case CV_SIDES: sv.num = (int32_t)sides(v); break;
    case CV_TALIGN:
        if (!IN(v, TALIGNS)) return T5_ERR_INVALID_ARG;
        sv.num = TALIGNS[v];
        break;
    case CV_DECOR:
        sv.num = ((v & T5UI_DECOR_UNDERLINE) ? LV_TEXT_DECOR_UNDERLINE : 0) |
                 ((v & T5UI_DECOR_STRIKE) ? LV_TEXT_DECOR_STRIKETHROUGH : 0);
        break;
    case CV_FONT:
        if ((uint32_t)v > T5UI_TEXT_MONO) return T5_ERR_INVALID_ARG;
        if (d->obj && !d->sel && v != T5UI_TEXT_MONO) {
            /* The whole type style (letter spacing too), keeping the opacity. */
            md_text_style(d->obj, (md_type_role_t)v, lv_obj_get_style_text_opa(d->obj, LV_PART_MAIN));
            return T5_OK;
        }
        sv.ptr = font(v);
        break;
    case CV_BOOL: sv.num = !!v; break;
    case CV_SCALE:
        put(d, LV_STYLE_TRANSFORM_SCALE_Y, sv);
        break;
    case CV_PAD_ALL:
        put(d, LV_STYLE_PAD_BOTTOM, sv);
        put(d, LV_STYLE_PAD_LEFT, sv);
        put(d, LV_STYLE_PAD_RIGHT, sv);
        break;
    }
    put(d, sp, sv);
    return T5_OK;
}

static bool look_get(lv_obj_t *o, t5ui_prop_t p, t5ui_val_t *out)
{
    lv_style_prop_t sp;
    uint8_t cv;
    if (!look(p, &sp, &cv)) return false;
    lv_style_value_t sv = lv_obj_get_style_prop(o, LV_PART_MAIN, sp);
    int32_t v = sv.num;
    switch (cv) {
    case CV_SIZE: v = unsz(v); break;
    case CV_COLOR: out->c = uncol(sv.color); return true;
    case CV_RADIUS: v = v == LV_RADIUS_CIRCLE ? T5UI_RADIUS_CIRCLE : v; break;
    case CV_ALIGN: v = BACK(ALIGNS, (lv_align_t)v, 0); break;
    case CV_FLOW:
        if (lv_obj_get_style_prop(o, LV_PART_MAIN, LV_STYLE_LAYOUT).num != LV_LAYOUT_FLEX) {
            v = T5UI_FLOW_NONE;
            break;
        }
        for (v = N(FLOWS) - 1; v > T5UI_ROW && FLOWS[v] != (lv_flex_flow_t)sv.num; v--) {}
        break;
    case CV_PLACE: v = BACK(PLACES, (lv_flex_align_t)v, 0); break;
    case CV_GRAD: v = BACK(GRADS, (lv_grad_dir_t)v, 0); break;
    case CV_SIDES: v = unsides((uint32_t)v); break;
    case CV_TALIGN: v = BACK(TALIGNS, (lv_text_align_t)v, 0); break;
    case CV_DECOR:
        v = ((v & LV_TEXT_DECOR_UNDERLINE) ? T5UI_DECOR_UNDERLINE : 0) |
            ((v & LV_TEXT_DECOR_STRIKETHROUGH) ? T5UI_DECOR_STRIKE : 0);
        break;
    case CV_FONT: v = unfont(sv.ptr); break;
    default: break;
    }
    out->i = v;
    return true;
}

/* ---- events ------------------------------------------------------------------------------ */
struct t5ui_event {
    lv_event_t *e;
    struct rec *r;
    t5ui_paint_t *paint;
};

struct t5ui_paint {
    lv_layer_t *layer;
    int32_t ox, oy;            /* the target's top left on the layer */
};

typedef struct rec {
    t5ui_event_cb_t cb;
    void *user;
    lv_obj_t *obj;             /* the handle it was added to */
    t5ui_ev_t kind;
} rec_t;

/* T5UI_EV_BIND, T5UI_EV_ITEM: the event's parameter. */
typedef struct {
    lv_obj_t *row;
    int32_t index;
} item_t;

static uint32_t s_code_back, s_code_bind, s_code_item, s_code_item_long, s_code_anim_end;

static void codes_init(void)
{
    if (s_code_back) return;
    s_code_back = lv_event_register_id();
    s_code_bind = lv_event_register_id();
    s_code_item = lv_event_register_id();
    s_code_item_long = lv_event_register_id();
    s_code_anim_end = lv_event_register_id();
}

static lv_event_code_t code_of(t5ui_ev_t ev)
{
    switch (ev) {
    case T5UI_EV_CLICK: return LV_EVENT_CLICKED;
    case T5UI_EV_LONG_PRESS: return LV_EVENT_LONG_PRESSED;
    case T5UI_EV_PRESS: return LV_EVENT_PRESSED;
    case T5UI_EV_RELEASE: return LV_EVENT_RELEASED;
    case T5UI_EV_VALUE: return LV_EVENT_VALUE_CHANGED;
    case T5UI_EV_FOCUS: return LV_EVENT_FOCUSED;
    case T5UI_EV_DEFOCUS: return LV_EVENT_DEFOCUSED;
    case T5UI_EV_SCROLL: return LV_EVENT_SCROLL;
    case T5UI_EV_RESIZE: return LV_EVENT_SIZE_CHANGED;
    case T5UI_EV_DELETE: return LV_EVENT_DELETE;
    case T5UI_EV_DRAW: return LV_EVENT_DRAW_POST;
    case T5UI_EV_BACK: return (lv_event_code_t)s_code_back;
    case T5UI_EV_BIND: return (lv_event_code_t)s_code_bind;
    case T5UI_EV_ITEM: return (lv_event_code_t)s_code_item;
    case T5UI_EV_READY: return LV_EVENT_READY;
    case T5UI_EV_CANCEL: return LV_EVENT_CANCEL;
    case T5UI_EV_SHORT_CLICK: return LV_EVENT_SHORT_CLICKED;
    case T5UI_EV_DOUBLE_CLICK: return LV_EVENT_DOUBLE_CLICKED;
    case T5UI_EV_PRESSING: return LV_EVENT_PRESSING;
    case T5UI_EV_PRESS_LOST: return LV_EVENT_PRESS_LOST;
    case T5UI_EV_SCROLL_END: return LV_EVENT_SCROLL_END;
    case T5UI_EV_KEY: return (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS);   /* before the widget */
    case T5UI_EV_INSERT: return LV_EVENT_INSERT;
    case T5UI_EV_GESTURE: return LV_EVENT_GESTURE;
    case T5UI_EV_ANIM_END: return (lv_event_code_t)s_code_anim_end;
    case T5UI_EV_ITEM_LONG: return (lv_event_code_t)s_code_item_long;
    case T5UI_EV_CHILD_CREATED: return LV_EVENT_CHILD_CREATED;
    case T5UI_EV_DRAW_BEHIND: return LV_EVENT_DRAW_MAIN;
    default: return LV_EVENT_ALL;
    }
}

static void free_user(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void tramp(lv_event_t *e)
{
    rec_t *r = lv_event_get_user_data(e);
    t5ui_event_t ev = { .e = e, .r = r };
    t5ui_paint_t paint;
    if (r->kind == T5UI_EV_DRAW || r->kind == T5UI_EV_DRAW_BEHIND) {
        lv_area_t a;
        lv_obj_get_coords(lv_event_get_current_target_obj(e), &a);
        paint = (t5ui_paint_t){ lv_event_get_layer(e), a.x1, a.y1 };
        ev.paint = &paint;
    }
    r->cb(&ev);
    if (r->kind == T5UI_EV_DELETE) free(r);    /* its own record goes last */
}

/* Events a text field's text area has. */
static bool field_event(t5ui_ev_t ev)
{
    return ev == T5UI_EV_VALUE || ev == T5UI_EV_FOCUS || ev == T5UI_EV_DEFOCUS || ev == T5UI_EV_READY ||
           ev == T5UI_EV_CANCEL || ev == T5UI_EV_KEY || ev == T5UI_EV_INSERT;
}

_Static_assert(T5UI_CHART_NONE == LV_CHART_POINT_NONE, "no-value points are LVGL's");

t5_err_t t5ui_be_on(t5ui_obj_t *obj, t5ui_ev_t ev, t5ui_event_cb_t cb, void *user)
{
    codes_init();
    lv_obj_t *o = O(obj);
    t5ui_kind_t k = kind_of(o);
    lv_obj_t *on = o;
    /* A text field's events happen on its text area. */
    if (is_field(k) && field_event(ev)) on = md_textfield_ta(o);
    if (k == T5UI_MD_ROW && ev == T5UI_EV_CLICK && !lv_obj_is_clickable(o)) {
        md_row_set_clickable(o, NULL, NULL);   /* clickable with a ripple */
    }
    rec_t *r = malloc(sizeof *r);
    if (!r) return T5_ERR_NO_MEM;
    *r = (rec_t){ cb, user, o, ev };
    lv_obj_add_event_cb(on, tramp, code_of(ev), r);
    if (ev != T5UI_EV_DELETE) lv_obj_add_event_cb(on, free_user, LV_EVENT_DELETE, r);
    if (ev == T5UI_EV_DRAW || ev == T5UI_EV_DRAW_BEHIND) lv_obj_invalidate(o);
    return T5_OK;
}

void t5ui_be_off(t5ui_obj_t *obj, t5ui_ev_t ev, t5ui_event_cb_t cb, void *user)
{
    lv_obj_t *o = O(obj);
    lv_obj_t *on = is_field(kind_of(o)) && field_event(ev) ? md_textfield_ta(o) : o;
    bool again = true;
    while (again) {
        again = false;
        for (uint32_t i = 0; i < lv_obj_get_event_count(on); i++) {
            lv_event_dsc_t *d = lv_obj_get_event_dsc(on, i);
            rec_t *r = lv_event_dsc_get_user_data(d);
            if (lv_event_dsc_get_cb(d) != tramp || r->obj != o || r->kind != ev || r->cb != cb) continue;
            if (user && r->user != user) continue;
            lv_obj_remove_event_dsc(on, d);
            for (uint32_t j = 0; j < lv_obj_get_event_count(on); j++) {
                lv_event_dsc_t *f = lv_obj_get_event_dsc(on, j);
                if (lv_event_dsc_get_cb(f) == free_user && lv_event_dsc_get_user_data(f) == r) {
                    lv_obj_remove_event_dsc(on, f);
                    break;
                }
            }
            free(r);
            again = true;
            break;
        }
    }
}

t5ui_obj_t *t5ui_event_obj(t5ui_event_t *e)
{
    return e ? H(e->r->obj) : NULL;
}

static bool indexed(t5ui_ev_t k)
{
    return k == T5UI_EV_BIND || k == T5UI_EV_ITEM || k == T5UI_EV_ITEM_LONG || k == T5UI_EV_ANIM_END;
}

t5ui_obj_t *t5ui_event_target(t5ui_event_t *e)
{
    if (!e) return NULL;
    if (e->r->kind == T5UI_EV_CHILD_CREATED) {
        lv_obj_t *child = lv_event_get_param(e->e);
        return H(child ? child : lv_event_get_target_obj(e->e));
    }
    if (indexed(e->r->kind)) {
        item_t *it = lv_event_get_param(e->e);
        return H(it && it->row ? it->row : e->r->obj);
    }
    return H(lv_event_get_target_obj(e->e));
}

void *t5ui_event_user(t5ui_event_t *e)
{
    return e ? e->r->user : NULL;
}

t5ui_ev_t t5ui_event_kind(t5ui_event_t *e)
{
    return e ? e->r->kind : 0;
}

int32_t t5ui_event_index(t5ui_event_t *e)
{
    if (!e || !indexed(e->r->kind)) return -1;
    item_t *it = lv_event_get_param(e->e);
    return it ? it->index : -1;
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

uint32_t t5ui_event_key(t5ui_event_t *e)
{
    return e && e->r->kind == T5UI_EV_KEY ? lv_event_get_key(e->e) : 0;
}

_Static_assert(T5UI_SWIPE_LEFT == LV_DIR_LEFT && T5UI_SWIPE_RIGHT == LV_DIR_RIGHT && T5UI_SWIPE_UP == LV_DIR_TOP &&
               T5UI_SWIPE_DOWN == LV_DIR_BOTTOM, "swipe bits are LVGL's directions");
_Static_assert(T5UI_KEY_UP == LV_KEY_UP && T5UI_KEY_DOWN == LV_KEY_DOWN && T5UI_KEY_RIGHT == LV_KEY_RIGHT &&
               T5UI_KEY_LEFT == LV_KEY_LEFT && T5UI_KEY_ESC == LV_KEY_ESC && T5UI_KEY_DEL == LV_KEY_DEL &&
               T5UI_KEY_BACKSPACE == LV_KEY_BACKSPACE && T5UI_KEY_ENTER == LV_KEY_ENTER &&
               T5UI_KEY_NEXT == LV_KEY_NEXT && T5UI_KEY_PREV == LV_KEY_PREV && T5UI_KEY_HOME == LV_KEY_HOME &&
               T5UI_KEY_END == LV_KEY_END, "key codes are LVGL's");

uint32_t t5ui_event_dir(t5ui_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (!e || e->r->kind != T5UI_EV_GESTURE || !in) return 0;
    return (uint32_t)lv_indev_get_gesture_dir(in) & (T5UI_SWIPE_LEFT | T5UI_SWIPE_RIGHT | T5UI_SWIPE_UP | T5UI_SWIPE_DOWN);
}

const char *t5ui_event_text(t5ui_event_t *e)
{
    return e && e->r->kind == T5UI_EV_INSERT ? lv_event_get_param(e->e) : NULL;
}

void t5ui_event_set_text(t5ui_event_t *e, const char *s)
{
    if (!e || e->r->kind != T5UI_EV_INSERT) return;
    lv_textarea_set_insert_replace(lv_event_get_current_target_obj(e->e), s ? s : "");
}

void t5ui_event_stop(t5ui_event_t *e)
{
    if (e) lv_event_stop_processing(e->e);
}

/* md's back arrows: T5UI_EV_BACK on the app bar or page that has it. */
static void back_clicked(lv_event_t *e)
{
    for (lv_obj_t *o = lv_event_get_current_target_obj(e); o; o = lv_obj_get_parent(o)) {
        t5ui_kind_t k = kind_of(o);
        if (k == T5UI_MD_APPBAR_BACK || k == T5UI_MD_PAGE) {
            lv_obj_send_event(o, (lv_event_code_t)s_code_back, NULL);
            return;
        }
    }
}

/* An md list's row being bound: T5UI_P_TEXT and the others then go to its labels. */
static md_list_row_t *s_binding;

static void list_bind(md_list_row_t *row, uint32_t index, void *user)
{
    node_t *n = user;
    item_t it = { row->row, (int32_t)index };
    md_list_row_t *outer = s_binding;
    s_binding = row;
    lv_obj_send_event(n->obj, (lv_event_code_t)s_code_bind, &it);
    s_binding = outer;
}

static void list_click(uint32_t index, void *user)
{
    node_t *n = user;
    item_t it = { NULL, (int32_t)index };
    lv_obj_send_event(n->obj, (lv_event_code_t)s_code_item, &it);
}

static void list_long(uint32_t index, void *user)
{
    node_t *n = user;
    item_t it = { NULL, (int32_t)index };
    lv_obj_send_event(n->obj, (lv_event_code_t)s_code_item_long, &it);
}

/* ---- the tree --------------------------------------------------------------------------------- */
t5ui_obj_t *t5ui_root(struct t5_app *app)
{
    return app ? H(t5_app_screen((t5_app_t *)app)) : NULL;
}

static lv_obj_t *make(lv_obj_t *parent, t5ui_kind_t kind, node_t *n)
{
    const md_scheme_t *sc = md_scheme();
    switch (kind) {
    case T5UI_BOX: {
        lv_obj_t *o = lv_obj_create(parent);
        lv_obj_remove_style_all(o);
        lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        return o;
    }
    case T5UI_PANEL: {
        lv_obj_t *o = lv_obj_create(parent);
        lv_obj_set_style_bg_color(o, col(sc->c.surface), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(o, 0, 0);
        lv_obj_set_style_radius(o, T5_DP(MD_SHAPE_MEDIUM_DP), 0);
        lv_obj_set_style_pad_all(o, T5_DP(16), 0);
        return o;
    }
    case T5UI_LABEL: return lv_label_create(parent);
    case T5UI_BUTTON: return lv_button_create(parent);
    case T5UI_IMAGE: return lv_image_create(parent);
    case T5UI_CANVAS: return lv_canvas_create(parent);
    case T5UI_SWITCH: return lv_switch_create(parent);
    case T5UI_CHECKBOX: {
        lv_obj_t *o = lv_checkbox_create(parent);
        lv_checkbox_set_text(o, "");
        return o;
    }
    case T5UI_SLIDER: return lv_slider_create(parent);
    case T5UI_ARC: {
        lv_obj_t *a = lv_arc_create(parent);
        lv_obj_set_style_arc_color(a, col(sc->c.primary), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(a, col(sc->c.primary), LV_PART_KNOB);
        return a;
    }
    case T5UI_BAR: {
        lv_obj_t *b = lv_bar_create(parent);
        lv_obj_set_style_bg_color(b, col(sc->c.primary), LV_PART_INDICATOR);
        return b;
    }
    case T5UI_SPINNER: return lv_spinner_create(parent);
    case T5UI_LED: return lv_led_create(parent);
    case T5UI_TEXTAREA: return lv_textarea_create(parent);
    case T5UI_DROPDOWN: return lv_dropdown_create(parent);
    case T5UI_ROLLER: return lv_roller_create(parent);
    case T5UI_CHART: {
        lv_obj_t *c = lv_chart_create(parent);
        lv_chart_set_update_mode(c, LV_CHART_UPDATE_MODE_SHIFT);
        lv_obj_set_style_size(c, 0, 0, LV_PART_INDICATOR);   /* no dots on lines */
        return c;
    }
    case T5UI_TABLE: return lv_table_create(parent);
    case T5UI_KEYBOARD: return lv_keyboard_create(parent);
    case T5UI_MD_TEXT: return md_label_create(parent, "", MD_TYPE_BODY1);
    case T5UI_MD_ICON: return md_icon_create(parent, "", MD_ICON_DP);
    case T5UI_MD_BUTTON_TEXT: return md_button_create(parent, "", MD_BUTTON_TEXT);
    case T5UI_MD_BUTTON_OUTLINED: return md_button_create(parent, "", MD_BUTTON_OUTLINED);
    case T5UI_MD_BUTTON_CONTAINED: return md_button_create(parent, "", MD_BUTTON_CONTAINED);
    case T5UI_MD_FAB: return md_fab_create(parent, "", false);
    case T5UI_MD_FAB_MINI: return md_fab_create(parent, "", true);
    case T5UI_MD_CARD: return md_card_create(parent);
    case T5UI_MD_DIVIDER: return md_divider_create(parent);
    case T5UI_MD_APPBAR: return md_appbar_create(parent, "", NULL, NULL, NULL);
    case T5UI_MD_APPBAR_BACK: return md_appbar_create(parent, "", MD_ICON_ARROW_BACK, back_clicked, NULL);
    case T5UI_MD_APPBAR_ACTION: return md_appbar_add_action(parent, "", NULL, NULL);
    case T5UI_MD_ROW: return md_row_create(parent, NULL, "", NULL);
    case T5UI_MD_ROW_SWITCH: return md_row_switch(parent, false);
    case T5UI_MD_SECTION: return md_section_create(parent, "");
    case T5UI_MD_SWITCH: return md_switch_create(parent, false);
    case T5UI_MD_CHECKBOX: return md_checkbox_create(parent, "", false);
    case T5UI_MD_SLIDER: return md_slider_create(parent, 0, 100, 0);
    case T5UI_MD_TEXTFIELD: return md_textfield_create(parent, "", false);
    case T5UI_MD_PASSWORD: return md_textfield_create(parent, "", true);
    case T5UI_MD_CHIP_ACTION: return md_chip_create(parent, "", NULL, MD_CHIP_ACTION);
    case T5UI_MD_CHIP_FILTER: return md_chip_create(parent, "", NULL, MD_CHIP_FILTER);
    case T5UI_MD_CHIP_CHOICE: return md_chip_create(parent, "", NULL, MD_CHIP_CHOICE);
    case T5UI_MD_TABS: return md_tabs_create(parent);
    case T5UI_MD_TAB: return md_tabs_add(parent, "");
    case T5UI_MD_PAGE: return md_page_create(parent, "", back_clicked, NULL, &n->bar);
    case T5UI_MD_LIST: {
        lv_obj_t *l = md_list_create(parent, 0, MD_LIST_ROW_1_DP, list_bind, list_click, n);
        if (l) md_list_set_long_press(l, list_long);
        return l;
    }
    case T5UI_MD_ROW_TRAILING: return md_row_trailing_text(parent, "");
    default: return NULL;
    }
}

t5ui_obj_t *t5ui_be_create(t5ui_obj_t *parent, t5ui_kind_t kind)
{
    codes_init();
    lv_obj_t *p = O(parent);
    node_t *pn = node_of(p);
    t5ui_kind_t pk = pn ? (t5ui_kind_t)pn->kind : 0;
    /* Children that only fit in one kind of parent. */
    if (kind == T5UI_MD_APPBAR_ACTION) {
        if (pk == T5UI_MD_PAGE) p = pn->bar;
        else if (!is_appbar(pk)) return NULL;
    } else if (kind == T5UI_MD_TAB) {
        if (pk != T5UI_MD_TABS) return NULL;
    } else if (kind == T5UI_MD_ROW_SWITCH || kind == T5UI_MD_ROW_TRAILING) {
        if (pk != T5UI_MD_ROW) return NULL;
    } else if (pk == T5UI_MD_TABS || pk == T5UI_MD_LIST) {
        return NULL;                           /* content goes in a tab; list rows are bound */
    } else if (pk == T5UI_MD_PAGE) {
        p = md_page_body(p);                   /* content goes under the app bar */
    }
    node_t *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    lv_obj_t *o = make(p, kind, n);
    if (!o) {
        free(n);
        return NULL;
    }
    n->obj = o;
    n->kind = (uint16_t)kind;
    n->ymax = 100;
    n->grid_rows = 3;
    n->grid_cols = 5;
    lv_obj_add_event_cb(o, node_free, LV_EVENT_DELETE, n);
    if (kind == T5UI_CHART) lv_chart_set_axis_range(o, LV_CHART_AXIS_PRIMARY_Y, n->ymin, n->ymax);
    return H(o);
}

t5ui_kind_t t5ui_kind(t5ui_obj_t *obj)
{
    return kind_of(O(obj));
}

void t5ui_delete(t5ui_obj_t *obj)
{
    if (obj) lv_obj_delete(O(obj));
}

void t5ui_clean(t5ui_obj_t *obj)
{
    if (!obj) return;
    node_t *n = node_of(O(obj));
    lv_obj_clean(n && n->kind == T5UI_MD_PAGE ? md_page_body(O(obj)) : O(obj));
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

void t5ui_move_to(t5ui_obj_t *obj, int32_t index)
{
    if (obj) lv_obj_move_to_index(O(obj), index);
}

void t5ui_set_user(t5ui_obj_t *obj, void *user)
{
    node_t *n = obj ? node_need(O(obj)) : NULL;
    if (n) n->user = user;
}

void *t5ui_user(t5ui_obj_t *obj)
{
    node_t *n = obj ? node_of(O(obj)) : NULL;
    return n ? n->user : NULL;
}

static void rect_from(const lv_area_t *a, t5ui_rect_t *out)
{
    *out = (t5ui_rect_t){ a->x1, a->y1, a->x2, a->y2 };
}

void t5ui_coords(t5ui_obj_t *obj, t5ui_rect_t *out)
{
    if (!obj || !out) return;
    lv_obj_update_layout(O(obj));
    lv_area_t a;
    lv_obj_get_coords(O(obj), &a);
    rect_from(&a, out);
}

void t5ui_content_coords(t5ui_obj_t *obj, t5ui_rect_t *out)
{
    if (!obj || !out) return;
    lv_obj_update_layout(O(obj));
    lv_area_t a;
    lv_obj_get_content_coords(O(obj), &a);
    rect_from(&a, out);
}

void t5ui_layout_now(t5ui_obj_t *obj)
{
    if (obj) lv_obj_update_layout(O(obj));
}

void t5ui_invalidate(t5ui_obj_t *obj)
{
    if (obj) lv_obj_invalidate(O(obj));
}

void t5ui_scroll_into_view(t5ui_obj_t *obj, bool animate)
{
    if (obj) lv_obj_scroll_to_view_recursive(O(obj), anim_on(animate));
}

/* ---- widget properties ------------------------------------------------------------------- */
/* A label inside obj (a button's), made if make and there's none. */
static lv_obj_t *inner_label(lv_obj_t *o, bool make)
{
    lv_obj_t *c = lv_obj_get_child(o, 0);
    if (c && IS(c, lv_label_class)) return c;
    if (!make) return NULL;
    c = lv_label_create(o);
    lv_obj_center(c);
    return c;
}

/* A tab's index and its tab view. */
static lv_obj_t *tab_view(lv_obj_t *tab, uint32_t *idx)
{
    lv_obj_t *content = lv_obj_get_parent(tab);
    *idx = (uint32_t)lv_obj_get_index(tab);
    return content ? lv_obj_get_parent(content) : NULL;
}

/* What text property p of o is: the label, or NULL. */
static lv_obj_t *text_label(lv_obj_t *o, t5ui_kind_t k, t5ui_prop_t p)
{
    if (s_binding && o == s_binding->row) {
        return p == T5UI_P_TEXT ? s_binding->primary : p == T5UI_P_SECONDARY ? s_binding->secondary
                                                     : p == T5UI_P_ICON      ? s_binding->icon
                                                                             : NULL;
    }
    switch (p) {
    case T5UI_P_TEXT:
        if (k == T5UI_MD_ROW) return md_row_label(o, false);
        if (is_chip(k)) return lv_obj_get_child(o, -1);
        if (is_md_button(k) || k == T5UI_BUTTON) return inner_label(o, false);
        if (is_appbar(k)) return md_appbar_title(o);
        if (IS(o, lv_label_class)) return o;
        return NULL;
    case T5UI_P_SECONDARY: return k == T5UI_MD_ROW ? md_row_label(o, true) : NULL;
    case T5UI_P_TITLE:
        if (is_appbar(k)) return md_appbar_title(o);
        if (k == T5UI_MD_PAGE) {
            node_t *n = node_of(o);
            return n && n->bar ? md_appbar_title(n->bar) : NULL;
        }
        return NULL;
    case T5UI_P_ICON:
        if (k == T5UI_MD_ICON) return o;
        if (k == T5UI_MD_ROW) return md_row_icon(o);
        if (k == T5UI_MD_FAB || k == T5UI_MD_FAB_MINI || k == T5UI_MD_APPBAR_ACTION) return inner_label(o, false);
        if (is_chip(k)) {
            /* A filter chip's first icon is its check mark. */
            uint32_t i = k == T5UI_MD_CHIP_FILTER ? 1 : 0;
            return lv_obj_get_child_count(o) > i + 1 ? lv_obj_get_child(o, (int32_t)i) : NULL;
        }
        return NULL;
    default: return NULL;
    }
}

static t5_err_t set_text(lv_obj_t *o, t5ui_kind_t k, t5ui_prop_t p, const char *s)
{
    if (p == T5UI_P_TEXT) {
        if (IS(o, lv_checkbox_class)) {
            lv_checkbox_set_text(o, s);
            return T5_OK;
        }
        if (IS(o, lv_textarea_class)) {
            lv_textarea_set_text(o, s);
            return T5_OK;
        }
        if (is_field(k)) {
            md_textfield_set_text(o, s);
            return T5_OK;
        }
        if (k == T5UI_MD_TAB) {
            uint32_t idx;
            lv_obj_t *tv = tab_view(o, &idx);
            if (!tv) return T5_ERR_NOT_SUPPORTED;
            lv_tabview_set_tab_text(tv, idx, s);
            return T5_OK;
        }
        if (is_md_button(k)) {
            lv_obj_t *l = inner_label(o, false);
            if (l) md_label_set_text(l, s, MD_TYPE_BUTTON);
            return l ? T5_OK : T5_ERR_NOT_SUPPORTED;
        }
        if (k == T5UI_BUTTON) {
            lv_label_set_text(inner_label(o, true), s);
            return T5_OK;
        }
    }
    if (p == T5UI_P_SECONDARY && k == T5UI_MD_ROW && !(s_binding && o == s_binding->row)) {
        md_row_set_secondary(o, s);
        return T5_OK;
    }
    if (p == T5UI_P_ICON && k == T5UI_MD_ROW && !(s_binding && o == s_binding->row)) {
        md_row_set_icon(o, s);
        return T5_OK;
    }
    if (p == T5UI_P_ICON && is_chip(k) && !text_label(o, k, p)) {
        lv_obj_t *icon = md_icon_create(o, s, 18);
        lv_obj_move_to_index(icon, k == T5UI_MD_CHIP_FILTER ? 1 : 0);
        return T5_OK;
    }
    if (p == T5UI_P_TITLE || (p == T5UI_P_TEXT && (is_appbar(k) || k == T5UI_MD_PAGE))) {
        node_t *n = k == T5UI_MD_PAGE ? node_of(o) : NULL;
        lv_obj_t *bar = is_appbar(k) ? o : n ? n->bar : NULL;
        if (!bar) return T5_ERR_NOT_SUPPORTED;
        md_appbar_set_title(bar, s);
        return T5_OK;
    }
    lv_obj_t *l = text_label(o, k, p);
    if (!l) return T5_ERR_NOT_SUPPORTED;
    lv_label_set_text(l, s);
    return T5_OK;
}

static const char *get_text(lv_obj_t *o, t5ui_kind_t k, t5ui_prop_t p)
{
    if (p == T5UI_P_TEXT) {
        if (IS(o, lv_checkbox_class)) return lv_checkbox_get_text(o);
        if (IS(o, lv_textarea_class)) return lv_textarea_get_text(o);
        if (is_field(k)) return md_textfield_text(o);
        if (k == T5UI_MD_TAB) {
            uint32_t idx;
            lv_obj_t *tv = tab_view(o, &idx);
            lv_obj_t *b = tv ? lv_tabview_get_tab_button(tv, (int32_t)idx) : NULL;
            lv_obj_t *l = b ? inner_label(b, false) : NULL;
            return l ? lv_label_get_text(l) : NULL;
        }
        if (k == T5UI_MD_PAGE) p = T5UI_P_TITLE;
    }
    lv_obj_t *l = text_label(o, k, p);
    return l ? lv_label_get_text(l) : NULL;
}

/* A text field's text area, or o itself if it's one. */
static lv_obj_t *textarea_of(lv_obj_t *o, t5ui_kind_t k)
{
    if (is_field(k)) return md_textfield_ta(o);
    return IS(o, lv_textarea_class) ? o : NULL;
}

static lv_chart_series_t *series_at(lv_obj_t *c, int32_t index)
{
    if (index < 0) return NULL;
    lv_chart_series_t *s = NULL;
    for (int32_t i = 0; i <= index; i++) {
        s = lv_chart_get_series_next(c, s);
        if (!s) return NULL;
    }
    return s;
}

static t5_err_t set_widget(lv_obj_t *o, node_t *n, t5ui_kind_t k, t5ui_prop_t p, const t5ui_val_t *val)
{
    int32_t v = val->i;
    lv_obj_t *ta = textarea_of(o, k);
    switch (p) {
    case T5UI_P_FLOATING: lv_obj_set_ignore_layout(o, v); return T5_OK;
    case T5UI_P_ELEVATION:
        md_set_elevation(o, v, true);
        if (n || (n = node_need(o))) n->elevation = (int16_t)v;
        return T5_OK;
    case T5UI_P_LONG_MODE:
        if (!IS(o, lv_label_class)) return T5_ERR_NOT_SUPPORTED;
        if (!IN(v, LONGS)) return T5_ERR_INVALID_ARG;
        lv_label_set_long_mode(o, LONGS[v]);
        return T5_OK;
    case T5UI_P_ICON_SIZE:
        if (k != T5UI_MD_ICON) return T5_ERR_NOT_SUPPORTED;
        lv_obj_set_style_text_font(o, md_icon_font(v), 0);
        return T5_OK;
    case T5UI_P_PLACEHOLDER:
        if (is_field(k)) md_textfield_set_label(o, val->s);
        else if (ta) lv_textarea_set_placeholder_text(ta, val->s);
        else return T5_ERR_NOT_SUPPORTED;
        return T5_OK;
    case T5UI_P_HELPER:
    case T5UI_P_HELPER_ERROR: {
        if (!is_field(k) || !n) return T5_ERR_NOT_SUPPORTED;
        /* md sets both at once: keep the other. */
        lv_obj_t *helper = lv_obj_get_child(o, -1);
        const char *text = p == T5UI_P_HELPER ? val->s : (helper ? lv_label_get_text(helper) : "");
        char *copy = strdup(text ? text : "");
        if (!copy) return T5_ERR_NO_MEM;
        if (p == T5UI_P_HELPER_ERROR) n->helper_error = !!v;
        md_textfield_set_helper(o, copy, n->helper_error);
        free(copy);
        return T5_OK;
    }
    case T5UI_P_IMAGE_SCALE:
        if (!IS(o, lv_image_class)) return T5_ERR_NOT_SUPPORTED;
        lv_image_set_scale(o, (uint32_t)clamp(v, 1, 65535));
        return T5_OK;
    case T5UI_P_IMAGE_ROTATION:
        if (!IS(o, lv_image_class)) return T5_ERR_NOT_SUPPORTED;
        lv_image_set_rotation(o, v);
        return T5_OK;
    case T5UI_P_HIDDEN: lv_obj_set_hidden(o, v); return T5_OK;
    case T5UI_P_CLICKABLE:
        if (k == T5UI_MD_ROW && v && !lv_obj_is_clickable(o)) md_row_set_clickable(o, NULL, NULL);
        else lv_obj_set_clickable(o, v);
        return T5_OK;
    case T5UI_P_SCROLLABLE: lv_obj_set_scrollable(o, v); return T5_OK;
    case T5UI_P_SCROLL_DIR:
        if (!IN(v, DIRS)) return T5_ERR_INVALID_ARG;
        lv_obj_set_scroll_dir(o, DIRS[v]);
        return T5_OK;
    case T5UI_P_SCROLLBAR:
        if (!IN(v, SCROLLBARS)) return T5_ERR_INVALID_ARG;
        lv_obj_set_scrollbar_mode(o, SCROLLBARS[v]);
        return T5_OK;
    case T5UI_P_SCROLL_X: lv_obj_scroll_to_x(o, v, LV_ANIM_OFF); return T5_OK;
    case T5UI_P_SCROLL_Y: lv_obj_scroll_to_y(o, v, LV_ANIM_OFF); return T5_OK;
    case T5UI_P_CHECKED:
        if (is_chip(k)) md_chip_set_selected(o, v);
        else lv_obj_set_state(o, LV_STATE_CHECKED, v);
        return T5_OK;
    case T5UI_P_DISABLED: lv_obj_set_state(o, LV_STATE_DISABLED, v); return T5_OK;
    case T5UI_P_CHECKABLE: lv_obj_set_checkable(o, v); return T5_OK;
    case T5UI_P_BUBBLE: lv_obj_set_event_bubble(o, v); return T5_OK;
    case T5UI_P_SCROLL_SNAP_X:
    case T5UI_P_SCROLL_SNAP_Y:
        if (!IN(v, SNAPS)) return T5_ERR_INVALID_ARG;
        if (p == T5UI_P_SCROLL_SNAP_X) lv_obj_set_scroll_snap_x(o, SNAPS[v]);
        else lv_obj_set_scroll_snap_y(o, SNAPS[v]);
        return T5_OK;
    case T5UI_P_SCROLL_ONE: lv_obj_set_scroll_one(o, v); return T5_OK;
    case T5UI_P_SCROLL_ELASTIC: lv_obj_set_scroll_elastic(o, v); return T5_OK;
    case T5UI_P_SCROLL_CHAIN: lv_obj_set_scroll_chain(o, v); return T5_OK;
    case T5UI_P_SNAPPABLE: lv_obj_set_snappable(o, v); return T5_OK;
    case T5UI_P_EXT_CLICK:
        if (!n && !(n = node_need(o))) return T5_ERR_NO_MEM;
        n->ext_click = (int16_t)clamp(v, 0, 1000);
        lv_obj_set_ext_click_area(o, n->ext_click);
        return T5_OK;
    case T5UI_P_STATE: {
        static const struct { uint32_t t; lv_state_t l; } S[] = {
            { T5UI_STATE_CHECKED, LV_STATE_CHECKED }, { T5UI_STATE_FOCUSED, LV_STATE_FOCUSED },
            { T5UI_STATE_PRESSED, LV_STATE_PRESSED }, { T5UI_STATE_DISABLED, LV_STATE_DISABLED },
            { T5UI_STATE_EDITED, LV_STATE_EDITED },
        };
        if ((uint32_t)v & ~(uint32_t)T5UI_STATE_ALL) return T5_ERR_INVALID_ARG;
        for (size_t i = 0; i < sizeof S / sizeof S[0]; i++) {
            bool on = (uint32_t)v & S[i].t;
            if (S[i].t == T5UI_STATE_CHECKED && is_chip(k)) md_chip_set_selected(o, on);
            else lv_obj_set_state(o, S[i].l, on);
        }
        return T5_OK;
    }
    case T5UI_P_RIPPLE:
        if (v) md_ripple_attach(o);
        return T5_OK;
    case T5UI_P_CURSOR:
        if (!ta) return T5_ERR_NOT_SUPPORTED;
        lv_textarea_set_cursor_pos(ta, v);
        return T5_OK;
    case T5UI_P_VALUE:
        if (IS(o, lv_bar_class)) lv_bar_set_value(o, v, LV_ANIM_OFF);   /* sliders too */
        else if (IS(o, lv_arc_class)) lv_arc_set_value(o, v);
        else if (IS(o, lv_led_class)) lv_led_set_brightness(o, (uint8_t)clamp(v, 0, 255));
        else return T5_ERR_NOT_SUPPORTED;
        return T5_OK;
    case T5UI_P_MIN:
    case T5UI_P_MAX:
        if (IS(o, lv_bar_class)) {
            int32_t lo = p == T5UI_P_MIN ? v : lv_bar_get_min_value(o);
            int32_t hi = p == T5UI_P_MAX ? v : lv_bar_get_max_value(o);
            lv_bar_set_range(o, lo, hi);
        } else if (IS(o, lv_arc_class)) {
            int32_t lo = p == T5UI_P_MIN ? v : lv_arc_get_min_value(o);
            int32_t hi = p == T5UI_P_MAX ? v : lv_arc_get_max_value(o);
            lv_arc_set_range(o, lo, hi);
        } else if (IS(o, lv_chart_class) && n) {
            if (p == T5UI_P_MIN) n->ymin = v;
            else n->ymax = v;
            lv_chart_set_axis_range(o, LV_CHART_AXIS_PRIMARY_Y, n->ymin, n->ymax);
        } else {
            return T5_ERR_NOT_SUPPORTED;
        }
        return T5_OK;
    case T5UI_P_START_VALUE:
        if (!IS(o, lv_bar_class)) return T5_ERR_NOT_SUPPORTED;
        lv_bar_set_mode(o, LV_BAR_MODE_RANGE);
        lv_bar_set_start_value(o, v, LV_ANIM_OFF);
        return T5_OK;
    case T5UI_P_ARC_START:
    case T5UI_P_ARC_END:
        if (!IS(o, lv_arc_class)) return T5_ERR_NOT_SUPPORTED;
        lv_arc_set_bg_angles(o, p == T5UI_P_ARC_START ? v : (int32_t)lv_arc_get_bg_angle_start(o),
                             p == T5UI_P_ARC_END ? v : (int32_t)lv_arc_get_bg_angle_end(o));
        return T5_OK;
    case T5UI_P_ARC_ROTATION:
        if (!IS(o, lv_arc_class)) return T5_ERR_NOT_SUPPORTED;
        lv_arc_set_rotation(o, v);
        return T5_OK;
    case T5UI_P_SELECTED:
        if (v < 0) return T5_ERR_INVALID_ARG;
        if (IS(o, lv_dropdown_class)) lv_dropdown_set_selected(o, (uint32_t)v);
        else if (IS(o, lv_roller_class)) lv_roller_set_selected(o, (uint32_t)v, LV_ANIM_OFF);
        else if (IS(o, lv_tabview_class)) lv_tabview_set_active(o, (uint32_t)v, LV_ANIM_OFF);
        else return T5_ERR_NOT_SUPPORTED;
        return T5_OK;
    case T5UI_P_OPTIONS:
        if (IS(o, lv_dropdown_class)) lv_dropdown_set_options(o, val->s);
        else if (IS(o, lv_roller_class)) lv_roller_set_options(o, val->s, LV_ROLLER_MODE_NORMAL);
        else return T5_ERR_NOT_SUPPORTED;
        return T5_OK;
    case T5UI_P_COUNT:
        if (v < 0) return T5_ERR_INVALID_ARG;
        if (k == T5UI_MD_LIST) md_list_set_count(o, (uint32_t)v);
        else if (IS(o, lv_chart_class)) lv_chart_set_point_count(o, (uint32_t)v);
        else return T5_ERR_NOT_SUPPORTED;
        return T5_OK;
    case T5UI_P_ROWS:
    case T5UI_P_COLUMNS:
        if (!IS(o, lv_table_class)) return T5_ERR_NOT_SUPPORTED;
        if (v < 0) return T5_ERR_INVALID_ARG;
        if (p == T5UI_P_ROWS) lv_table_set_row_count(o, (uint32_t)v);
        else lv_table_set_column_count(o, (uint32_t)v);
        return T5_OK;
    case T5UI_P_ONE_LINE:
        if (!ta) return T5_ERR_NOT_SUPPORTED;
        lv_textarea_set_one_line(ta, v);
        return T5_OK;
    case T5UI_P_PASSWORD:
        if (!ta) return T5_ERR_NOT_SUPPORTED;
        lv_textarea_set_password_mode(ta, v);
        return T5_OK;
    case T5UI_P_MAX_LENGTH:
        if (!ta) return T5_ERR_NOT_SUPPORTED;
        lv_textarea_set_max_length(ta, (uint32_t)(v < 0 ? 0 : v));
        return T5_OK;
    case T5UI_P_TARGET: {
        if (!IS(o, lv_keyboard_class)) return T5_ERR_NOT_SUPPORTED;
        lv_obj_t *t = val->o ? textarea_of(O(val->o), kind_of(O(val->o))) : NULL;
        if (val->o && !t) return T5_ERR_INVALID_ARG;
        lv_keyboard_set_textarea(o, t);
        return T5_OK;
    }
    case T5UI_P_KEYBOARD_MODE:
        if (!IS(o, lv_keyboard_class)) return T5_ERR_NOT_SUPPORTED;
        lv_keyboard_set_mode(o, v == T5UI_KB_NUMBER ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
        return T5_OK;
    case T5UI_P_CHART_TYPE:
        if (!IS(o, lv_chart_class)) return T5_ERR_NOT_SUPPORTED;
        lv_chart_set_type(o, v == T5UI_CHART_BAR ? LV_CHART_TYPE_BAR : LV_CHART_TYPE_LINE);
        return T5_OK;
    case T5UI_P_CHART_GRID_ROWS:
    case T5UI_P_CHART_GRID_COLUMNS:
        if (!IS(o, lv_chart_class) || !n) return T5_ERR_NOT_SUPPORTED;
        if (p == T5UI_P_CHART_GRID_ROWS) n->grid_rows = (uint8_t)clamp(v, 0, 255);
        else n->grid_cols = (uint8_t)clamp(v, 0, 255);
        lv_chart_set_div_line_count(o, n->grid_rows, n->grid_cols);
        return T5_OK;
    case T5UI_P_ROW_DP:
        if (k != T5UI_MD_LIST) return T5_ERR_NOT_SUPPORTED;
        if (v < 24 || v > 200) return T5_ERR_INVALID_ARG;
        md_list_set_row_dp(o, v);
        return T5_OK;
    case T5UI_P_ANIM_TIME:
        if (!IS(o, lv_spinner_class)) return T5_ERR_NOT_SUPPORTED;
        lv_spinner_set_anim_params(o, (uint32_t)clamp(v, 1, 60000), lv_spinner_get_arc_sweep(o));
        return T5_OK;
    default: return T5_ERR_NOT_SUPPORTED;
    }
}

static bool get_widget(lv_obj_t *o, node_t *n, t5ui_kind_t k, t5ui_prop_t p, t5ui_val_t *out)
{
    lv_obj_t *ta = textarea_of(o, k);
    int32_t v;
    switch (p) {
    case T5UI_P_FLOATING: v = lv_obj_is_ignore_layout(o); break;
    case T5UI_P_ELEVATION: v = n ? n->elevation : 0; break;
    case T5UI_P_LONG_MODE:
        if (!IS(o, lv_label_class)) return false;
        v = BACK(LONGS, lv_label_get_long_mode(o), 0);
        break;
    case T5UI_P_PLACEHOLDER:
        if (is_field(k)) {
            out->s = md_textfield_label(o);
            return true;
        }
        if (!ta) return false;
        out->s = lv_textarea_get_placeholder_text(ta);
        return true;
    case T5UI_P_HELPER: {
        if (!is_field(k)) return false;
        lv_obj_t *helper = lv_obj_get_child(o, -1);
        out->s = helper ? lv_label_get_text(helper) : "";
        return true;
    }
    case T5UI_P_HELPER_ERROR:
        if (!is_field(k)) return false;
        v = n && n->helper_error;
        break;
    case T5UI_P_IMAGE_SCALE:
        if (!IS(o, lv_image_class)) return false;
        v = (int32_t)lv_image_get_scale(o);
        break;
    case T5UI_P_IMAGE_ROTATION:
        if (!IS(o, lv_image_class)) return false;
        v = lv_image_get_rotation(o);
        break;
    case T5UI_P_HIDDEN: v = lv_obj_is_hidden(o); break;
    case T5UI_P_CLICKABLE: v = lv_obj_is_clickable(o); break;
    case T5UI_P_SCROLLABLE: v = lv_obj_is_scrollable(o); break;
    case T5UI_P_SCROLL_DIR: v = BACK(DIRS, lv_obj_get_scroll_dir(o), 0); break;
    case T5UI_P_SCROLLBAR: v = BACK(SCROLLBARS, lv_obj_get_scrollbar_mode(o), 0); break;
    case T5UI_P_SCROLL_X: v = lv_obj_get_scroll_x(o); break;
    case T5UI_P_SCROLL_Y: v = lv_obj_get_scroll_y(o); break;
    case T5UI_P_CHECKED: v = lv_obj_has_state(o, LV_STATE_CHECKED); break;
    case T5UI_P_DISABLED: v = lv_obj_has_state(o, LV_STATE_DISABLED); break;
    case T5UI_P_CHECKABLE: v = lv_obj_is_checkable(o); break;
    case T5UI_P_BUBBLE: v = lv_obj_is_event_bubble(o); break;
    case T5UI_P_SCROLL_SNAP_X: v = BACK(SNAPS, lv_obj_get_scroll_snap_x(o), 0); break;
    case T5UI_P_SCROLL_SNAP_Y: v = BACK(SNAPS, lv_obj_get_scroll_snap_y(o), 0); break;
    case T5UI_P_SCROLL_ONE: v = lv_obj_is_scroll_one(o); break;
    case T5UI_P_SCROLL_ELASTIC: v = lv_obj_is_scroll_elastic(o); break;
    case T5UI_P_SCROLL_CHAIN: v = lv_obj_is_scroll_chain_hor(o) || lv_obj_is_scroll_chain_ver(o); break;
    case T5UI_P_SNAPPABLE: v = lv_obj_is_snappable(o); break;
    case T5UI_P_EXT_CLICK: v = n ? n->ext_click : 0; break;
    case T5UI_P_STATE: {
        lv_state_t st = lv_obj_get_state(o);
        v = ((st & LV_STATE_CHECKED) ? T5UI_STATE_CHECKED : 0) | ((st & LV_STATE_FOCUSED) ? T5UI_STATE_FOCUSED : 0) |
            ((st & LV_STATE_PRESSED) ? T5UI_STATE_PRESSED : 0) | ((st & LV_STATE_DISABLED) ? T5UI_STATE_DISABLED : 0) |
            ((st & LV_STATE_EDITED) ? T5UI_STATE_EDITED : 0);
        break;
    }
    case T5UI_P_CURSOR:
        if (!ta) return false;
        v = (int32_t)lv_textarea_get_cursor_pos(ta);
        break;
    case T5UI_P_VALUE:
        if (IS(o, lv_bar_class)) v = lv_bar_get_value(o);
        else if (IS(o, lv_arc_class)) v = lv_arc_get_value(o);
        else if (IS(o, lv_led_class)) v = lv_led_get_brightness(o);
        else return false;
        break;
    case T5UI_P_MIN:
        if (IS(o, lv_bar_class)) v = lv_bar_get_min_value(o);
        else if (IS(o, lv_arc_class)) v = lv_arc_get_min_value(o);
        else if (IS(o, lv_chart_class) && n) v = n->ymin;
        else return false;
        break;
    case T5UI_P_MAX:
        if (IS(o, lv_bar_class)) v = lv_bar_get_max_value(o);
        else if (IS(o, lv_arc_class)) v = lv_arc_get_max_value(o);
        else if (IS(o, lv_chart_class) && n) v = n->ymax;
        else return false;
        break;
    case T5UI_P_START_VALUE:
        if (!IS(o, lv_bar_class)) return false;
        v = lv_bar_get_start_value(o);
        break;
    case T5UI_P_ARC_START:
        if (!IS(o, lv_arc_class)) return false;
        v = (int32_t)lv_arc_get_bg_angle_start(o);
        break;
    case T5UI_P_ARC_END:
        if (!IS(o, lv_arc_class)) return false;
        v = (int32_t)lv_arc_get_bg_angle_end(o);
        break;
    case T5UI_P_ARC_ROTATION:
        if (!IS(o, lv_arc_class)) return false;
        v = lv_arc_get_rotation(o);
        break;
    case T5UI_P_SELECTED:
        if (IS(o, lv_dropdown_class)) v = (int32_t)lv_dropdown_get_selected(o);
        else if (IS(o, lv_roller_class)) v = (int32_t)lv_roller_get_selected(o);
        else if (IS(o, lv_tabview_class)) v = (int32_t)lv_tabview_get_tab_active(o);
        else return false;
        break;
    case T5UI_P_OPTIONS:
        if (IS(o, lv_dropdown_class)) out->s = lv_dropdown_get_options(o);
        else if (IS(o, lv_roller_class)) out->s = lv_roller_get_options(o);
        else return false;
        return true;
    case T5UI_P_COUNT:
        if (k == T5UI_MD_LIST) v = (int32_t)md_list_count(o);
        else if (IS(o, lv_chart_class)) v = (int32_t)lv_chart_get_point_count(o);
        else return false;
        break;
    case T5UI_P_ROWS:
        if (!IS(o, lv_table_class)) return false;
        v = (int32_t)lv_table_get_row_count(o);
        break;
    case T5UI_P_COLUMNS:
        if (!IS(o, lv_table_class)) return false;
        v = (int32_t)lv_table_get_column_count(o);
        break;
    case T5UI_P_ONE_LINE:
        if (!ta) return false;
        v = lv_textarea_get_one_line(ta);
        break;
    case T5UI_P_PASSWORD:
        if (!ta) return false;
        v = lv_textarea_get_password_mode(ta);
        break;
    case T5UI_P_MAX_LENGTH:
        if (!ta) return false;
        v = (int32_t)lv_textarea_get_max_length(ta);
        break;
    case T5UI_P_CONTENT:
        out->o = H(k == T5UI_MD_PAGE ? md_page_body(o) : o);
        return true;
    case T5UI_P_TARGET: {
        if (!IS(o, lv_keyboard_class)) return false;
        lv_obj_t *t = lv_keyboard_get_textarea(o);
        /* A text field's text area: the field (frame, then the field). */
        lv_obj_t *field = t && lv_obj_get_parent(t) ? lv_obj_get_parent(lv_obj_get_parent(t)) : NULL;
        out->o = H(field && is_field(kind_of(field)) ? field : t);
        return true;
    }
    case T5UI_P_KEYBOARD_MODE:
        if (!IS(o, lv_keyboard_class)) return false;
        v = lv_keyboard_get_mode(o) == LV_KEYBOARD_MODE_NUMBER ? T5UI_KB_NUMBER : T5UI_KB_TEXT;
        break;
    case T5UI_P_CHART_TYPE:
        if (!IS(o, lv_chart_class)) return false;
        v = lv_chart_get_type(o) == LV_CHART_TYPE_BAR ? T5UI_CHART_BAR : T5UI_CHART_LINE;
        break;
    case T5UI_P_CHART_GRID_ROWS:
        if (!IS(o, lv_chart_class) || !n) return false;
        v = n->grid_rows;
        break;
    case T5UI_P_CHART_GRID_COLUMNS:
        if (!IS(o, lv_chart_class) || !n) return false;
        v = n->grid_cols;
        break;
    case T5UI_P_ANIM_TIME:
        if (!IS(o, lv_spinner_class)) return false;
        v = (int32_t)lv_spinner_get_anim_duration(o);
        break;
    default: return false;
    }
    out->i = v;
    return true;
}

static t5_err_t set_prop(lv_obj_t *o, t5ui_prop_t p, uint32_t at, const t5ui_val_t *v)
{
    if (t5ui_prop_info(p) & T5UI_F_LOOK) {
        dest_t d = { .obj = o, .sel = sel_of(at) };
        return look_set(&d, p, v);
    }
    node_t *n = node_of(o);
    t5ui_kind_t k = n ? (t5ui_kind_t)n->kind : 0;
    if (v->type == T5UI_T_STR && p != T5UI_P_PLACEHOLDER && p != T5UI_P_HELPER && p != T5UI_P_OPTIONS) {
        return set_text(o, k, p, v->s);
    }
    return set_widget(o, n, k, p, v);
}

t5_err_t t5ui_be_set(t5ui_obj_t *obj, t5ui_prop_t p, uint32_t at, const t5ui_val_t *v)
{
    return set_prop(O(obj), p, at, v);
}

bool t5ui_be_get(t5ui_obj_t *obj, t5ui_prop_t p, t5ui_val_t *v)
{
    lv_obj_t *o = O(obj);
    if (t5ui_prop_info(p) & T5UI_F_LOOK) return look_get(o, p, v);
    node_t *n = node_of(o);
    t5ui_kind_t k = n ? (t5ui_kind_t)n->kind : 0;
    if (v->type == T5UI_T_STR && p != T5UI_P_PLACEHOLDER && p != T5UI_P_HELPER && p != T5UI_P_OPTIONS) {
        v->s = get_text(o, k, p);
        return v->s != NULL;
    }
    return get_widget(o, n, k, p, v);
}

/* ---- styles -------------------------------------------------------------------------------- */
struct t5ui_style {
    lv_style_t s;
};

static void style_release(void *h)
{
    t5ui_style_t *s = h;
    lv_style_reset(&s->s);
    free(s);
}

t5ui_style_t *t5ui_style_create(void)
{
    t5ui_style_t *s = malloc(sizeof *s);
    if (!s) return NULL;
    lv_style_init(&s->s);
    t5_res_add(T5_RES_OTHER, s, style_release);   /* after the app's objects are gone */
    return s;
}

t5_err_t t5ui_be_style_set(t5ui_style_t *s, t5ui_prop_t p, const t5ui_val_t *v)
{
    dest_t d = { .style = &s->s };
    t5_err_t err = look_set(&d, p, v);
    if (err == T5_OK) lv_obj_report_style_change(&s->s);
    return err;
}

t5_err_t t5ui_be_style_add(t5ui_obj_t *obj, t5ui_style_t *s, uint32_t at)
{
    lv_obj_add_style(O(obj), &s->s, sel_of(at));
    return T5_OK;
}

void t5ui_style_remove(t5ui_obj_t *obj, t5ui_style_t *s)
{
    if (obj && s) lv_obj_remove_style(O(obj), &s->s, LV_PART_ANY | LV_STATE_ANY);
}

void t5ui_style_clear(t5ui_obj_t *obj)
{
    if (obj) lv_obj_remove_style_all(O(obj));
}

static lv_obj_tree_walk_res_t unstyle(lv_obj_t *o, void *s)
{
    lv_obj_remove_style(o, s, LV_PART_ANY | LV_STATE_ANY);
    return LV_OBJ_TREE_WALK_NEXT;
}

void t5ui_style_delete(t5ui_style_t *s)
{
    if (!s) return;
    t5_app_t *app = t5_app_ctx_get();
    lv_obj_t *root = app ? t5_app_screen(app) : lv_screen_active();
    if (root) lv_obj_tree_walk(root, unstyle, &s->s);
    t5_res_remove(s);
    style_release(s);
}

/* ---- animations ------------------------------------------------------------------------ */
static void anim_exec(void *var, int32_t v)
{
    anim_rec_t *r = var;
    set_prop(r->obj, r->prop, 0, &(t5ui_val_t){ .type = T5UI_T_INT, .i = v });
}

static void anim_done(lv_anim_t *a)
{
    anim_rec_t *r = a->var;
    item_t it = { NULL, (int32_t)r->prop };
    lv_obj_send_event(r->obj, (lv_event_code_t)s_code_anim_end, &it);
}

t5_err_t t5ui_be_animate(t5ui_obj_t *obj, t5ui_prop_t p, int32_t from, int32_t to, uint32_t ms, uint32_t delay_ms,
                         t5ui_ease_t ease)
{
    static const lv_anim_path_cb_t PATHS[] = { lv_anim_path_linear, lv_anim_path_ease_in, lv_anim_path_ease_out,
                                               lv_anim_path_ease_in_out, lv_anim_path_overshoot,
                                               lv_anim_path_bounce };
    codes_init();
    lv_obj_t *o = O(obj);
    /* Check it applies before animating it. */
    t5_err_t err = set_prop(o, p, 0, &(t5ui_val_t){ .type = T5UI_T_INT, .i = from });
    if (err != T5_OK) return err;
    node_t *n = node_need(o);
    if (!n) return T5_ERR_NO_MEM;
    anim_rec_t *r = n->anims;
    while (r && r->prop != p) r = r->next;
    if (!r) {
        r = malloc(sizeof *r);
        if (!r) return T5_ERR_NO_MEM;
        *r = (anim_rec_t){ n->anims, o, p };
        n->anims = r;
    }
    lv_anim_delete(r, NULL);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, r);
    lv_anim_set_exec_cb(&a, anim_exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_delay(&a, delay_ms);
    if (ease <= T5UI_EASE_BOUNCE) lv_anim_set_path_cb(&a, PATHS[ease]);
    else md_anim_set_curve(&a, ease == T5UI_EASE_STANDARD ? MD_CURVE_STANDARD
                               : ease == T5UI_EASE_DECELERATE ? MD_CURVE_DECELERATION : MD_CURVE_ACCELERATION);
    lv_anim_set_completed_cb(&a, anim_done);
    return lv_anim_start(&a) ? T5_OK : T5_ERR_NO_MEM;
}

void t5ui_animate_stop(t5ui_obj_t *obj, t5ui_prop_t p)
{
    node_t *n = obj ? node_of(O(obj)) : NULL;
    if (!n) return;
    for (anim_rec_t *r = n->anims; r; r = r->next) {
        if (!p || r->prop == p) lv_anim_delete(r, NULL);
    }
}

/* ---- commands ----------------------------------------------------------------------------- */
int32_t t5ui_be_cmd(t5ui_obj_t *obj, t5ui_cmd_t cmd, int32_t a, int32_t b, int32_t c, const char *text)
{
    lv_obj_t *o = O(obj);
    t5ui_kind_t k = kind_of(o);
    lv_obj_t *ta = textarea_of(o, k);
    switch (cmd) {
    case T5UI_CMD_CHART_ADD_SERIES: {
        if (!IS(o, lv_chart_class)) return T5_ERR_NOT_SUPPORTED;
        int32_t count = 0;
        for (lv_chart_series_t *s = lv_chart_get_series_next(o, NULL); s; s = lv_chart_get_series_next(o, s)) count++;
        if (count >= 8) return T5_ERR_NO_MEM;
        return lv_chart_add_series(o, col((t5ui_color_t)a), LV_CHART_AXIS_PRIMARY_Y) ? count : T5_ERR_NO_MEM;
    }
    case T5UI_CMD_CHART_PUSH:
    case T5UI_CMD_CHART_SET_ALL:
    case T5UI_CMD_CHART_COLOR:
    case T5UI_CMD_CHART_X_START:
    case T5UI_CMD_CHART_SET:
    case T5UI_CMD_CHART_CLEAR: {
        if (!IS(o, lv_chart_class)) return T5_ERR_NOT_SUPPORTED;
        lv_chart_series_t *s = series_at(o, a);
        if (!s) return T5_ERR_INVALID_ARG;
        if (cmd == T5UI_CMD_CHART_PUSH) {
            lv_chart_set_next_value(o, s, b);
        } else if (cmd == T5UI_CMD_CHART_SET_ALL) {
            lv_chart_set_all_values(o, s, b);
        } else if (cmd == T5UI_CMD_CHART_COLOR) {
            lv_chart_set_series_color(o, s, col((t5ui_color_t)b));
        } else if (cmd == T5UI_CMD_CHART_X_START) {
            if (b < 0 || (uint32_t)b >= lv_chart_get_point_count(o)) return T5_ERR_INVALID_ARG;
            lv_chart_set_x_start_point(o, s, (uint32_t)b);
        } else if (cmd == T5UI_CMD_CHART_SET) {
            if (b < 0 || (uint32_t)b >= lv_chart_get_point_count(o)) return T5_ERR_INVALID_ARG;
            lv_chart_set_series_value_by_id(o, s, (uint32_t)b, c);
            lv_chart_refresh(o);
        } else {
            lv_chart_set_all_values(o, s, LV_CHART_POINT_NONE);
        }
        lv_chart_refresh(o);
        return T5_OK;
    }
    case T5UI_CMD_TABLE_SET_CELL:
        if (!IS(o, lv_table_class)) return T5_ERR_NOT_SUPPORTED;
        if (a < 0 || b < 0) return T5_ERR_INVALID_ARG;
        lv_table_set_cell_value(o, (uint32_t)a, (uint32_t)b, text ? text : "");
        return T5_OK;
    case T5UI_CMD_TEXT_INSERT:
        if (!ta) return T5_ERR_NOT_SUPPORTED;
        lv_textarea_add_text(ta, text ? text : "");
        return T5_OK;
    case T5UI_CMD_TEXT_DELETE:
        if (!ta) return T5_ERR_NOT_SUPPORTED;
        lv_textarea_delete_char(ta);
        return T5_OK;
    case T5UI_CMD_LIST_REFRESH:
        if (k != T5UI_MD_LIST) return T5_ERR_NOT_SUPPORTED;
        md_list_refresh(o);
        return T5_OK;
    case T5UI_CMD_SCROLL_TO: lv_obj_scroll_to(o, a, b, anim_on(c)); return T5_OK;
    case T5UI_CMD_SCROLL_BY: lv_obj_scroll_by(o, a, b, anim_on(c)); return T5_OK;
    case T5UI_CMD_OPEN:
    case T5UI_CMD_CLOSE:
        if (!IS(o, lv_dropdown_class)) return T5_ERR_NOT_SUPPORTED;
        if (cmd == T5UI_CMD_OPEN) lv_dropdown_open(o);
        else lv_dropdown_close(o);
        return T5_OK;
    case T5UI_CMD_FOCUS: {
        lv_obj_t *f = ta ? ta : o;
        lv_group_t *g = lv_obj_get_group(f);
        if (!g) {
            g = lv_group_get_default();
            if (!g) return T5_ERR_NOT_SUPPORTED;
            lv_group_add_obj(g, f);
        }
        lv_group_focus_obj(f);
        lv_group_set_editing(g, a != 0);
        return T5_OK;
    }
    case T5UI_CMD_IGNORE_PRESS: {
        lv_indev_t *in = lv_indev_active();
        if (in) lv_indev_wait_release(in);
        return T5_OK;
    }
    case T5UI_CMD_DELETE_LATER: lv_obj_delete_async(o); return T5_OK;
    case T5UI_CMD_SEND: {
        if (a < T5UI_EV_CLICK || a > T5UI_EV_DRAW_BEHIND || a == T5UI_EV_DELETE || a == T5UI_EV_DRAW ||
            a == T5UI_EV_DRAW_BEHIND) {
            return T5_ERR_INVALID_ARG;
        }
        codes_init();
        lv_obj_t *to = is_field(k) && field_event((t5ui_ev_t)a) ? ta : o;
        lv_obj_send_event(to, (lv_event_code_t)(code_of((t5ui_ev_t)a) & ~LV_EVENT_PREPROCESS), NULL);
        return T5_OK;
    }
    default: return T5_ERR_NOT_SUPPORTED;
    }
}

/* ---- basics ----------------------------------------------------------------------------- */
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

t5_err_t t5ui_later(t5ui_fn_t fn, void *arg)
{
    if (!fn) return T5_ERR_INVALID_ARG;
    return lv_async_call(fn, arg) == LV_RESULT_OK ? T5_OK : T5_ERR_NO_MEM;
}

void t5ui_later_cancel(t5ui_fn_t fn, void *arg)
{
    if (fn) lv_async_call_cancel(fn, arg);
}

_Static_assert(sizeof(t5ui_touch_t) == sizeof(t5_ui_touch_t) && T5UI_TOUCH_MAX == T5_UI_TOUCH_MAX,
               "t5ui_touch_t is t5_ui_touch_t");

int t5ui_touches(t5ui_touch_t *out, int max)
{
    return out && max > 0 ? t5_ui_touch_get((t5_ui_touch_t *)out, max) : 0;
}

void t5ui_text_size(const char *text, t5ui_text_t style, int32_t max_w, int32_t *w, int32_t *h)
{
    lv_point_t p = { 0, 0 };
    lv_text_get_size(&p, text ? text : "", font(style), 0, 0, max_w > 0 ? max_w : LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (w) *w = p.x;
    if (h) *h = p.y;
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

t5ui_color_t t5ui_theme_surface(int32_t elevation_dp)
{
    return md_surface_at(md_scheme(), (int)elevation_dp);
}

_Static_assert(T5UI_ACCENT_COUNT == MD_ACCENT_COUNT && (int)T5UI_ACCENT_RED == (int)MD_ACCENT_RED,
               "t5ui accents are md's");

t5ui_color_t t5ui_accent_color(t5ui_accent_t a, t5ui_tone_t tone)
{
    if ((unsigned)a >= T5UI_ACCENT_COUNT) a = T5UI_ACCENT_PURPLE;
    const md_accent_def_t *d = md_accent((md_accent_t)a);
    switch (tone) {
    case T5UI_TONE_LIGHT_VARIANT: return d->light_variant;
    case T5UI_TONE_DARK: return d->dark;
    case T5UI_TONE_DARK_VARIANT: return d->dark_variant;
    case T5UI_TONE_ON_LIGHT: return d->on_light;
    case T5UI_TONE_ON_DARK: return d->on_dark;
    default: return d->light;
    }
}

const char *t5ui_accent_name(t5ui_accent_t a)
{
    return (unsigned)a < T5UI_ACCENT_COUNT ? md_accent((md_accent_t)a)->name : "";
}

t5ui_accent_t t5ui_theme_accent(void)
{
    return (t5ui_accent_t)md_get_accent();
}

/* ---- the OS's own (t5ui_sys.h) ------------------------------------------------------- */
t5ui_obj_t *t5ui_sys_layer(t5ui_layer_t layer)
{
    switch (layer) {
    case T5UI_LAYER_TOP: return H(lv_layer_top());
    case T5UI_LAYER_BOTTOM: return H(lv_layer_bottom());
    case T5UI_LAYER_SCREEN: return H(lv_screen_active());
    default: return NULL;
    }
}

void t5ui_sys_set_accent(t5ui_accent_t primary, t5ui_accent_t secondary)
{
    if ((unsigned)primary < T5UI_ACCENT_COUNT && (unsigned)secondary < T5UI_ACCENT_COUNT) {
        md_set_accent((md_accent_t)primary, (md_accent_t)secondary);
    }
}

t5_err_t t5ui_sys_snapshot(t5ui_obj_t *obj, t5ui_pixels_t *out)
{
    if (!obj || !out) return T5_ERR_INVALID_ARG;
    lv_draw_buf_t *b = lv_snapshot_take(O(obj), LV_COLOR_FORMAT_RGB565);
    if (!b) return T5_ERR_NO_MEM;
    uint32_t w = b->header.w, h = b->header.h, row = w * 2;
    uint8_t *px = malloc((size_t)row * h);
    if (!px) {
        lv_draw_buf_destroy(b);
        return T5_ERR_NO_MEM;
    }
    for (uint32_t y = 0; y < h; y++) memcpy(px + y * row, b->data + y * b->header.stride, row);
    lv_draw_buf_destroy(b);
    *out = (t5ui_pixels_t){ .size = sizeof *out, .w = (uint16_t)w, .h = (uint16_t)h, .stride = row,
                            .format = T5UI_PX_RGB565, .data = px };
    return T5_OK;
}

void t5ui_sys_snapshot_free(t5ui_pixels_t *px)
{
    if (!px) return;
    free((void *)px->data);
    px->data = NULL;
}

t5ui_obj_t *t5ui_sys_obj(void *native)
{
    return H(native);
}

void *t5ui_sys_native(t5ui_obj_t *obj)
{
    return O(obj);
}

/* ---- timers -------------------------------------------------------------------------- */
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

void t5ui_timer_reset(t5ui_timer_t *t)
{
    if (t) lv_timer_reset(t->t);
}

void t5ui_timer_delete(t5ui_timer_t *t)
{
    if (!t) return;
    t5_res_remove(t);
    lv_timer_delete(t->t);
    free(t);
}

/* ---- dialogs and messages ---------------------------------------------------------------- */
_Static_assert(sizeof(t5ui_answer_cb_t) == sizeof(md_dialog_cb_t), "answer callbacks match md's");

t5ui_obj_t *t5ui_dialog(const char *title, const char *text, const char *confirm, const char *dismiss,
                        t5ui_answer_cb_t cb, void *user)
{
    return H(md_dialog_show(title, text, confirm, dismiss, (md_dialog_cb_t)cb, user));
}

void t5ui_dialog_close(t5ui_obj_t *dialog)
{
    if (dialog) md_dialog_close(O(dialog));
}

t5ui_obj_t *t5ui_menu(t5ui_obj_t *anchor, const char *const *items, int n, t5ui_answer_cb_t cb, void *user)
{
    if (!anchor || !items || n <= 0) return NULL;
    return H(md_menu_show(O(anchor), items, n, (md_menu_cb_t)cb, user));
}

void t5ui_snackbar(const char *text, const char *action, t5ui_action_cb_t cb, void *user)
{
    md_snackbar_show(text ? text : "", action, (md_action_cb_t)cb, user);
}

/* ---- images ------------------------------------------------------------------------------- */
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

/* A copy of an app's pixels as an LVGL draw buffer. */
static lv_draw_buf_t *buf_from(const t5ui_pixels_t *px)
{
    uint8_t bpp;
    lv_color_format_t cf = px_format(px->format, &bpp);
    if (!bpp) return NULL;
    uint32_t stride = px->stride ? px->stride : (uint32_t)px->w * bpp;
    if (stride < (uint32_t)px->w * bpp) return NULL;
    lv_draw_buf_t *b = lv_draw_buf_create(px->w, px->h, cf, LV_STRIDE_AUTO);
    if (!b) return NULL;
    for (uint32_t y = 0; y < px->h; y++) {
        memcpy(b->data + y * b->header.stride, (const uint8_t *)px->data + y * stride, (size_t)px->w * bpp);
    }
    return b;
}

/* Show b on o in place of the buffer it showed before. */
static void swap_buf(node_t *n, lv_draw_buf_t *b)
{
    lv_draw_buf_t *old = n->buf;
    n->buf = b;
    if (old) {
        lv_image_cache_drop(old);
        lv_draw_buf_destroy(old);
    }
}

t5_err_t t5ui_be_image_pixels(t5ui_obj_t *img, const t5ui_pixels_t *px)
{
    lv_obj_t *o = O(img);
    if (!IS(o, lv_image_class) || IS(o, lv_canvas_class)) return T5_ERR_NOT_SUPPORTED;
    node_t *n = node_need(o);
    if (!n) return T5_ERR_NO_MEM;
    lv_draw_buf_t *b = buf_from(px);
    if (!b) return T5_ERR_NO_MEM;
    lv_image_set_src(o, b);
    swap_buf(n, b);
    return T5_OK;
}

/* ---- canvases and drawing ---------------------------------------------------------------- */
t5_err_t t5ui_canvas_size(t5ui_obj_t *canvas, int32_t w, int32_t h)
{
    lv_obj_t *c = O(canvas);
    if (!c || w <= 0 || h <= 0) return T5_ERR_INVALID_ARG;
    if (!IS(c, lv_canvas_class)) return T5_ERR_NOT_SUPPORTED;
    node_t *n = node_need(c);
    if (!n) return T5_ERR_NO_MEM;
    lv_draw_buf_t *b = lv_draw_buf_create((uint32_t)w, (uint32_t)h, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
    if (!b) return T5_ERR_NO_MEM;
    lv_canvas_set_draw_buf(c, b);
    lv_canvas_fill_bg(c, lv_color_black(), LV_OPA_TRANSP);
    swap_buf(n, b);
    return T5_OK;
}

static bool canvas_ready(lv_obj_t *c)
{
    return c && IS(c, lv_canvas_class) && lv_canvas_get_draw_buf(c);
}

void t5ui_canvas_fill(t5ui_obj_t *canvas, t5ui_color_t color, t5ui_opa_t opa)
{
    if (canvas_ready(O(canvas))) lv_canvas_fill_bg(O(canvas), col(color), opa);
}

t5ui_paint_t *t5ui_canvas_begin(t5ui_obj_t *canvas)
{
    if (!canvas_ready(O(canvas))) return NULL;
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
    if (canvas) lv_canvas_finish_layer(O(canvas), p->layer);
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
    d.align = IN(align, TALIGNS) ? TALIGNS[align] : LV_TEXT_ALIGN_LEFT;
    d.text_local = 1;                          /* copied: the app's string may go */
    lv_area_t a = area_of(p, r);
    lv_draw_label(p->layer, &d, &a);
}

static void buf_destroy_later(void *b)
{
    lv_draw_buf_destroy(b);
}

void t5ui_draw_pixels(t5ui_paint_t *p, int32_t x, int32_t y, const t5ui_pixels_t *px)
{
    if (!p || !px || px->size < sizeof *px || !px->w || !px->h || !px->data) return;
    lv_draw_buf_t *b = buf_from(px);           /* the app's buffer may go before the layer renders */
    if (!b) return;
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = b;
    lv_area_t a = { x + p->ox, y + p->oy, x + p->ox + px->w - 1, y + p->oy + px->h - 1 };
    lv_draw_image(p->layer, &d, &a);
    /* The draw task reads the copy while the layer renders: within this
     * frame for a draw event, in t5ui_canvas_end for a canvas. Async calls
     * run after both. */
    lv_async_call(buf_destroy_later, b);
}
