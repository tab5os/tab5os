/* SPDX-License-Identifier: Apache-2.0
 * MD2 ripple (plan 4.7): an ink circle growing from the touch point,
 * clipped to the widget. Timings from MDC: 75 ms fade in, 225 ms expand,
 * 150 ms fade out after release (and after the expansion finishes).
 *
 * Ink is the widget's text colour (white on dark surfaces, on-primary on
 * contained buttons) at the scheme's ink opacity. Round widgets (FABs,
 * radio-like controls) ripple from their centre, out to their radius, so
 * the square clip never shows. Rounded-corner widgets clip to their box;
 * the ink can touch the 4 dp corners for one frame, which is invisible at
 * 24% on dark.
 *
 * Fallback (design, "Motion"): when the probe reports a tight frame budget,
 * or in low-power mode, draw a plain state overlay over the whole widget
 * instead: one opaque-shape fill, no circle, no clip.
 */
#include <stdlib.h>
#include "md/md.h"
#include "md_priv.h"

typedef struct {
    lv_obj_t *obj;
    lv_point_t c;           /* centre, relative to the object */
    int32_t r, r_max;
    lv_opa_t opa;           /* current ink opacity, 0..255 */
    lv_opa_t ink;           /* target opacity */
    bool pressed, expanded, fading, fade_only;
} ripple_t;

static md_perf_probe_t s_probe;
static uint32_t s_presses, s_fallbacks;

void md_ripple_set_probe(md_perf_probe_t probe)
{
    s_probe = probe;
}

void md_ripple_stats(uint32_t *presses, uint32_t *fallbacks)
{
    if (presses) *presses = s_presses;
    if (fallbacks) *fallbacks = s_fallbacks;
}

static void set_r(void *v, int32_t r)
{
    ripple_t *rp = v;
    rp->r = r;
    lv_obj_invalidate(rp->obj);
}

static void set_opa(void *v, int32_t o)
{
    ripple_t *rp = v;
    rp->opa = (lv_opa_t)o;
    lv_obj_invalidate(rp->obj);
}

static void fade_out(ripple_t *rp)
{
    if (rp->fading) return;
    rp->fading = true;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, rp);
    lv_anim_set_exec_cb(&a, set_opa);
    lv_anim_set_values(&a, rp->opa, 0);
    lv_anim_set_duration(&a, MD_DUR_RIPPLE_FADE_OUT_MS);
    md_anim_set_curve(&a, MD_CURVE_STANDARD);
    lv_anim_start(&a);
}

static void expand_done(lv_anim_t *a)
{
    ripple_t *rp = a->var;
    rp->expanded = true;
    if (!rp->pressed) fade_out(rp);
}

static int32_t far_corner(lv_point_t c, int32_t w, int32_t h)
{
    int32_t dx = LV_MAX(c.x, w - c.x), dy = LV_MAX(c.y, h - c.y);
    return (int32_t)lv_sqrt32((uint32_t)(dx * dx + dy * dy)) + 1;
}

static bool is_round(lv_obj_t *obj)
{
    int32_t rad = lv_obj_get_style_radius(obj, 0);
    int32_t m = LV_MIN(lv_obj_get_width(obj), lv_obj_get_height(obj)) / 2;
    return rad == LV_RADIUS_CIRCLE || rad >= m;
}

