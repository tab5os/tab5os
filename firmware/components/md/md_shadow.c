/* SPDX-License-Identifier: Apache-2.0
 * Baked nine-slice shadows (plan 4.4, design "Rendering budget" rule 1).
 *
 * Each shadow image (tools/assets/shadows.py) holds the three MD2 layers
 * composited and dithered. We draw its four corners unscaled and tile the
 * 4 px centre bands along the edges, so the cost is a few small image
 * blits, never a live blur. The surface is drawn on top and hides the
 * transparent middle.
 */
#include <stdlib.h>
#include <string.h>
#include "md/md.h"
#include "md/md_shadows.h"
#include "md_priv.h"
#include "md_zasset.h"

/* Nine sub-images sharing the shadow's pixels. */
typedef struct slices {
    const md_shadow_t *sh;
    lv_image_dsc_t part[9];
    struct slices *next;
} slices_t;

static slices_t *s_cache;

static void sub(lv_image_dsc_t *out, const lv_image_dsc_t *img, int x, int y, int w, int h)
{
    *out = *img;
    out->header.w = (uint32_t)w;
    out->header.h = (uint32_t)h;
    uint32_t bpp = lv_color_format_get_size((lv_color_format_t)img->header.cf);
    out->data = img->data + (size_t)y * img->header.stride + (size_t)x * bpp;
    /* LVGL checks stride x height; the tail past the last row's pixels is
     * never read. */
    out->data_size = (uint32_t)h * img->header.stride;
}

static const slices_t *slices_for(const md_shadow_t *sh)
{
    for (slices_t *s = s_cache; s; s = s->next) {
        if (s->sh == sh) return s;
    }
    slices_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->sh = sh;
    const lv_image_dsc_t *img = sh->img;
    int W = (int)img->header.w, H = (int)img->header.h;
    int xs[4] = { 0, sh->left, W - sh->right, W };
    int ys[4] = { 0, sh->top, H - sh->bottom, H };
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            int w = xs[c + 1] - xs[c], h = ys[r + 1] - ys[r];
            if (w > 0 && h > 0) sub(&s->part[r * 3 + c], img, xs[c], ys[r], w, h);
        }
    }
    s->next = s_cache;
    s_cache = s;
    return s;
}

static void draw_part(lv_layer_t *layer, const lv_image_dsc_t *src, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                      bool tile)
{
    if (!src->data || x2 < x1 || y2 < y1) return;
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = src;
    d.recolor = lv_color_black();   /* A8 shadows are a mask filled with the recolour */
    d.recolor_opa = LV_OPA_COVER;
    d.tile = tile;
    lv_area_t a = { x1, y1, x2, y2 };
    if (!tile) {
        a.x2 = x1 + (int32_t)src->header.w - 1;
        a.y2 = y1 + (int32_t)src->header.h - 1;
    }
    lv_draw_image(layer, &d, &a);
}

/* Draw img with its top-left at (x, y), only inside (cx1, cy1)-(cx2, cy2). */
static void draw_clipped(lv_layer_t *layer, const lv_image_dsc_t *img, int32_t x, int32_t y, int32_t cx1, int32_t cy1,
                         int32_t cx2, int32_t cy2)
{
    lv_area_t saved = layer->_clip_area;
    lv_area_t clip = { LV_MAX(saved.x1, cx1), LV_MAX(saved.y1, cy1), LV_MIN(saved.x2, cx2), LV_MIN(saved.y2, cy2) };
    if (clip.x1 > clip.x2 || clip.y1 > clip.y2) return;
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = img;
    d.recolor = lv_color_black();
    d.recolor_opa = LV_OPA_COVER;
    lv_area_t a = { x, y, x + (int32_t)img->header.w - 1, y + (int32_t)img->header.h - 1 };
    layer->_clip_area = clip;
    lv_draw_image(layer, &d, &a);
    layer->_clip_area = saved;
}

typedef struct {
    int dp;
} shadow_ud_t;

static const md_shadow_t *pick(lv_obj_t *obj, int dp)
{
    int32_t rad = lv_obj_get_style_radius(obj, 0);
    int32_t m = LV_MIN(lv_obj_get_width(obj), lv_obj_get_height(obj)) / 2;
    int rdp = (rad == LV_RADIUS_CIRCLE || rad >= m) ? 100 : (int)(rad / MD_PX_PER_DP);
    md_assets_load();
    return md_shadow_get(dp, rdp);
}

static void draw_shadow(lv_layer_t *layer, lv_obj_t *obj, const md_shadow_t *sh);

