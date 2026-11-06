/* SPDX-License-Identifier: Apache-2.0
 * Base MD2 widgets (design "Components"): buttons, FAB, card, top app bar,
 * a recycled list and a modal dialog. Enough for the rendering gate (4.8);
 * Phase 6.1 adds the rest of the t5_md_* set on the same patterns.
 */
#include <stdlib.h>
#include <string.h>
#include "md/md.h"
#include "md/md_icons.h"
#include "md_priv.h"

/* ---- buttons ---------------------------------------------------------------- */
lv_obj_t *md_button_create(lv_obj_t *parent, const char *text, md_button_variant_t v)
{
    lv_obj_t *b = lv_button_create(parent);    /* theme: contained, ripple */
    if (v != MD_BUTTON_CONTAINED) {
        lv_obj_add_style(b, md_style_btn_text(), 0);
        if (v == MD_BUTTON_OUTLINED) lv_obj_add_style(b, md_style_btn_outlined(), 0);
    }
    lv_obj_set_height(b, T5_DP(MD_BUTTON_H_DP));
    lv_obj_set_ext_click_area(b, T5_DP(MD_TOUCH_DP - MD_BUTTON_H_DP) / 2);
    lv_obj_t *l = lv_label_create(b);
    md_label_set_text(l, text, MD_TYPE_BUTTON);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *icon_button(lv_obj_t *parent, const char *icon)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_add_style(b, md_style_btn_text(), 0);
    lv_obj_set_size(b, T5_DP(MD_TOUCH_DP), T5_DP(MD_TOUCH_DP));
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_min_width(b, 0, 0);
    lv_obj_set_style_text_color(b, md_color(md_scheme()->c.on_surface), 0);
    lv_obj_t *l = md_icon_create(b, icon, MD_ICON_DP);
    lv_obj_center(l);
    return b;
}

lv_obj_t *md_fab_create(lv_obj_t *parent, const char *icon, bool mini)
{
    const md_scheme_t *sc = md_scheme();
    int d = mini ? MD_FAB_MINI_DP : MD_FAB_DP;
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, T5_DP(d), T5_DP(d));
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_min_width(b, 0, 0);
    md_set_elevation(b, MD_ELEV_FAB, true);
    lv_obj_set_style_bg_color(b, md_color(sc->c.secondary), 0);
    lv_obj_set_style_text_color(b, md_color(sc->c.on_secondary), 0);
    lv_obj_t *l = md_icon_create(b, icon, MD_ICON_DP);
    lv_obj_center(l);
    return b;
}

