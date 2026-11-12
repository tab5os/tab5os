/* SPDX-License-Identifier: Apache-2.0
 * Video planes on screen (t5_ui/plane.h). UI task only.
 *
 * Each plane is an lv_image the size of the plane. A timer takes posted
 * frames. A frame that can take the fast path is drawn by the display
 * pipeline with one PPA pass from the producer's buffer straight into the
 * framebuffer (scaled, converted and turned for the panel at once):
 *   - nothing over it: a frame of its own, now (ui.c ui_plane_present);
 *   - things over it: they are invalidated, and at the start of LVGL's
 *     frame (frame_begin) the picture goes in first, then the parts LVGL
 *     is about to redraw are read back from it, upright, into the image's
 *     own buffer, so the objects over it blend onto the right pixels. The
 *     PPA keeps order, so LVGL's bands land on top. LVGL only waits for
 *     the read-back where it wasn't read back last frame too: elsewhere
 *     (things that stay put over a moving picture) what lies under a
 *     translucent object is a frame old, and the UI doesn't stall on the
 *     PPA every frame.
 * The image's buffer (the underlay) is therefore only right where LVGL
 * draws. When the fast path can't be used (a screen animation, the image
 * transformed or clipped) the whole frame goes into the underlay and LVGL
 * draws the image like any other: the slow path.
 */
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "core/lv_obj_draw_private.h"    /* lv_obj_get_ext_draw_size */
#include "core/lv_refr_private.h"        /* lv_inv_area */
#include "display/lv_display_private.h"  /* screen animations in progress */
#include "misc/lv_area_private.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_display/t5_display.h"
#include "t5_ui/plane.h"
#include "t5_ui/t5_ui.h"
#include "ui_hw.h"
#include "ui_priv.h"

static const char *TAG = "t5.plane";

typedef struct ui_plane {
    t5_plane_t *plane;
    lv_obj_t *img;
    lv_image_dsc_t dsc;
    uint16_t *under;            /* the picture upright, where LVGL draws it */
    size_t under_bytes;
    t5_plane_src_t cur;         /* the frame on screen */
    bool have;                  /* cur is set */
    bool dirty;                 /* cur hasn't reached a framebuffer yet */
    bool under_full;            /* the underlay holds all of cur */
    t5_rect_t known[8];         /* plane areas read back last frame */
    int nknown;
    lv_timer_t *timer;
    struct ui_plane *next;
} ui_plane_t;

static ui_plane_t *s_planes;
static t5_ui_plane_stats_t s_st;

void t5_ui_plane_stats(t5_ui_plane_stats_t *out)
{
    *out = s_st;
}

static bool draws_nothing(lv_obj_t *o)
{
    if (!lv_obj_check_type(o, &lv_obj_class)) return false;
    if (lv_obj_get_style_bg_opa(o, LV_PART_MAIN) > LV_OPA_MIN) return false;
    if (lv_obj_get_style_bg_image_src(o, LV_PART_MAIN)) return false;
    if (lv_obj_get_style_border_width(o, LV_PART_MAIN) && lv_obj_get_style_border_opa(o, LV_PART_MAIN) > LV_OPA_MIN)
        return false;
    if (lv_obj_get_style_outline_width(o, LV_PART_MAIN) && lv_obj_get_style_outline_opa(o, LV_PART_MAIN) > LV_OPA_MIN)
        return false;
    if (lv_obj_get_style_shadow_width(o, LV_PART_MAIN) && lv_obj_get_style_shadow_opa(o, LV_PART_MAIN) > LV_OPA_MIN)
        return false;
    return true;
}

/* Does o hide all of vis (opaque, square, over all of it)? */
static bool hides(lv_obj_t *o, const lv_area_t *vis)
{
    if (lv_obj_get_style_bg_opa(o, LV_PART_MAIN) < LV_OPA_COVER || lv_obj_get_style_radius(o, LV_PART_MAIN) ||
        lv_obj_get_style_opa_recursive(o, LV_PART_MAIN) < LV_OPA_COVER)
        return false;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    return lv_area_is_in(vis, &a, 0);
}