static void press(ripple_t *rp)
{
    lv_obj_t *obj = rp->obj;
    int32_t w = lv_obj_get_width(obj), h = lv_obj_get_height(obj);
    lv_anim_delete(rp, NULL);
    s_presses++;
    rp->pressed = true;
    rp->expanded = false;
    rp->fading = false;
    rp->ink = md_scheme()->ripple_ink_opa;
    rp->fade_only = s_probe && !s_probe();
    if (rp->fade_only) s_fallbacks++;

    if (is_round(obj)) {
        rp->c = (lv_point_t){ w / 2, h / 2 };
        rp->r_max = LV_MIN(w, h) / 2;
    } else {
        lv_point_t p = { w / 2, h / 2 };
        lv_indev_t *in = lv_indev_active();
        if (in) {
            lv_indev_get_point(in, &p);
            lv_area_t co;
            lv_obj_get_coords(obj, &co);
            p.x -= co.x1;
            p.y -= co.y1;
        }
        rp->c = p;
        rp->r_max = far_corner(p, w, h);
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, rp);
    lv_anim_set_exec_cb(&a, set_opa);
    lv_anim_set_values(&a, 0, rp->ink);
    lv_anim_set_duration(&a, MD_DUR_RIPPLE_FADE_IN_MS);
    lv_anim_start(&a);

    if (rp->fade_only) {
        rp->expanded = true;
        rp->r = rp->r_max;
        return;
    }
    /* MDC starts the circle at ~60% of the larger side's 0.6 ratio; start
     * from a small seed so the touch point is visible on the first frame. */
    lv_anim_init(&a);
    lv_anim_set_var(&a, rp);
    lv_anim_set_exec_cb(&a, set_r);
    lv_anim_set_values(&a, LV_MAX(T5_DP(8), rp->r_max / 5), rp->r_max);
    lv_anim_set_duration(&a, MD_DUR_RIPPLE_EXPAND_MS);
    lv_anim_set_completed_cb(&a, expand_done);
    md_anim_set_curve(&a, MD_CURVE_DECELERATION);
    lv_anim_start(&a);
    rp->r = LV_MAX(T5_DP(8), rp->r_max / 5);
}

static void draw(ripple_t *rp, lv_event_t *e)
{
    if (rp->opa == 0) return;
    lv_obj_t *obj = rp->obj;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t co;
    lv_obj_get_coords(obj, &co);

    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_obj_get_style_text_color(obj, 0);
    d.bg_opa = rp->opa;
    if (rp->fade_only) {
        d.radius = lv_obj_get_style_radius(obj, 0);
        lv_draw_rect(layer, &d, &co);
        return;
    }
    lv_area_t circle = {
        co.x1 + rp->c.x - rp->r, co.y1 + rp->c.y - rp->r,
        co.x1 + rp->c.x + rp->r, co.y1 + rp->c.y + rp->r,
    };
    d.radius = LV_RADIUS_CIRCLE;
    /* Clip to the widget's box for this one draw. */
    lv_area_t saved = layer->_clip_area;
    lv_area_t clip = {
        LV_MAX(saved.x1, co.x1), LV_MAX(saved.y1, co.y1), LV_MIN(saved.x2, co.x2), LV_MIN(saved.y2, co.y2),
    };
    if (clip.x1 > clip.x2 || clip.y1 > clip.y2) return;
    layer->_clip_area = clip;
    lv_draw_rect(layer, &d, &circle);
    layer->_clip_area = saved;
}

static void ripple_event(lv_event_t *e)
{
    ripple_t *rp = lv_event_get_user_data(e);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        press(rp);
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        rp->pressed = false;
        if (rp->expanded) fade_out(rp);
        break;
    case LV_EVENT_DRAW_MAIN_END:
        draw(rp, e);
        break;
    case LV_EVENT_DELETE:
        lv_anim_delete(rp, NULL);
        free(rp);
        break;
    default:
        break;
    }
}

void md_ripple_attach(lv_obj_t *obj)
{
    /* Once per object: the theme applies on creation, widgets may call it too. */
    uint32_t n = lv_obj_get_event_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_event_dsc_t *d = lv_obj_get_event_dsc(obj, i);
        if (lv_event_dsc_get_cb(d) == ripple_event) return;
    }
    ripple_t *rp = calloc(1, sizeof *rp);
    if (!rp) return;
    rp->obj = obj;
    lv_obj_add_event_cb(obj, ripple_event, LV_EVENT_ALL, rp);
    /* The theme cancels LVGL's pressed darkening (md_style_no_press()). */
    lv_obj_add_style(obj, md_style_no_press(), LV_STATE_PRESSED);
}