lv_obj_t *md_card_create(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    md_set_elevation(c, MD_ELEV_CARD, false);
    lv_obj_set_style_radius(c, T5_DP(MD_SHAPE_MEDIUM_DP), 0);
    lv_obj_set_style_pad_all(c, T5_DP(16), 0);
    lv_obj_set_style_pad_gap(c, T5_DP(8), 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(c, false);
    return c;
}

/* ---- top app bar -------------------------------------------------------------- */
typedef struct {
    lv_obj_t *title;
    lv_obj_t *actions;
} appbar_t;

static void free_ud(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

lv_obj_t *md_appbar_create(lv_obj_t *parent, const char *title, const char *nav_icon, lv_event_cb_t nav_cb,
                           void *user_data)
{
    appbar_t *ab = calloc(1, sizeof *ab);
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_user_data(bar, ab);
    lv_obj_add_event_cb(bar, free_ud, LV_EVENT_DELETE, ab);
    /* Square and full width: stays on the PPA fill path. Dark app bars use
     * the 4 dp surface rather than the primary colour (MD2 dark theme). */
    md_set_elevation(bar, MD_ELEV_APP_BAR, false);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_size(bar, LV_PCT(100), T5_DP(MD_APP_BAR_DENSE_DP));
    lv_obj_set_style_pad_hor(bar, T5_DP(4), 0);
    lv_obj_set_style_pad_ver(bar, 0, 0);
    lv_obj_set_style_pad_column(bar, T5_DP(4), 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(bar, false);
    if (nav_icon) {
        lv_obj_t *nav = icon_button(bar, nav_icon);
        if (nav_cb) lv_obj_add_event_cb(nav, nav_cb, LV_EVENT_CLICKED, user_data);
    } else {
        lv_obj_set_style_pad_left(bar, T5_DP(16), 0);
    }
    ab->title = md_label_create(bar, title, MD_TYPE_HEADLINE6);
    lv_obj_set_style_pad_left(ab->title, nav_icon ? T5_DP(16) : 0, 0);
    lv_obj_set_flex_grow(ab->title, 1);
    lv_label_set_long_mode(ab->title, LV_LABEL_LONG_MODE_DOTS);
    return bar;
}

lv_obj_t *md_appbar_add_action(lv_obj_t *bar, const char *icon, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *b = icon_button(bar, icon);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user_data);
    return b;
}

void md_appbar_set_title(lv_obj_t *bar, const char *title)
{
    appbar_t *ab = lv_obj_get_user_data(bar);
    if (ab) lv_label_set_text(ab->title, title);
}

/* ---- recycled list ------------------------------------------------------------ */
#define LIST_EXTRA_ROWS 3

typedef struct {
    md_list_row_t rows[64];
    uint32_t bound[64];          /* index each row shows, UINT32_MAX = none */
    uint32_t nrows;
    uint32_t count;
    int32_t row_h;
    md_list_bind_cb_t bind;
    md_list_click_cb_t click;
    void *ud;
    lv_obj_t *spacer;
    bool two_line;
} list_t;

static void row_clicked(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target_obj(e);
    lv_obj_t *list = lv_obj_get_parent(row);
    list_t *l = lv_obj_get_user_data(list);
    for (uint32_t i = 0; i < l->nrows; i++) {
        if (l->rows[i].row == row && l->bound[i] != UINT32_MAX && l->click) {
            l->click(l->bound[i], l->ud);
        }
    }
}

static void make_row(lv_obj_t *list, list_t *l, uint32_t i)
{
    md_list_row_t *r = &l->rows[i];
    r->row = lv_obj_create(list);
    lv_obj_t *row = r->row;
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), l->row_h);
    lv_obj_set_clickable(row, true);
    lv_obj_set_ignore_layout(row, true);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_scroll_on_focus(row, true);
    lv_obj_set_style_text_color(row, md_color(md_scheme()->c.on_surface), 0);
    lv_obj_set_style_pad_hor(row, T5_DP(16), 0);
    md_ripple_attach(row);
    lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED, NULL);

    r->icon = md_icon_create(row, MD_ICON_INFO, MD_ICON_DP);
    lv_obj_set_style_text_opa(r->icon, md_scheme()->on_surface.medium, 0);
    lv_obj_align(r->icon, LV_ALIGN_LEFT_MID, 0, 0);
    int32_t text_x = T5_DP(56);   /* MD2 list: text starts at 72 dp with a 16 dp margin */
    r->primary = md_label_create(row, "", MD_TYPE_SUBTITLE1);
    lv_label_set_long_mode(r->primary, LV_LABEL_LONG_MODE_CLIP);
    if (l->two_line) {
        r->secondary = md_label_create(row, "", MD_TYPE_BODY2);
        lv_obj_set_style_text_opa(r->secondary, md_scheme()->on_surface.medium, 0);
        lv_obj_align(r->primary, LV_ALIGN_TOP_LEFT, text_x, l->row_h / 2 - T5_DP(22));
        lv_obj_align(r->secondary, LV_ALIGN_TOP_LEFT, text_x, l->row_h / 2 + T5_DP(2));
    } else {
        r->secondary = NULL;
        lv_obj_align(r->primary, LV_ALIGN_LEFT_MID, text_x, 0);
    }
    l->bound[i] = UINT32_MAX;
}

static void list_layout(lv_obj_t *list, bool force)
{
    list_t *l = lv_obj_get_user_data(list);
    if (!l || !l->row_h) return;
    int32_t sy = lv_obj_get_scroll_y(list);
    if (sy < 0) sy = 0;
    uint32_t first = (uint32_t)(sy / l->row_h);
    /* Row object k shows index first + ((k - first) mod n): rows keep their
     * index while it stays visible, so scrolling rebinds only the row that
     * wrapped around. */
    for (uint32_t k = 0; k < l->nrows; k++) {
        uint32_t idx = first + ((k + l->nrows - (first % l->nrows)) % l->nrows);
        md_list_row_t *r = &l->rows[k];
        if (idx >= l->count) {
            lv_obj_set_hidden(r->row, true);
            l->bound[k] = UINT32_MAX;
            continue;
        }
        if (force || l->bound[k] != idx) {
            lv_obj_set_hidden(r->row, false);
            lv_obj_set_y(r->row, (int32_t)idx * l->row_h);
            l->bound[k] = idx;
            if (l->bind) l->bind(r, idx, l->ud);
        }
    }
}

