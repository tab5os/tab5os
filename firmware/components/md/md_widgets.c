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
    md_list_click_cb_t long_press;
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

static void row_long_pressed(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target_obj(e);
    list_t *l = lv_obj_get_user_data(lv_obj_get_parent(row));
    if (!l->long_press) return;
    for (uint32_t i = 0; i < l->nrows; i++) {
        if (l->rows[i].row == row && l->bound[i] != UINT32_MAX) {
            lv_indev_wait_release(lv_indev_active());   /* no click after the long press */
            l->long_press(l->bound[i], l->ud);
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
    lv_obj_add_event_cb(row, row_long_pressed, LV_EVENT_LONG_PRESSED, NULL);

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
#define MD_DIALOG_SCRIM_OPA 82   /* MD2 scrim: 32% */
/* While the dialog fades in or out, nothing under it changes but the scrim
 * covers the whole screen, so every frame would redraw the screen and blend
 * the scrim over it in software. Instead the screen behind is frozen to a
 * snapshot (md_freeze.c), and the scrim is applied to the snapshot as it is
 * drawn (md_freeze_dim: a fill and a PPA blit); the real scrim stays clear
 * until the screen goes live again at the end. Optionally the dialog is
 * drawn from its own snapshot too (MD_FREEZE_DIALOG_BOX; not worth it). */
typedef struct {
    lv_obj_t *scrim, *box, *backdrop;
    lv_draw_buf_t *box_snap;
    bool baked;                  /* the scrim and shadow are in the backdrop's pixels */
    int shadow_dp;               /* the box's shadow, while it's baked */
    lv_timer_t *settle;          /* waiting to start the fade (open_when_settled) */
    bool deleting;
    bool waiting_frame, prepare_queued;   /* dialog_first_frame, dialog_prepare pending */
    uint32_t settle_ticks;
    lv_opa_t box_opa;
    lv_opa_t scrim_opa;          /* the scrim's opacity, wherever it is drawn */
    md_dialog_cb_t cb;
    void *ud;
    bool closing;
} dialog_t;

/* Where the box's snapshot goes: its coordinates grown by the shadow. */
static void box_area(dialog_t *d, lv_area_t *a)
{
    lv_obj_get_coords(d->box, a);
    int32_t ext = ((int32_t)d->box_snap->header.w - lv_area_get_width(a)) / 2;
    lv_area_increase(a, ext, ext);
    a->x2 = a->x1 + (int32_t)d->box_snap->header.w - 1;
    a->y2 = a->y1 + (int32_t)d->box_snap->header.h - 1;
}

/* Snapshot the dialog and the screen behind it for an animation. Without
 * the memory for it, the box's own opacity is animated instead. */
static void scrim_set(dialog_t *d, lv_opa_t v)
{
    d->scrim_opa = v;
    /* LVGL redraws an object on any style set, changed or not, and the
     * scrim covers the screen: only touch it when the value changes. */
    lv_opa_t want = v;
    if (d->backdrop) {
        if (!d->baked) md_freeze_dim(d->backdrop, lv_obj_get_style_bg_color(d->scrim, 0), v);
        want = LV_OPA_TRANSP;
    }
    if (lv_obj_get_style_bg_opa(d->scrim, 0) != want) lv_obj_set_style_bg_opa(d->scrim, want, 0);
}

/* Into the frozen backdrop, once: the scrim (but not under the objects
 * below the dialog), then the box's shadow, as the live dialog draws them. */
static void bake_draw(lv_layer_t *layer, void *arg)
{
    dialog_t *d = arg;
    md_scrim_draw(layer, d->scrim, d->scrim_opa, true);
    md_shadow_draw(layer, d->box, d->shadow_dp);
}

static void backdrop_thawed(lv_obj_t *scr, void *arg);

static void dialog_freeze(dialog_t *d)
{
    uint32_t mode = md_freeze_mode();
    uint32_t t0 = lv_tick_get();
    lv_obj_update_layout(d->scrim);
    if ((mode & MD_FREEZE_DIALOG_BACKDROP) && !md_screens_frozen() && md_freeze_screen(lv_screen_active()))
        d->backdrop = lv_screen_active();
    /* The scrim and the shadow don't change while the box fades: draw them
     * into the backdrop once. Each frame is then an opaque blit of the
     * backdrop plus the box itself, over the box's own area only (without
     * its shadow the box has no extra draw area). */
    if (d->backdrop) {
        d->baked = md_freeze_draw_into(d->backdrop, bake_draw, d);
        md_freeze_set_thaw_cb(d->backdrop, backdrop_thawed, d);
    }
    uint32_t box_ms = 0;
    if (!d->baked && (mode & MD_FREEZE_DIALOG_BOX)) {
        uint32_t t1 = lv_tick_get();
        d->box_snap = lv_snapshot_take(d->box, LV_COLOR_FORMAT_ARGB8888);
        if (d->box_snap) lv_obj_set_hidden(d->box, true);
        box_ms = lv_tick_elaps(t1);
    }
    md_freeze_note_dialog(lv_tick_elaps(t0), box_ms, 0);
    scrim_set(d, d->scrim_opa);
}

/* The backdrop went live, by our thaw or someone else's: the scrim and
 * the shadow go back on their objects. */
static void backdrop_thawed(lv_obj_t *scr, void *arg)
{
    (void)scr;
    dialog_t *d = arg;
    d->backdrop = NULL;
    d->baked = false;   /* the scrim draws the shadow again (scrim_draw) */
    if (!d->deleting) lv_obj_set_style_bg_opa(d->scrim, d->scrim_opa, 0);
}

/* live: put the scrim back on the scrim object (not while it is deleted). */
static void dialog_thaw_ex(dialog_t *d, bool live)
{
    if (d->box_snap) {
        lv_obj_set_hidden(d->box, false);
        lv_draw_buf_destroy(d->box_snap);
        d->box_snap = NULL;
    }
    if (d->backdrop) {
        d->deleting = !live;
        md_thaw_screen(d->backdrop);   /* backdrop_thawed puts the scrim back */
    }
}

static void dialog_thaw(dialog_t *d)
{
    dialog_thaw_ex(d, true);
}

static void box_opa_anim(void *obj, int32_t v)
{
    lv_obj_t *box = obj;
    dialog_t *d = lv_obj_get_user_data(lv_obj_get_parent(box));
    if (!d || !d->box_snap) {
        lv_obj_set_style_opa(box, (lv_opa_t)v, 0);
        return;
    }
    d->box_opa = (lv_opa_t)v;
    lv_area_t a;
    box_area(d, &a);
    lv_obj_invalidate_area(d->scrim, &a);
}

static void scrim_draw(lv_event_t *e)
{
    dialog_t *d = lv_event_get_user_data(e);
    /* The box's shadow belongs to the scrim while the dialog shows: drawn
     * at full strength under the box (the scrim's children come after this
     * event), it doesn't fade with the box, so the box's opacity changes
     * redraw only the box. Baked into the backdrop instead when frozen. */
    lv_layer_t *layer = lv_event_get_layer(e);
    if (d->baked) {
        md_scrim_draw(layer, d->scrim, d->scrim_opa, false);   /* over the objects below, live */
    } else if (d->shadow_dp) {
        md_shadow_draw(layer, d->box, d->shadow_dp);
    }
    if (!d->box_snap || d->box_opa <= LV_OPA_MIN) return;
    lv_area_t a;
    box_area(d, &a);
    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.src = d->box_snap;
    dsc.opa = d->box_opa;
    lv_draw_image(lv_event_get_layer(e), &dsc, &a);
}

static void scrim_opa_anim(void *obj, int32_t v)
{
    dialog_t *d = lv_obj_get_user_data(obj);
    if (d) scrim_set(d, (lv_opa_t)v);
}

/* The animation that finishes last ends the dialog's open or close: the
 * scrim's, or the box's when the scrim doesn't animate. Either way the
 * scrim is the dialog. */
static lv_obj_t *anim_scrim(lv_anim_t *a)
{
    lv_obj_t *o = a->var;   /* the scrim lives on the top layer; the box in the scrim */
    return lv_obj_get_parent(o) == lv_display_get_layer_top(lv_obj_get_display(o)) ? o : lv_obj_get_parent(o);
}

/* Open: the backdrop stays frozen. The dialog is modal and the screen
 * behind is dimmed, and thawing would redraw the whole screen for the same
 * pixels (and again at close). Taps on the dialog then redraw only its
 * area over the snapshot. Without a snapshot there's nothing to do. */
static void dialog_opened(lv_anim_t *a)
{
    dialog_t *d = lv_obj_get_user_data(anim_scrim(a));
    if (d && d->box_snap) {   /* the box itself again, so its buttons work */
        lv_obj_set_hidden(d->box, false);
        lv_obj_set_style_opa(d->box, LV_OPA_COVER, 0);
        lv_draw_buf_destroy(d->box_snap);
        d->box_snap = NULL;
    }
}

static void dialog_deleted(lv_anim_t *a)
{
    lv_obj_delete(anim_scrim(a));
}

static void stop_settle(dialog_t *d)
{
    if (d->settle) {
        lv_timer_delete(d->settle);
        d->settle = NULL;
    }
}

static void dialog_anim(dialog_t *d, lv_opa_t box_from, lv_opa_t box_to, lv_opa_t scrim_from, lv_opa_t scrim_to,
                        uint32_t ms, md_curve_t curve, lv_anim_completed_cb_t done);

/* Waits for the scrim's frame to reach every framebuffer (at most ~120 ms),
 * then starts the box's fade. */
static void open_when_settled(lv_timer_t *t)
{
    dialog_t *d = lv_timer_get_user_data(t);
    if (!md_freeze_settled() && ++d->settle_ticks < 30) return;
    stop_settle(d);
    dialog_anim(d, LV_OPA_TRANSP, LV_OPA_COVER, MD_DIALOG_SCRIM_OPA, MD_DIALOG_SCRIM_OPA, MD_DUR_DIALOG_IN_MS,
                MD_CURVE_DECELERATION, dialog_opened);
}

/* The display rotated (or resized) under an open dialog: the snapshot no
 * longer fits. Back to drawing it all live. */
static void dialog_resized(lv_event_t *e)
{
    dialog_t *d = lv_event_get_user_data(e);
    if (d->baked || d->box_snap) dialog_thaw(d);
}

/* After the dialog's first frame (the live scrim is on screen): freeze the
 * screen behind it with the scrim and shadow baked in. Those pixels are
 * the ones already shown, so invalidation is off meanwhile: the swap costs
 * no frame, and the framebuffers catch up with the dim during the snapshot
 * instead of during the fade. */
static void dialog_prepare(void *arg)
{
    dialog_t *d = arg;
    d->prepare_queued = false;
    if (d->closing) return;
    lv_display_t *disp = lv_obj_get_display(d->scrim);
    if (md_freeze_mode() & (MD_FREEZE_DIALOG_BACKDROP | MD_FREEZE_DIALOG_BOX)) {
        bool inv = lv_display_is_invalidation_enabled(disp);
        lv_display_enable_invalidation(disp, false);
        dialog_freeze(d);
        lv_display_enable_invalidation(disp, inv);
        if (d->box_snap) lv_obj_invalidate(d->box);   /* drawn from its snapshot now */
    }
    d->settle = lv_timer_create(open_when_settled, 4, d);
}

static void dialog_first_frame(lv_event_t *e)
{
    dialog_t *d = lv_event_get_user_data(e);
    lv_display_remove_event_cb_with_user_data(lv_obj_get_display(d->scrim), dialog_first_frame, d);
    d->waiting_frame = false;
    d->prepare_queued = lv_async_call(dialog_prepare, d) == LV_RESULT_OK;
}

static void dialog_free(lv_event_t *e)
{
    dialog_t *d = lv_event_get_user_data(e);
    stop_settle(d);
    if (d->waiting_frame) lv_display_remove_event_cb_with_user_data(lv_obj_get_display(d->scrim), dialog_first_frame, d);
    if (d->prepare_queued) lv_async_call_cancel(dialog_prepare, d);
    lv_display_remove_event_cb_with_user_data(lv_obj_get_display(d->scrim), dialog_resized, d);
    dialog_thaw_ex(d, false);
    free(d);
}

static void dialog_anim(dialog_t *d, lv_opa_t box_from, lv_opa_t box_to, lv_opa_t scrim_from, lv_opa_t scrim_to,
                        uint32_t ms, md_curve_t curve, lv_anim_completed_cb_t done)
{
    d->box_opa = box_from;
    if (!d->box_snap) lv_obj_set_style_opa(d->box, box_from, 0);
    scrim_set(d, scrim_from);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, d->box);
    lv_anim_set_exec_cb(&a, box_opa_anim);
    lv_anim_set_values(&a, box_from, box_to);
    lv_anim_set_duration(&a, ms);
    md_anim_set_curve(&a, curve);
    /* A scrim that doesn't change: only the box animates, so each frame
     * redraws the box's area, not the whole screen (see md_dialog_show). */
    if (scrim_from == scrim_to) lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
    if (scrim_from == scrim_to) return;
    lv_anim_init(&a);
    lv_anim_set_var(&a, d->scrim);
    lv_anim_set_exec_cb(&a, scrim_opa_anim);
    lv_anim_set_values(&a, scrim_from, scrim_to);
    lv_anim_set_duration(&a, ms);
    md_anim_set_curve(&a, curve);
    lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

void md_dialog_close(lv_obj_t *scrim)
{
    dialog_t *d = lv_obj_get_user_data(scrim);
    if (!d || d->closing) return;
    d->closing = true;
    stop_settle(d);
    if (d->waiting_frame) {
        lv_display_remove_event_cb_with_user_data(lv_obj_get_display(scrim), dialog_first_frame, d);
        d->waiting_frame = false;
    }
    /* Still opening: stop there, from wherever it got to. */
    lv_opa_t box_from = d->box_snap ? d->box_opa : lv_obj_get_style_opa(d->box, 0);
    lv_anim_delete(d->box, box_opa_anim);
    lv_anim_delete(scrim, scrim_opa_anim);
    lv_opa_t scrim_from = d->scrim_opa;
    if (!d->baked) {   /* not frozen (or thawed by a rotation): freeze now */
        dialog_thaw(d);
        lv_obj_set_style_opa(d->box, LV_OPA_COVER, 0);
        dialog_freeze(d);
    }
    /* The scrim stays put while the box fades, and goes with the dialog. */
    dialog_anim(d, box_from, LV_OPA_TRANSP, scrim_from, scrim_from, MD_DUR_DIALOG_OUT_MS,
                MD_CURVE_ACCELERATION, dialog_deleted);
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
    if (d->box_snap) {
        /* Opening: the box is hidden behind its snapshot, so a tap on it
         * lands on the scrim. */
        lv_point_t p;
        lv_area_t a;
        lv_indev_get_point(lv_indev_active(), &p);
        lv_obj_get_coords(d->box, &a);
        if (p.x >= a.x1 && p.x <= a.x2 && p.y >= a.y1 && p.y <= a.y2) return;
    }
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
    lv_display_add_event_cb(lv_obj_get_display(scrim), dialog_resized, LV_EVENT_RESOLUTION_CHANGED, d);
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

    if (title) {
        lv_obj_t *t = md_label_create(box, title, MD_TYPE_HEADLINE6);
        lv_obj_set_width(t, LV_PCT(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    }
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

    /* The scrim (MD2: 32%) appears at once and only the dialog fades in.
     * Fading the scrim too redraws the whole screen every frame, which this
     * PSRAM can't do at 30 fps (bench-results.md, "Smoothness"); a scrim
     * that's there from the first frame leaves just the box's area to
     * redraw. Never animate the shadow's size. */
    lv_obj_add_event_cb(scrim, scrim_draw, LV_EVENT_DRAW_MAIN_END, d);
    d->shadow_dp = md_shadow_dp(box);
    md_shadow_attach(box, 0);
    /* The scrim and shadow, live, on the very next frame: the dim shows at
     * once (a translucent fill, on the PPA). The backdrop is frozen behind
     * it after that frame (dialog_prepare), and the box fades in once every
     * framebuffer has the dim (md_freeze_settled), so the fade's frames
     * redraw only the box. */
    d->scrim_opa = MD_DIALOG_SCRIM_OPA;
    lv_obj_set_style_bg_opa(scrim, MD_DIALOG_SCRIM_OPA, 0);
    lv_obj_set_style_opa(box, LV_OPA_TRANSP, 0);
    lv_display_add_event_cb(lv_obj_get_display(scrim), dialog_first_frame, LV_EVENT_REFR_READY, d);
    d->waiting_frame = true;
    return scrim;
}

void md_list_set_long_press(lv_obj_t *list, md_list_click_cb_t cb)
{
    list_t *l = lv_obj_get_user_data(list);
    if (l) l->long_press = cb;
}
