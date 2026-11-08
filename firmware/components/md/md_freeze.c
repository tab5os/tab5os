/* SPDX-License-Identifier: Apache-2.0
 * Snapshot compositing for screen transitions and dialog backdrops.
 *
 * A full-screen animation (a slide, a fade, a scrim fading over a screen)
 * otherwise redraws every widget of both screens in every frame. Instead,
 * each screen involved is rendered once into an RGB565 snapshot in PSRAM
 * and hidden; the display's bottom layer draws the snapshots where the
 * screens currently are, at their current opacity. LVGL's own animation
 * logic (positions, opacity, screen events, auto-delete) runs unchanged,
 * and the top and sys layers (status bar, dialogs, keyboard) stay live
 * above. When the animation ends the screens are shown again.
 *
 * On the device every lv_screen_load_anim() call goes through here (the
 * linker wraps it; md's CMakeLists.txt), so apps get it too.
 */
#include <stdio.h>
#include <string.h>
#include "md/md.h"
#include "md_priv.h"
#include "misc/lv_area_private.h"   /* lv_area_intersect */
#include "display/lv_display_private.h"   /* act_scr, scr_to_load, draw_prev_over_act */

#define MAX_FROZEN 2

typedef struct {
    lv_obj_t *obj;
    lv_draw_buf_t *snap;
    bool was_hidden;
    /* A scrim over it (md_freeze_dim): drawn as the snapshot at 255 - opa
     * over a fill of the scrim colour, the same pixels as blending a scrim
     * over it, but a fill and a PPA blit instead of a software blend. */
    lv_color_t dim_color;
    lv_opa_t dim_opa;
    md_freeze_thaw_cb_t on_thaw;   /* told when it goes live again, whoever thaws it */
    void *on_thaw_arg;
} frozen_t;

/* [0] is drawn first (underneath). For a transition: the old screen, then
 * the new one, swapped for the "out" animations, which LVGL draws the
 * other way round. */
static frozen_t s_fz[MAX_FROZEN];
static bool s_hooked;
static bool s_enabled = true;
/* Transitions, and dialogs over a frozen backdrop with the scrim and
 * shadow baked in (bench-results.md, "Smoothness"). The dialog's box
 * snapshot (MD_FREEZE_DIALOG_BOX) measured no faster. */
static uint32_t s_mode = MD_FREEZE_TRANSITIONS | MD_FREEZE_DIALOG_BACKDROP;
static md_freeze_stats_t s_st;
static lv_anim_t *s_tr_anim;              /* the running transition's ticker */
static int s_tr_var;                       /* its anim var */
static void tr_tick(void *var, int32_t v);

static frozen_t *find(const lv_obj_t *o)
{
    for (int i = 0; i < MAX_FROZEN; i++) if (s_fz[i].obj == o) return &s_fz[i];
    return NULL;
}

/* Where a frozen screen's snapshot goes, at what opacity. */
static lv_opa_t snap_area(const frozen_t *f, lv_area_t *a)
{
    if (!f->obj || !f->snap) return LV_OPA_TRANSP;
    lv_obj_get_coords(f->obj, a);
    a->x2 = a->x1 + (int32_t)f->snap->header.w - 1;
    a->y2 = a->y1 + (int32_t)f->snap->header.h - 1;
    lv_opa_t opa = lv_obj_get_style_opa(f->obj, LV_PART_MAIN);
    return f->dim_opa && opa >= LV_OPA_COVER ? LV_OPA_COVER : opa;   /* the fill covers */
}

static bool contains(const lv_area_t *o, const lv_area_t *i)
{
    return o->x1 <= i->x1 && o->y1 <= i->y1 && o->x2 >= i->x2 && o->y2 >= i->y2;
}

/* Do the opaque snapshots cover `clip` (one alone, or two side by side as
 * in a slide)? Then the bottom layer's background needn't be drawn there. */
