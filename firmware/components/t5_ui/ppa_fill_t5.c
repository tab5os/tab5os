/* SPDX-License-Identifier: Apache-2.0
 * Translucent fills on the PPA.
 *
 * LVGL's PPA unit only takes opaque fills, and opaque fills are faster on
 * the CPU anyway (ui.c, "PPA fills"). A translucent fill is different: the
 * CPU reads, mixes and writes every pixel (~80 ns a pixel measured, a
 * full-screen dialog scrim ~75 ms), while the PPA's blender does it in
 * hardware. So a translucent, square-cornered, ungradiented fill big enough
 * to beat the PPA's set-up goes to the blender: the band is the background
 * and the colour the foreground, at a fixed alpha of the fill's opacity.
 * The linker wraps lv_draw_ppa_fill (CMakeLists.txt); ui.c's evaluate hook
 * claims these fills for the PPA unit (t5_ui_ppa_fill_alpha_ok).
 */
#include "lvgl.h"
#include "draw/espressif/ppa/lv_draw_ppa_private.h"
#include "draw/sw/lv_draw_sw.h"
#include "ui_hw.h"
#include "esp_timer.h"

volatile uint32_t t5_ui_ppa_draw_us, t5_ui_ppa_draw_n;

/* Below this many pixels the PPA's set-up and cache maintenance cost more
 * than the CPU's blend. */
#define MIN_PX 4096

void __real_lv_draw_ppa_fill(lv_draw_task_t *t, const lv_draw_fill_dsc_t *dsc, const lv_area_t *coords);

bool t5_ui_ppa_fill_alpha_ok(const lv_draw_task_t *t)
{
    const lv_draw_fill_dsc_t *dsc = t->draw_dsc;
    lv_color_format_t dest = dsc->base.layer->color_format;
    /* Rounded too: the corners only touch the top and bottom `radius` rows,
     * which stay in software; the rows between go to the blender. */
    return dsc->opa > LV_OPA_MIN && dsc->opa < LV_OPA_MAX && dsc->radius >= 0 &&
           dsc->grad.dir == LV_GRAD_DIR_NONE && (dest == LV_COLOR_FORMAT_RGB565 || dest == LV_COLOR_FORMAT_RGB888) &&
           lv_area_get_size(&t->area) >= MIN_PX;
}

void __wrap_lv_draw_ppa_fill(lv_draw_task_t *t, const lv_draw_fill_dsc_t *dsc, const lv_area_t *coords)
{
    if (dsc->opa >= LV_OPA_MAX) {
        __real_lv_draw_ppa_fill(t, dsc, coords);
        return;
    }
    if (dsc->opa <= LV_OPA_MIN) return;
    /* coords is the fill already clipped to this band (lv_draw_ppa.c); the
     * corners belong to the whole rectangle, t->area. Rounding the clipped
     * part gave every band's slice corners of its own. */
    const lv_area_t *full = &t->area;
    lv_area_t mid = *full;
    if (dsc->radius > 0) {
        int32_t w = lv_area_get_width(full), h = lv_area_get_height(full);
        int32_t r = LV_MIN(dsc->radius, LV_MIN(w, h) / 2);
        /* The corner rows in software, each clipped to its strip. */
        lv_area_t saved = t->clip_area, strip;
        lv_area_t top = { full->x1, full->y1, full->x2, full->y1 + r - 1 };
        lv_area_t bottom = { full->x1, full->y2 - r + 1, full->x2, full->y2 };
        const lv_area_t *strips[2] = { &top, &bottom };
        for (int i = 0; i < 2 && r > 0; i++) {
            if (!lv_area_intersect(&strip, &saved, strips[i]) || !lv_area_intersect(&strip, &strip, coords)) continue;
            t->clip_area = strip;
            lv_draw_sw_fill(t, (lv_draw_fill_dsc_t *)dsc, full);
            /* Out of the cache before the PPA unit invalidates the task's
             * rows (lv_draw_ppa.c), or these pixels are lost. */
            lv_area_move(&strip, -t->target_layer->buf_area.x1, -t->target_layer->buf_area.y1);
            lv_draw_buf_flush_cache(t->target_layer->draw_buf, &strip);
        }
        t->clip_area = saved;
        mid.y1 += r;
        mid.y2 -= r;
        if (mid.y1 > mid.y2) return;
    }
    if (!lv_area_intersect(&mid, &mid, coords)) return;   /* back to this band */
    coords = &mid;
    lv_draw_fill_dsc_t sq = *dsc;   /* the rows between the corners: square */
    sq.radius = 0;
    lv_draw_ppa_unit_t *u = (lv_draw_ppa_unit_t *)t->draw_unit;
    lv_draw_buf_t *buf = t->target_layer->draw_buf;
    lv_area_t rel = *coords, clip = t->clip_area, area;
    lv_area_move(&rel, -t->target_layer->buf_area.x1, -t->target_layer->buf_area.y1);
    lv_area_move(&clip, -t->target_layer->buf_area.x1, -t->target_layer->buf_area.y1);
    if (!lv_area_intersect(&area, &rel, &clip)) return;

    lv_color_format_t cf = buf->header.cf;
    int32_t w = lv_ppa_pic_w(buf->header.stride, buf->header.w, cf);
    if (w == 0) {
        u->img_sw_fallback = true;   /* software wrote it: no cache invalidate after */
        lv_draw_sw_fill(t, &sq, coords);
        return;
    }
    uint32_t bw = (uint32_t)lv_area_get_width(&area), bh = (uint32_t)lv_area_get_height(&area);
    ppa_blend_oper_config_t cfg = {
        .in_bg = {
            .buffer = buf->data, .pic_w = (uint32_t)w, .pic_h = buf->header.h, .block_w = bw, .block_h = bh,
            .block_offset_x = (uint32_t)area.x1, .block_offset_y = (uint32_t)area.y1,
            .blend_cm = lv_color_format_to_ppa_blend(cf),
        },
        .bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE,
        .bg_alpha_fix_val = 0xFF,
        /* The foreground is a fixed colour at a fixed alpha: the A8 buffer is
         * only read for its (replaced) alpha, so the band itself serves, as
         * in LVGL's own image copy. */
        .in_fg = {
            .buffer = buf->data, .pic_w = (uint32_t)w, .pic_h = buf->header.h, .block_w = bw, .block_h = bh,
            .block_offset_x = (uint32_t)area.x1, .block_offset_y = (uint32_t)area.y1,
            .blend_cm = PPA_BLEND_COLOR_MODE_A8,
        },
        .fg_fix_rgb_val = { .r = dsc->color.red, .g = dsc->color.green, .b = dsc->color.blue },
        .fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE,
        .fg_alpha_fix_val = dsc->opa,
        .out = {
            .buffer = buf->data, .buffer_size = buf->data_size, .pic_w = (uint32_t)w, .pic_h = buf->header.h,
            .block_offset_x = (uint32_t)area.x1, .block_offset_y = (uint32_t)area.y1,
            .blend_cm = lv_color_format_to_ppa_blend(cf),
        },
        .mode = PPA_TRANS_MODE_BLOCKING,
        .user_data = u,
    };
    int64_t t0 = esp_timer_get_time();
    if (ppa_do_blend(u->blend_client, &cfg) != ESP_OK) {
        u->img_sw_fallback = true;
        lv_draw_sw_fill(t, &sq, coords);
    }
    t5_ui_ppa_draw_us += (uint32_t)(esp_timer_get_time() - t0);
    t5_ui_ppa_draw_n++;
}