/* Invalidate whatever o (drawn above the plane) puts inside vis; count the
 * areas. A plain container that draws nothing itself costs nothing: only
 * its children count, so a full-screen layout box over the picture doesn't
 * make every frame a full redraw. */
static int inv_over(lv_display_t *d, lv_obj_t *o, const lv_area_t *vis)
{
    /* LVGL doesn't draw it, or anything in it (the shell keeps a
     * transparent standby screen on the top layer, for one). */
    if (lv_obj_is_hidden(o) || lv_obj_get_style_opa(o, LV_PART_MAIN) <= LV_OPA_MIN) return 0;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int32_t ext = lv_obj_get_ext_draw_size(o);
    lv_area_increase(&a, ext, ext);
    bool spills = lv_obj_is_overflow_visible(o);
    int n = 0;
    if (hides(o, vis)) return -1;
    if (!draws_nothing(o)) {
        lv_area_t c;
        if (lv_area_intersect(&c, &a, vis)) {
            lv_inv_area(d, &c);
            n++;
        }
        if (!spills) return n;   /* its children are inside what was just invalidated */
    } else if (!spills && !lv_area_is_on(&a, vis)) {
        return 0;
    }
    uint32_t cnt = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < cnt; i++) {
        int k = inv_over(d, lv_obj_get_child(o, (int32_t)i), vis);
        if (k < 0) return k;
        n += k;
    }
    return n;
}

static int inv_layer(lv_display_t *d, lv_obj_t *layer, const lv_area_t *vis)
{
    if (!layer) return 0;
    int n = 0;
    uint32_t cnt = lv_obj_get_child_count(layer);
    for (uint32_t i = 0; i < cnt; i++) {
        int k = inv_over(d, lv_obj_get_child(layer, (int32_t)i), vis);
        if (k < 0) return k;
        n += k;
    }
    return n;
}

/* Everything drawn after the image: later siblings at every level up to the
 * screen, then the top and system layers. -1 if one of them hides the
 * picture altogether (the standby screen over a video): nothing to draw.
 * (Areas invalidated before that are drawn anyway: harmless.) */
static int inv_overlays(lv_display_t *d, lv_obj_t *img, const lv_area_t *vis)
{
    int n = 0, k;
    for (lv_obj_t *o = img, *par; (par = lv_obj_get_parent(o)); o = par) {
        uint32_t cnt = lv_obj_get_child_count(par);
        for (uint32_t i = (uint32_t)lv_obj_get_index(o) + 1; i < cnt; i++) {
            if ((k = inv_over(d, lv_obj_get_child(par, (int32_t)i), vis)) < 0) return k;
            n += k;
        }
    }
    if ((k = inv_layer(d, lv_display_get_layer_top(d), vis)) < 0) return k;
    n += k;
    if ((k = inv_layer(d, lv_display_get_layer_sys(d), vis)) < 0) return k;
    return n + k;
}

static bool untransformed(lv_obj_t *o)
{
    return lv_obj_get_style_transform_scale_x(o, LV_PART_MAIN) == LV_SCALE_NONE &&
           lv_obj_get_style_transform_scale_y(o, LV_PART_MAIN) == LV_SCALE_NONE &&
           lv_obj_get_style_transform_rotation(o, LV_PART_MAIN) == 0 &&
           lv_obj_get_style_transform_skew_x(o, LV_PART_MAIN) == 0 &&
           lv_obj_get_style_transform_skew_y(o, LV_PART_MAIN) == 0;
}

/* Can the frame go straight into the framebuffer? The image must be on
 * the active screen with no screen animation, untransformed, opaque, and
 * wholly visible (not clipped by a parent or the screen's edge). */