static bool snaps_cover(const lv_area_t *clip)
{
    lv_area_t r[MAX_FROZEN];
    int n = 0;
    for (int i = 0; i < MAX_FROZEN; i++) {
        if (snap_area(&s_fz[i], &r[n]) >= LV_OPA_COVER) n++;
    }
    for (int i = 0; i < n; i++) if (contains(&r[i], clip)) return true;
    if (n < 2) return false;
    const lv_area_t *a = &r[0], *b = &r[1];
    if (a->y1 <= clip->y1 && a->y2 >= clip->y2 && b->y1 <= clip->y1 && b->y2 >= clip->y2) {
        if (b->x1 < a->x1) { const lv_area_t *t = a; a = b; b = t; }
        return a->x1 <= clip->x1 && b->x1 <= a->x2 + 1 && (a->x2 > b->x2 ? a->x2 : b->x2) >= clip->x2;
    }
    if (a->x1 <= clip->x1 && a->x2 >= clip->x2 && b->x1 <= clip->x1 && b->x2 >= clip->x2) {
        if (b->y1 < a->y1) { const lv_area_t *t = a; a = b; b = t; }
        return a->y1 <= clip->y1 && b->y1 <= a->y2 + 1 && (a->y2 > b->y2 ? a->y2 : b->y2) >= clip->y2;
    }
    return false;
}

/* A crossfade of two frozen screens (a fade-in or fade-out transition: one
 * screen at partial opacity over the other, in the same place) is drawn
 * as a fade through the background instead: the screen underneath fades
 * out for the first half, the one on top fades in for the second. Each
 * frame then blends one snapshot rather than blitting one and blending the
 * other, half the bytes read (bench-results.md, "Smoothness"). Returns
 * false for anything else. */
static bool fade_through(lv_opa_t opa_out[MAX_FROZEN])
{
    lv_area_t a[MAX_FROZEN];
    lv_opa_t o[MAX_FROZEN];
    for (int i = 0; i < MAX_FROZEN; i++) {
        if (!s_fz[i].obj || s_fz[i].dim_opa) return false;
        o[i] = snap_area(&s_fz[i], &a[i]);
    }
    if (memcmp(&a[0], &a[1], sizeof a[0]) || o[0] < LV_OPA_COVER || o[1] >= LV_OPA_COVER) return false;
    uint32_t m = o[1];   /* [1] is drawn on top: its opacity is the mix */
    opa_out[0] = m < 128 ? (lv_opa_t)(255 - 2 * m) : LV_OPA_TRANSP;
    opa_out[1] = m < 128 ? LV_OPA_TRANSP : (lv_opa_t)(2 * m - 255);
    /* Nearly opaque: draw it opaque, a plain blit rather than a blend. */
    for (int i = 0; i < MAX_FROZEN; i++) {
        if (opa_out[i] >= 240) opa_out[i] = LV_OPA_COVER;
    }
    return true;
}

static void draw_one(lv_layer_t *layer, const frozen_t *f, int force_opa)
{
    lv_area_t a;
    lv_opa_t opa = snap_area(f, &a);
    if (force_opa >= 0) opa = (lv_opa_t)force_opa;
    if (opa <= LV_OPA_MIN) return;
    if (f->dim_opa) {
        lv_draw_rect_dsc_t r;
        lv_draw_rect_dsc_init(&r);
        r.bg_color = f->dim_color;
        r.bg_opa = opa;
        lv_draw_rect(layer, &r, &a);
        opa = (lv_opa_t)((uint32_t)opa * (255 - f->dim_opa) / 255);
        if (opa <= LV_OPA_MIN) return;
    }
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = f->snap;
    d.opa = opa;
    lv_draw_image(layer, &d, &a);
}

/* The bottom layer's background is drawn here rather than by LVGL (hook()
 * makes it transparent), and only where no snapshot covers it: during a
 * transition LVGL would otherwise fill every band in software just for
 * the snapshots to cover it, ~11 ms of a 40 ms frame. */
static void bottom_draw(lv_event_t *e)
{
    lv_obj_t *bottom = lv_event_get_current_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    bool ours = lv_event_get_user_data(e) == NULL;   /* else LVGL drew it */
    lv_opa_t ft[MAX_FROZEN];
    bool through = fade_through(ft);
    /* Coverage by the opacity actually drawn: a fade through draws the
     * snapshots below their screens' opacity, and over an unfilled band they
     * would blend with whatever the band held last (ghost copies one band
     * apart). */
    bool covered = through ? (ft[0] >= LV_OPA_COVER || ft[1] >= LV_OPA_COVER) : snaps_cover(&layer->_clip_area);
    if (ours && !covered) {
        lv_draw_rect_dsc_t r;
        lv_draw_rect_dsc_init(&r);
        r.bg_color = lv_obj_get_style_bg_color(bottom, LV_PART_MAIN);
        r.bg_opa = LV_OPA_COVER;
        lv_draw_rect(layer, &r, &layer->_clip_area);
    }
    for (int i = 0; i < MAX_FROZEN; i++) draw_one(layer, &s_fz[i], through ? ft[i] : -1);
}