static void shadow_event(lv_event_t *e)
{
    shadow_ud_t *u = lv_event_get_user_data(e);
    lv_obj_t *obj = lv_event_get_target_obj(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        free(u);
        return;
    }
    const md_shadow_t *sh = pick(obj, u->dp);
    if (!sh) return;
    if (code == LV_EVENT_REFR_EXT_DRAW_SIZE) {
        int32_t *s = lv_event_get_param(e);
        int32_t need = LV_MAX(LV_MAX(sh->box_x, sh->box_y),
                              LV_MAX((int32_t)sh->img->header.w - sh->box_x - sh->box_min_w,
                                     (int32_t)sh->img->header.h - sh->box_y - sh->box_min_h));
        if (*s < need) *s = need;
        return;
    }
    if (code != LV_EVENT_DRAW_MAIN_BEGIN) return;
    draw_shadow(lv_event_get_layer(e), obj, sh);
}

/* The nine-slice shadow of `sh` around obj's current coordinates. */
static void draw_shadow(lv_layer_t *layer, lv_obj_t *obj, const md_shadow_t *sh)
{
    lv_area_t co;
    lv_obj_get_coords(obj, &co);
    int32_t w = lv_area_get_width(&co), h = lv_area_get_height(&co);
    if (w < sh->box_min_w || h < sh->box_min_h) return;
    const slices_t *s = slices_for(sh);
    if (!s) return;
    int32_t W = (int32_t)sh->img->header.w + (w - sh->box_min_w);
    int32_t H = (int32_t)sh->img->header.h + (h - sh->box_min_h);
    int32_t X = co.x1 - sh->box_x, Y = co.y1 - sh->box_y;
    int32_t xm1 = X + sh->left, xm2 = X + W - sh->right - 1;   /* tiled middle columns */
    int32_t ym1 = Y + sh->top, ym2 = Y + H - sh->bottom - 1;
    /* Corners: the whole image, placed at each corner and clipped to it. */
    int32_t iw = (int32_t)sh->img->header.w, ih = (int32_t)sh->img->header.h;
    draw_clipped(layer, sh->img, X, Y, X, Y, xm1 - 1, ym1 - 1);
    draw_clipped(layer, sh->img, X + W - iw, Y, xm2 + 1, Y, X + W - 1, ym1 - 1);
    draw_clipped(layer, sh->img, X, Y + H - ih, X, ym2 + 1, xm1 - 1, Y + H - 1);
    draw_clipped(layer, sh->img, X + W - iw, Y + H - ih, xm2 + 1, ym2 + 1, X + W - 1, Y + H - 1);
    /* Edges, tiled. The centre is under the surface; skip it. */
    draw_part(layer, &s->part[1], xm1, Y, xm2, ym1 - 1, true);
    draw_part(layer, &s->part[7], xm1, ym2 + 1, xm2, Y + H - 1, true);
    draw_part(layer, &s->part[3], X, ym1, xm1 - 1, ym2, true);
    draw_part(layer, &s->part[5], xm2 + 1, ym1, X + W - 1, ym2, true);
}

void md_shadow_draw(lv_layer_t *layer, lv_obj_t *obj, int dp)
{
    const md_shadow_t *sh = dp > 0 ? pick(obj, dp) : NULL;
    if (sh) draw_shadow(layer, obj, sh);
}

int md_shadow_dp(lv_obj_t *obj)
{
    uint32_t n = lv_obj_get_event_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_event_dsc_t *d = lv_obj_get_event_dsc(obj, i);
        if (lv_event_dsc_get_cb(d) == shadow_event) return ((shadow_ud_t *)lv_event_dsc_get_user_data(d))->dp;
    }
    return 0;
}

void md_shadow_attach(lv_obj_t *obj, int dp)
{
    uint32_t n = lv_obj_get_event_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_event_dsc_t *d = lv_obj_get_event_dsc(obj, i);
        if (lv_event_dsc_get_cb(d) == shadow_event) {
            shadow_ud_t *u = lv_event_dsc_get_user_data(d);
            u->dp = dp;
            lv_obj_refresh_ext_draw_size(obj);
            lv_obj_invalidate(obj);
            return;
        }
    }
    if (dp <= 0) return;
    shadow_ud_t *u = calloc(1, sizeof *u);
    if (!u) return;
    u->dp = dp;
    lv_obj_add_event_cb(obj, shadow_event, LV_EVENT_ALL, u);
    lv_obj_refresh_ext_draw_size(obj);
}

void md_shadow_scheme_changed(void)
{
    /* Shadows are drawn from the same images on dark and light; nothing to
     * rebuild. Kept as the hook for a light-tuned set later. */
}

void md_set_elevation(lv_obj_t *obj, int dp, bool shadow)
{
    for (int i = 0; i < MD_ELEV_LEVELS; i++) {
        lv_obj_remove_style(obj, md_style_elevation(md_elev_levels[i]), 0);
    }
    lv_obj_add_style(obj, md_style_elevation(dp), 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    md_shadow_attach(obj, (shadow || !md_is_dark()) ? dp : 0);
}