static bool fast_ok(ui_plane_t *u, lv_area_t *c)
{
    lv_obj_t *img = u->img;
    lv_display_t *d = lv_obj_get_display(img);
    if (!d || d->prev_scr || d->scr_to_load) return false;
    if (lv_obj_get_screen(img) != lv_display_get_screen_active(d)) return false;
    if (!lv_obj_is_visible(img)) return false;
    if (lv_image_get_scale_x(img) != LV_SCALE_NONE || lv_image_get_scale_y(img) != LV_SCALE_NONE ||
        lv_image_get_rotation(img) != 0 || lv_image_get_offset_x(img) || lv_image_get_offset_y(img))
        return false;
    if (lv_obj_get_style_opa_recursive(img, LV_PART_MAIN) < LV_OPA_MAX ||
        lv_obj_get_style_image_opa(img, LV_PART_MAIN) < LV_OPA_MAX ||
        lv_obj_get_style_image_recolor_opa(img, LV_PART_MAIN) > LV_OPA_MIN)
        return false;
    lv_obj_get_coords(img, c);
    if (lv_area_get_width(c) != t5_plane_w(u->plane) || lv_area_get_height(c) != t5_plane_h(u->plane)) return false;
    lv_area_t scr = { 0, 0, lv_display_get_horizontal_resolution(d) - 1, lv_display_get_vertical_resolution(d) - 1 };
    if (!lv_area_is_in(c, &scr, 0)) return false;
    for (lv_obj_t *o = img; o; o = lv_obj_get_parent(o)) {
        if (!untransformed(o)) return false;
        if (o != img) {
            lv_area_t pa;
            lv_obj_get_coords(o, &pa);
            if (!lv_area_is_in(c, &pa, 0)) return false;
        }
    }
    return true;
}

/* Where the frame lands (plane coordinates, clipped to the plane), or
 * false if the frame doesn't fit the plane it was posted to. */
static bool landed(const ui_plane_t *u, t5_rect_t *r)
{
    int w, h;
    t5_plane_src_out(&u->cur, &w, &h);
    if (w < 1 || h < 1 || u->cur.dx + w > t5_plane_w(u->plane) || u->cur.dy + h > t5_plane_h(u->plane)) return false;
    *r = (t5_rect_t){ u->cur.dx, u->cur.dy, u->cur.dx + w - 1, u->cur.dy + h - 1 };
    return true;
}

/* The parts of the plane (pw x ph) the frame at r doesn't cover. */
static int uncovered(t5_rect_t r, int pw, int ph, t5_rect_t out[4])
{
    int n = 0;
    if (r.y1 > 0) out[n++] = (t5_rect_t){ 0, 0, pw - 1, r.y1 - 1 };
    if (r.y2 < ph - 1) out[n++] = (t5_rect_t){ 0, r.y2 + 1, pw - 1, ph - 1 };
    if (r.x1 > 0) out[n++] = (t5_rect_t){ 0, r.y1, r.x1 - 1, r.y2 };
    if (r.x2 < pw - 1) out[n++] = (t5_rect_t){ r.x2 + 1, r.y1, pw - 1, r.y2 };
    return n;
}

/* The whole frame into the underlay, for LVGL to draw. */
static bool draw_under(ui_plane_t *u)
{
    int pw = t5_plane_w(u->plane), ph = t5_plane_h(u->plane);
    t5_rect_t r, gaps[4];
    if (!landed(u, &r)) return false;
    int n = uncovered(r, pw, ph, gaps);
    for (int i = 0; i < n; i++) t5_ui_hw_fill_black(u->under, pw, ph, u->under_bytes, gaps[i]);
    if (t5_ui_hw_plane_to(&u->cur, u->under, pw, ph, u->under_bytes) != T5_OK) return false;
    lv_image_cache_drop(&u->dsc);
    u->under_full = true;
    return true;
}

static void tick(lv_timer_t *t)
{
    ui_plane_t *u = lv_timer_get_user_data(t);
    if (t5_plane_take(u->plane, &u->cur)) {
        u->have = true;
        u->dirty = true;
        u->under_full = false;
    }
    if (!u->dirty) return;
    lv_area_t c;
    if (fast_ok(u, &c)) {
        /* Over it: LVGL's next frame carries the picture. Else a frame of
         * its own, now. Either way frame_begin clears dirty, or the next
         * tick tries again (no framebuffer was free). */
        int n = inv_overlays(lv_obj_get_display(u->img), u->img, &c);
        if (n < 0) u->dirty = false;      /* hidden: when it shows again, LVGL draws it from cur */
        else if (!n) ui_plane_present();
        return;
    }
    u->dirty = false;
    if (draw_under(u)) {
        lv_obj_invalidate(u->img);
        s_st.slow++;
    }
}