static void list_event(lv_event_t *e)
{
    lv_obj_t *list = lv_event_get_target_obj(e);
    list_t *l = lv_obj_get_user_data(list);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_SCROLL:
        list_layout(list, false);
        break;
    case LV_EVENT_SIZE_CHANGED: {
        /* Enough rows to cover the height plus a margin for fast flings. */
        int32_t h = lv_obj_get_height(list);
        uint32_t need = (uint32_t)(h / l->row_h) + 1 + LIST_EXTRA_ROWS;
        if (need > 64) need = 64;
        while (l->nrows < need) make_row(list, l, l->nrows++);
        list_layout(list, true);
        break;
    }
    case LV_EVENT_DELETE:
        free(l);
        break;
    default:
        break;
    }
}

static void set_spacer(lv_obj_t *list, list_t *l)
{
    lv_obj_set_y(l->spacer, l->count ? (int32_t)l->count * l->row_h - 1 : 0); /* px: last pixel row */
}

lv_obj_t *md_list_create(lv_obj_t *parent, uint32_t count, int row_dp, md_list_bind_cb_t bind,
                         md_list_click_cb_t click, void *user_data)
{
    list_t *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_set_user_data(list, l);
    /* Square, flat, opaque: the list background stays a PPA fill. */
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_bg_color(list, md_color(md_scheme()->c.surface), 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);
    l->count = count;
    l->row_h = T5_DP(row_dp);
    l->bind = bind;
    l->click = click;
    l->ud = user_data;
    l->two_line = row_dp >= MD_LIST_ROW_2_DP;
    l->spacer = lv_obj_create(list);
    lv_obj_remove_style_all(l->spacer);
    lv_obj_set_size(l->spacer, 1, 1); /* px: invisible extent marker */
    lv_obj_set_ignore_layout(l->spacer, true);
    set_spacer(list, l);
    lv_obj_add_event_cb(list, list_event, LV_EVENT_ALL, NULL);
    return list;
}

void md_list_set_count(lv_obj_t *list, uint32_t count)
{
    list_t *l = lv_obj_get_user_data(list);
    l->count = count;
    set_spacer(list, l);
    list_layout(list, true);
}

void md_list_refresh(lv_obj_t *list)
{
    list_layout(list, true);
}

uint32_t md_list_rows_alive(lv_obj_t *list)
{
    list_t *l = lv_obj_get_user_data(list);
    return l ? l->nrows : 0;
}

/* ---- dialog ---------------------------------------------------------------------- */
typedef struct {
    lv_obj_t *scrim, *box;
    md_dialog_cb_t cb;
    void *ud;
    bool closing;
} dialog_t;

static void set_opa_anim(void *obj, int32_t v)
{
    lv_obj_set_style_opa(obj, (lv_opa_t)v, 0);
}

static void set_bg_opa_anim(void *obj, int32_t v)
{
    lv_obj_set_style_bg_opa(obj, (lv_opa_t)v, 0);
}

static void dialog_deleted(lv_anim_t *a)
{
    lv_obj_delete(a->var);
}

static void dialog_free(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

void md_dialog_close(lv_obj_t *scrim)
{
    dialog_t *d = lv_obj_get_user_data(scrim);
    if (!d || d->closing) return;
    d->closing = true;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, d->box);
    lv_anim_set_exec_cb(&a, set_opa_anim);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, MD_DUR_DIALOG_OUT_MS);
    md_anim_set_curve(&a, MD_CURVE_ACCELERATION);
    lv_anim_start(&a);
    lv_anim_init(&a);
    lv_anim_set_var(&a, scrim);
    lv_anim_set_exec_cb(&a, set_bg_opa_anim);
    lv_anim_set_values(&a, lv_obj_get_style_bg_opa(scrim, 0), LV_OPA_TRANSP);
    lv_anim_set_duration(&a, MD_DUR_DIALOG_OUT_MS);
    lv_anim_set_completed_cb(&a, dialog_deleted);
    lv_anim_start(&a);
}