static void frozen_deleted(lv_event_t *e)
{
    frozen_t *f = find(lv_event_get_current_target_obj(e));
    if (!f) return;
    if (f->snap) lv_draw_buf_destroy(f->snap);
    memset(f, 0, sizeof *f);
}

static void hook(lv_display_t *d)
{
    if (s_hooked) return;
    lv_obj_t *bottom = lv_display_get_layer_bottom(d);
    /* Only for opaque displays (LVGL gives those an opaque bottom layer). */
    if (lv_obj_get_style_bg_opa(bottom, LV_PART_MAIN) >= LV_OPA_COVER) {
        lv_obj_set_style_bg_opa(bottom, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_add_event_cb(bottom, bottom_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    } else {
        lv_obj_add_event_cb(bottom, bottom_draw, LV_EVENT_DRAW_MAIN_END, (void *)1);
    }
    s_hooked = true;
}

bool md_freeze_screen(lv_obj_t *scr)
{
    if (!s_enabled || !scr || lv_obj_get_parent(scr)) return false;
    if (find(scr)) return true;
    frozen_t *f = find(NULL);
    if (!f) return false;
    lv_display_t *d = lv_obj_get_display(scr);
    hook(d);
    uint32_t t0 = lv_tick_get();
    lv_draw_buf_t *snap = lv_snapshot_take(scr, LV_COLOR_FORMAT_RGB565);
    if (!snap) {
        s_st.failed++;
        return false;
    }
    /* The CPU drew it through the cache and the PPA reads memory: write it
     * back now, or the PPA can read stale lines, and the first frames pay
     * for the evictions. */
    lv_draw_buf_flush_cache(snap, NULL);
    uint32_t ms = lv_tick_elaps(t0);
    s_st.snapshots++;
    s_st.snap_ms_last = ms;
    if (ms > s_st.snap_ms_max) s_st.snap_ms_max = ms;
    f->obj = scr;
    f->snap = snap;
    f->was_hidden = lv_obj_is_hidden(scr);
    lv_obj_add_event_cb(scr, frozen_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_set_hidden(scr, true);
    lv_obj_invalidate(lv_display_get_layer_bottom(d));
    return true;
}

void md_thaw_screen(lv_obj_t *scr)
{
    frozen_t *f = scr ? find(scr) : NULL;
    if (!f) return;
    lv_obj_remove_event_cb_with_user_data(scr, frozen_deleted, NULL);
    lv_draw_buf_t *snap = f->snap;
    bool hidden = f->was_hidden;
    md_freeze_thaw_cb_t cb = f->on_thaw;
    void *cb_arg = f->on_thaw_arg;
    memset(f, 0, sizeof *f);
    if (cb) cb(scr, cb_arg);
    if (!hidden) lv_obj_set_hidden(scr, false);
    lv_obj_invalidate(lv_display_get_layer_bottom(lv_obj_get_display(scr)));
    if (snap) lv_draw_buf_destroy(snap);
}

void md_thaw_screens(void)
{
    if (s_tr_anim) {
        s_tr_anim = NULL;
        lv_anim_delete(&s_tr_var, tr_tick);
    }
    for (int i = 0; i < MAX_FROZEN; i++) {
        if (s_fz[i].obj) md_thaw_screen(s_fz[i].obj);
        else if (s_fz[i].snap) {
            lv_draw_buf_destroy(s_fz[i].snap);
            memset(&s_fz[i], 0, sizeof s_fz[i]);
        }
    }
}

void md_freeze_dim(lv_obj_t *scr, lv_color_t color, lv_opa_t opa)
{
    frozen_t *f = scr ? find(scr) : NULL;
    if (!f || (f->dim_opa == opa && lv_color_eq(f->dim_color, color))) return;
    f->dim_color = color;
    f->dim_opa = opa;
    lv_obj_invalidate(lv_display_get_layer_bottom(lv_obj_get_display(scr)));
}

bool md_freeze_draw_into(lv_obj_t *scr, md_freeze_draw_fn_t fn, void *arg)
{
    frozen_t *f = scr ? find(scr) : NULL;
    if (!f || !f->snap || !fn) return false;
    uint32_t t0 = lv_tick_get();
    /* A hidden canvas over the snapshot: what fn draws goes through LVGL's
     * draw units, so translucent fills and images land on the PPA. */
    lv_obj_t *cv = lv_canvas_create(lv_display_get_layer_top(lv_obj_get_display(scr)));
    if (!cv) return false;
    lv_obj_set_hidden(cv, true);
    lv_canvas_set_draw_buf(cv, f->snap);
    lv_layer_t layer;
    lv_canvas_init_layer(cv, &layer);
    fn(&layer, arg);
    lv_canvas_finish_layer(cv, &layer);
    lv_obj_delete(cv);
    f->dim_opa = 0;
    s_st.bake_ms_last = lv_tick_elaps(t0);
    lv_obj_invalidate(lv_display_get_layer_bottom(lv_obj_get_display(scr)));
    return true;
}

void md_freeze_set_thaw_cb(lv_obj_t *scr, md_freeze_thaw_cb_t cb, void *arg)
{
    frozen_t *f = scr ? find(scr) : NULL;
    if (!f) return;
    f->on_thaw = cb;
    f->on_thaw_arg = arg;
}

const lv_draw_buf_t *md_freeze_snapshot(const lv_obj_t *scr)
{
    frozen_t *f = scr ? find(scr) : NULL;
    return f ? f->snap : NULL;
}

bool md_screens_frozen(void)
{
    for (int i = 0; i < MAX_FROZEN; i++) if (s_fz[i].obj) return true;
    return false;
}

void md_freeze_set_enabled(bool on)
{
    s_enabled = on;
    if (!on) md_thaw_screens();
}

bool md_freeze_enabled(void)
{
    return s_enabled;
}

void md_freeze_set_mode(uint32_t mask)
{
    s_mode = mask;
}

uint32_t md_freeze_mode(void)
{
    return s_enabled ? s_mode : 0;
}

static md_perf_probe_t s_settled;

void md_freeze_set_settled_probe(md_perf_probe_t probe)
{
    s_settled = probe;
}

bool md_freeze_settled(void)
{
    return !s_settled || s_settled();
}

void md_freeze_note_dialog(uint32_t prep_ms, uint32_t box_snap_ms, uint32_t compose_ms)
{
    s_st.dialog_prep_ms_last = prep_ms;
    s_st.box_snap_ms_last = box_snap_ms;
    s_st.compose_ms_last = compose_ms;
}

/* ---- scrims over a frozen backdrop ------------------------------------------ */
/* Objects on the top layer under a scrim (the status bar) aren't in the
 * frozen backdrop, which is the screen alone, and they keep redrawing. So
 * the scrim is baked in everywhere but their areas, and drawn live over
 * those, which matches what a live scrim shows. */
#define MAX_BELOW 8
static int below_areas(lv_obj_t *scrim, lv_area_t *out)
{
    lv_obj_t *parent = lv_obj_get_parent(scrim);
    int32_t idx = lv_obj_get_index(scrim), n = 0;
    for (int32_t i = 0; i < idx && n < MAX_BELOW; i++) {
        lv_obj_t *o = lv_obj_get_child(parent, i);
        if (o && !lv_obj_is_hidden(o)) lv_obj_get_coords(o, &out[n++]);
    }
    return n;
}

/* r minus cut: up to four pieces. */
static int area_minus(const lv_area_t *r, const lv_area_t *cut, lv_area_t *out)
{
    lv_area_t c;
    if (!lv_area_intersect(&c, r, cut)) {
        out[0] = *r;
        return 1;
    }
    int n = 0;
    if (c.y1 > r->y1) out[n++] = (lv_area_t){ r->x1, r->y1, r->x2, c.y1 - 1 };
    if (c.y2 < r->y2) out[n++] = (lv_area_t){ r->x1, c.y2 + 1, r->x2, r->y2 };
    if (c.x1 > r->x1) out[n++] = (lv_area_t){ r->x1, c.y1, c.x1 - 1, c.y2 };
    if (c.x2 < r->x2) out[n++] = (lv_area_t){ c.x2 + 1, c.y1, r->x2, c.y2 };
    return n;
}

void md_scrim_draw(lv_layer_t *layer, lv_obj_t *scrim, lv_opa_t opa, bool bake)
{
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_obj_get_style_bg_color(scrim, 0);
    r.bg_opa = opa;
    lv_area_t below[MAX_BELOW];
    int nb = below_areas(scrim, below);
    if (!bake) {
        for (int i = 0; i < nb; i++) lv_draw_rect(layer, &r, &below[i]);
        return;
    }
    lv_area_t parts[2][4 * MAX_BELOW + 1];
    int np = 1;
    parts[0][0] = layer->_clip_area;
    for (int i = 0; i < nb; i++) {
        int nn = 0;
        for (int k = 0; k < np; k++) {
            lv_area_t pcs[4];
            int m = area_minus(&parts[i & 1][k], &below[i], pcs);
            for (int j = 0; j < m && nn < 4 * MAX_BELOW + 1; j++) parts[(i + 1) & 1][nn++] = pcs[j];
        }
        np = nn;
    }
    for (int k = 0; k < np; k++) lv_draw_rect(layer, &r, &parts[nb & 1][k]);
}

void md_freeze_print(void)
{
    for (int i = 0; i < MAX_FROZEN; i++) {
        lv_area_t a;
        const frozen_t *f = &s_fz[i];
        lv_opa_t o = snap_area(f, &a);
        if (f->obj)
            printf("frozen[%d] %p at %d,%d-%d,%d opa %d snap %dx%d dim %d\n", i, (void *)f->obj, (int)a.x1, (int)a.y1,
                        (int)a.x2, (int)a.y2, (int)o, f->snap ? (int)f->snap->header.w : 0,
                        f->snap ? (int)f->snap->header.h : 0, (int)f->dim_opa);
    }
}

void md_freeze_stats(md_freeze_stats_t *out)
{
    if (out) *out = s_st;
}

/* ---- screen transitions ------------------------------------------------------ */
static lv_display_t *s_tr_disp;
static lv_obj_t *s_tr_new;

static void thaw_async(void *p)
{
    (void)p;
    md_thaw_screens();
}

/* Every tick of the transition: the screens moved (they are hidden, so
 * LVGL didn't invalidate anything); redraw from the snapshots. Stop if
 * something else took over the display meanwhile. */
static void tr_tick(void *var, int32_t v)
{
    (void)var; (void)v;
    lv_display_t *d = s_tr_disp;
    if (!d) return;
    if (lv_display_get_screen_active(d) != s_tr_new && d->scr_to_load != s_tr_new) {
        lv_async_call(thaw_async, NULL);
        return;
    }
    lv_obj_invalidate(lv_display_get_layer_bottom(d));
}

static void tr_done(lv_anim_t *a)
{
    (void)a;
    s_tr_anim = NULL;
    md_thaw_screens();
}

static bool is_out(lv_screen_load_anim_t t)
{
    return t == LV_SCREEN_LOAD_ANIM_FADE_OUT || t == LV_SCREEN_LOAD_ANIM_OUT_LEFT ||
           t == LV_SCREEN_LOAD_ANIM_OUT_RIGHT || t == LV_SCREEN_LOAD_ANIM_OUT_TOP ||
           t == LV_SCREEN_LOAD_ANIM_OUT_BOTTOM;
}

#ifdef ESP_PLATFORM
void __real_lv_screen_load_anim(lv_obj_t *scr, lv_screen_load_anim_t anim, uint32_t time, uint32_t delay,
                                bool auto_del);
#define LOAD_ANIM __real_lv_screen_load_anim
#else
#define LOAD_ANIM lv_screen_load_anim
#endif

void md_screen_load_anim(lv_obj_t *scr, lv_screen_load_anim_t anim, uint32_t time, uint32_t delay, bool auto_del)
{
    lv_display_t *d = scr ? lv_obj_get_display(scr) : NULL;
    lv_obj_t *old = d ? lv_display_get_screen_active(d) : NULL;
    md_thaw_screens();
    bool comp = (md_freeze_mode() & MD_FREEZE_TRANSITIONS) && d && old && old != scr && time > 0 && anim != LV_SCREEN_LOAD_ANIM_NONE &&
                !d->scr_to_load;
    if (comp) {
        /* Snapshot both before LVGL moves anything: the new screen at 0,0. */
        int under = is_out(anim) ? 1 : 0;
        comp = md_freeze_screen(old) && md_freeze_screen(scr);
        if (comp && under) {
            frozen_t t = s_fz[0];
            s_fz[0] = s_fz[1];
            s_fz[1] = t;
        }
        if (!comp) {
            md_thaw_screens();
            s_st.fallbacks++;
        }
    }
    LOAD_ANIM(scr, anim, time, delay, auto_del);
    if (!comp) return;
    s_tr_disp = d;
    s_tr_new = scr;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_tr_var);
    lv_anim_set_exec_cb(&a, tr_tick);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, time);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_completed_cb(&a, tr_done);
    s_tr_anim = lv_anim_start(&a);
    s_st.transitions++;
}

#ifdef ESP_PLATFORM
void __wrap_lv_screen_load_anim(lv_obj_t *scr, lv_screen_load_anim_t anim, uint32_t time, uint32_t delay,
                                bool auto_del)
{
    md_screen_load_anim(scr, anim, time, delay, auto_del);
}
#endif