/* t5_plane_end, on the producer's task: keep the last frame (in the
 * underlay, for LVGL to draw from now on) and stop reading its buffer. The
 * UI lock means no frame is being drawn meanwhile. */
static void ended(t5_plane_t *p, void *arg)
{
    bool ui = t5_ui_on_ui_task();
    if (!ui) t5_ui_lock();
    if (!t5_plane_closed(p)) {       /* else arg is gone: deleted() closes it under the lock */
        ui_plane_t *u = arg;
        if (u->have && !u->under_full) draw_under(u);
        u->have = false;
        u->dirty = false;
    }
    if (!ui) t5_ui_unlock();
}

static void deleted(lv_event_t *e)
{
    ui_plane_t *u = lv_event_get_user_data(e);
    for (ui_plane_t **pp = &s_planes; *pp; pp = &(*pp)->next) {
        if (*pp == u) {
            *pp = u->next;
            break;
        }
    }
    lv_timer_delete(u->timer);
    lv_image_cache_drop(&u->dsc);
    /* Nothing reads the producer's buffers any more: frames finish their
     * PPA work before they end, and this runs between frames. */
    t5_plane_close(u->plane);
    t5_plane_release(u->plane);
    heap_caps_free(u->under);
    free(u);
}

lv_obj_t *t5_ui_plane_create(lv_obj_t *parent, t5_plane_t *plane)
{
    if (!parent || !plane) return NULL;
    ui_plane_t *u = calloc(1, sizeof *u);
    if (!u) return NULL;
    int w = t5_plane_w(plane), h = t5_plane_h(plane);
    u->plane = plane;
    u->under_bytes = (size_t)w * h * 2;
    u->under = t5_ui_hw_alloc(u->under_bytes);
    u->timer = u->under ? lv_timer_create(tick, 8, u) : NULL;
    if (!u->timer) {
        heap_caps_free(u->under);
        free(u);
        return NULL;
    }
    u->under_bytes = (u->under_bytes + 63) & ~(size_t)63;   /* t5_ui_hw_alloc rounds up to the cache line */
    memset(u->under, 0, u->under_bytes);
    u->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    u->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    u->dsc.header.w = (uint32_t)w;
    u->dsc.header.h = (uint32_t)h;
    u->dsc.header.stride = (uint32_t)w * 2;
    u->dsc.data_size = (uint32_t)w * h * 2;
    u->dsc.data = (const uint8_t *)u->under;
    t5_plane_hold(plane);
    t5_plane_set_end_cb(plane, ended, u);
    u->img = lv_image_create(parent);
    lv_obj_remove_style_all(u->img);
    lv_obj_set_clickable(u->img, false);
    lv_obj_set_size(u->img, w, h);
    lv_image_set_src(u->img, &u->dsc);
    lv_obj_add_event_cb(u->img, deleted, LV_EVENT_DELETE, u);
    u->next = s_planes;
    s_planes = u;
    return u->img;
}

/* ---- the display pipeline's side (ui.c) ---------------------------------------- */
/* Were all of LVGL's areas over the plane at c read back with this frame? */
static bool all_known(const ui_plane_t *u, const t5_rect_t *frame, int n, const lv_area_t *c)
{
    for (int i = 0; i < n; i++) {
        lv_area_t a = { frame[i].x1, frame[i].y1, frame[i].x2, frame[i].y2 }, x;
        if (!lv_area_intersect(&x, &a, c)) continue;
        t5_rect_t loc = { x.x1 - c->x1, x.y1 - c->y1, x.x2 - c->x1, x.y2 - c->y1 };
        bool seen = false;
        for (int j = 0; j < u->nknown && !seen; j++) {
            const t5_rect_t *k = &u->known[j];
            seen = loc.x1 >= k->x1 && loc.y1 >= k->y1 && loc.x2 <= k->x2 && loc.y2 <= k->y2;
        }
        if (!seen) return false;
    }
    return true;
}