static void dialog_button(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target_obj(e);
    lv_obj_t *scrim = lv_event_get_user_data(e);
    dialog_t *d = lv_obj_get_user_data(scrim);
    if (d->closing) return;
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (d->cb) d->cb(idx, d->ud);
    md_dialog_close(scrim);
}

static void scrim_clicked(lv_event_t *e)
{
    lv_obj_t *scrim = lv_event_get_current_target_obj(e);
    if (lv_event_get_target_obj(e) != scrim) return;   /* a tap inside the dialog */
    dialog_t *d = lv_obj_get_user_data(scrim);
    if (d->closing) return;
    if (d->cb) d->cb(-1, d->ud);
    md_dialog_close(scrim);
}

lv_obj_t *md_dialog_show(const char *title, const char *text, const char *btn_confirm, const char *btn_dismiss,
                         md_dialog_cb_t cb, void *user_data)
{
    const md_scheme_t *sc = md_scheme();
    dialog_t *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->cb = cb;
    d->ud = user_data;

    lv_obj_t *scrim = lv_obj_create(lv_layer_top());
    d->scrim = scrim;
    lv_obj_set_user_data(scrim, d);
    lv_obj_add_event_cb(scrim, dialog_free, LV_EVENT_DELETE, d);
    lv_obj_remove_style_all(scrim);
    lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(scrim, md_color(sc->c.scrim), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(scrim, true);
    lv_obj_add_event_cb(scrim, scrim_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *box = lv_obj_create(scrim);
    d->box = box;
    md_set_elevation(box, MD_ELEV_DIALOG, true);
    lv_obj_set_style_radius(box, T5_DP(MD_SHAPE_MEDIUM_DP), 0);
    lv_obj_set_style_pad_all(box, T5_DP(24), 0);
    lv_obj_set_style_pad_bottom(box, T5_DP(8), 0);
    lv_obj_set_style_pad_row(box, T5_DP(20), 0);
    lv_obj_set_width(box, LV_PCT(80));
    lv_obj_set_style_min_width(box, T5_DP(MD_DIALOG_MIN_W_DP), 0);
    lv_obj_set_style_max_width(box, T5_DP(MD_DIALOG_MAX_W_DP), 0);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(box, false);
    lv_obj_center(box);

    if (title) md_label_create(box, title, MD_TYPE_HEADLINE6);
    if (text) {
        lv_obj_t *t = md_label_create(box, text, MD_TYPE_BODY1);
        lv_obj_set_style_text_opa(t, sc->on_surface.medium, 0);
        lv_obj_set_width(t, LV_PCT(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    }
    lv_obj_t *actions = lv_obj_create(box);
    lv_obj_remove_style_all(actions);
    lv_obj_set_size(actions, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(actions, T5_DP(8), 0);
    lv_obj_set_style_pad_bottom(actions, 0, 0);
    if (btn_dismiss) {
        lv_obj_t *b = md_button_create(actions, btn_dismiss, MD_BUTTON_TEXT);
        lv_obj_set_user_data(b, (void *)(intptr_t)1);
        lv_obj_add_event_cb(b, dialog_button, LV_EVENT_CLICKED, scrim);
    }
    if (btn_confirm) {
        lv_obj_t *b = md_button_create(actions, btn_confirm, MD_BUTTON_TEXT);
        lv_obj_set_user_data(b, (void *)(intptr_t)0);
        lv_obj_add_event_cb(b, dialog_button, LV_EVENT_CLICKED, scrim);
    }

    /* Fade the scrim (a plain fill) and the dialog; never animate the shadow's size. */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, scrim);
    lv_anim_set_exec_cb(&a, set_bg_opa_anim);
    lv_anim_set_values(&a, LV_OPA_TRANSP, 82);   /* MD2 scrim: 32% */
    lv_anim_set_duration(&a, MD_DUR_DIALOG_IN_MS);
    md_anim_set_curve(&a, MD_CURVE_DECELERATION);
    lv_anim_start(&a);
    lv_obj_set_style_opa(box, LV_OPA_TRANSP, 0);
    lv_anim_init(&a);
    lv_anim_set_var(&a, box);
    lv_anim_set_exec_cb(&a, set_opa_anim);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, MD_DUR_DIALOG_IN_MS);
    md_anim_set_curve(&a, MD_CURVE_DECELERATION);
    lv_anim_start(&a);
    return scrim;
}