static bool overlaps(const t5_rect_t *frame, int n, const lv_area_t *c)
{
    for (int i = 0; i < n; i++) {
        lv_area_t a = { frame[i].x1, frame[i].y1, frame[i].x2, frame[i].y2 };
        if (lv_area_is_on(&a, c)) return true;
    }
    return false;
}

int ui_planes_begin(uint16_t *fb, t5_rotation_t rot, t5_rect_t *frame, int n_lv, int max, uint32_t *bytes, bool *wait)
{
    int n = n_lv;
    for (ui_plane_t *u = s_planes; u; u = u->next) {
        if (!u->have) continue;
        lv_area_t c;
        bool fast = fast_ok(u, &c);
        if (!fast) lv_obj_get_coords(u->img, &c);
        bool under_it = overlaps(frame, n_lv, &c);
        if (!u->dirty && !under_it) continue;
        if (!fast || !u->dirty) {
            /* No new frame: the framebuffers have the picture (the target
             * is caught up from the newest), and what lies over it isn't
             * all being redrawn, so it isn't drawn again here. LVGL draws
             * its areas from the underlay: right where this frame was read
             * back, else filled whole once. (A new frame on the slow path:
             * the tick draws it.) */
            if (under_it && !u->under_full && !(fast && all_known(u, frame, n_lv, &c))) draw_under(u);
            continue;
        }
        t5_rect_t r;
        if (n >= max || !landed(u, &r)) {
            u->dirty = false;
            continue;
        }
        int pw = t5_plane_w(u->plane), ph = t5_plane_h(u->plane);
        t5_rect_t gaps[4];
        int ng = uncovered(r, pw, ph, gaps);
        for (int i = 0; i < ng; i++) {
            t5_rect_t g = { c.x1 + gaps[i].x1, c.y1 + gaps[i].y1, c.x1 + gaps[i].x2, c.y1 + gaps[i].y2 };
            t5_ui_hw_fill_black(fb, T5_PANEL_W, T5_PANEL_H, (size_t)T5_PANEL_W * T5_PANEL_H * 2,
                                t5_rot_rect_to_panel(rot, g));
        }
        t5_rect_t dst = { c.x1 + r.x1, c.y1 + r.y1, c.x1 + r.x2, c.y1 + r.y2 };
        if (t5_ui_hw_plane_async(&u->cur, dst, fb, rot) != T5_OK) {
            T5_LOGW(TAG, "plane frame failed: drawing it through LVGL");
            u->dirty = false;
            continue;
        }
        frame[n++] = (t5_rect_t){ c.x1, c.y1, c.x2, c.y2 };
        *bytes += (uint32_t)pw * (uint32_t)ph * 2;
        if (!under_it) u->nknown = 0;    /* nothing read back for this frame */
        /* What LVGL is about to draw over it needs the picture under it. */
        if (under_it && !u->under_full) {
            t5_rect_t known[8];
            int nk = 0;
            for (int i = 0; i < n_lv; i++) {
                lv_area_t a = { frame[i].x1, frame[i].y1, frame[i].x2, frame[i].y2 }, x;
                if (!lv_area_intersect(&x, &a, &c)) continue;
                t5_rect_t xr = { x.x1, x.y1, x.x2, x.y2 };
                t5_rect_t loc = { x.x1 - c.x1, x.y1 - c.y1, x.x2 - c.x1, x.y2 - c.y1 };
                bool seen = false;
                for (int j = 0; j < u->nknown && !seen; j++) {
                    const t5_rect_t *k = &u->known[j];
                    seen = loc.x1 >= k->x1 && loc.y1 >= k->y1 && loc.x2 <= k->x2 && loc.y2 <= k->y2;
                }
                if (t5_ui_hw_readback_async(fb, xr, rot, u->under, pw, ph, u->under_bytes, loc.x1, loc.y1) != T5_OK)
                    seen = false;
                if (!seen) *wait = true;
                if (nk < 8) known[nk++] = loc;
                else *wait = true;
            }
            memcpy(u->known, known, sizeof known[0] * (size_t)nk);
            u->nknown = nk;
            lv_image_cache_drop(&u->dsc);
        }
        if (u->dirty) s_st.fast++;
        u->dirty = false;
    }
    return n;
}
