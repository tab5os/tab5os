/* SPDX-License-Identifier: Apache-2.0
 * Images with alpha on the PPA.
 *
 * LVGL's PPA unit (lv_draw_ppa_img.c) only takes opaque RGB565/RGB888
 * images: it copies them with a blend whose foreground is fully clear. So
 * anything faded (a screen snapshot under a dialog's scrim, a fading
 * snapshot of the dialog itself) or with an alpha channel (ARGB8888) falls
 * back to a software blend of every pixel, ~40 ms a full-screen frame.
 *
 * The PPA's blender does real alpha blending: here the band is the
 * background and the image the foreground, with the image's alpha fixed at
 * the draw opacity (RGB565/RGB888) or its own alpha scaled by it
 * (ARGB8888). The linker wraps lv_draw_ppa_img (CMakeLists.txt) so these
 * images come here and the rest go on to LVGL's; ui.c's evaluate hook
 * claims them for the PPA unit (t5_ui_ppa_img_alpha_ok).
 */
#include "lvgl.h"
#include "draw/espressif/ppa/lv_draw_ppa_private.h"
#include "draw/lv_draw_image_private.h"
#include "image/lv_image_decoder_private.h"
#include "draw/sw/lv_draw_sw.h"
#include "ui_hw.h"
#include "esp_timer.h"

void __real_lv_draw_ppa_img(lv_draw_task_t *t, const lv_draw_image_dsc_t *dsc, const lv_area_t *coords);

static bool needs_alpha(const lv_draw_image_dsc_t *dsc)
{
    return dsc->opa < LV_OPA_MAX || dsc->header.cf == LV_COLOR_FORMAT_ARGB8888;
}

bool t5_ui_ppa_img_alpha_ok(const lv_draw_image_dsc_t *dsc)
{
    lv_color_format_t cf = dsc->header.cf, dest = dsc->base.layer->color_format;
    return needs_alpha(dsc) && dsc->opa > LV_OPA_MIN &&
           (cf == LV_COLOR_FORMAT_RGB565 || cf == LV_COLOR_FORMAT_RGB888 || cf == LV_COLOR_FORMAT_ARGB8888) &&
           (dest == LV_COLOR_FORMAT_RGB565 || dest == LV_COLOR_FORMAT_RGB888) && dsc->clip_radius == 0 &&
           dsc->bitmap_mask_src == NULL && dsc->sup == NULL && dsc->tile == 0 &&
           dsc->blend_mode == LV_BLEND_MODE_NORMAL && dsc->recolor_opa <= LV_OPA_MIN && dsc->skew_x == 0 &&
           dsc->skew_y == 0 && dsc->scale_x == LV_SCALE_NONE && dsc->scale_y == LV_SCALE_NONE &&
           dsc->rotation == 0 && lv_image_src_get_type(dsc->src) == LV_IMAGE_SRC_VARIABLE;
}

static void alpha_core(lv_draw_task_t *t, const lv_draw_image_dsc_t *dsc, const lv_image_decoder_dsc_t *decoder_dsc,
                       lv_draw_image_sup_t *sup, const lv_area_t *img_coords, const lv_area_t *clipped)
{
    (void)sup;
    lv_draw_buf_t *dest_buf = t->target_layer->draw_buf;
    const lv_draw_buf_t *src_buf = decoder_dsc->decoded;
    lv_draw_ppa_unit_t *u = (lv_draw_ppa_unit_t *)t->draw_unit;

    /* The clipped area relative to the image, and to the layer's buffer. */
    lv_area_t src = *clipped, dst = *clipped;
    lv_area_move(&src, -img_coords->x1, -img_coords->y1);
    lv_area_move(&dst, -t->target_layer->buf_area.x1, -t->target_layer->buf_area.y1);

    lv_color_format_t src_cf = dsc->header.cf, dest_cf = dest_buf->header.cf;
    int32_t src_w = lv_ppa_pic_w(src_buf->header.stride, dsc->header.w, src_cf);
    int32_t dest_w = lv_ppa_pic_w(dest_buf->header.stride, dest_buf->header.w, dest_cf);
    uint32_t bw = (uint32_t)lv_area_get_width(clipped), bh = (uint32_t)lv_area_get_height(clipped);
    if (src_w == 0 || dest_w == 0) {
        lv_draw_sw_image(t, dsc, &t->area);
        return;
    }

    ppa_blend_oper_config_t cfg = {
        .in_bg = {
            .buffer = dest_buf->data,
            .pic_w = (uint32_t)dest_w,
            .pic_h = dest_buf->header.h,
            .block_w = bw,
            .block_h = bh,
            .block_offset_x = (uint32_t)dst.x1,
            .block_offset_y = (uint32_t)dst.y1,
            .blend_cm = lv_color_format_to_ppa_blend(dest_cf),
        },
        .bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE,
        .bg_alpha_fix_val = 0xFF,
        .in_fg = {
            .buffer = src_buf->data,
            .pic_w = (uint32_t)src_w,
            .pic_h = dsc->header.h,
            .block_w = bw,
            .block_h = bh,
            .block_offset_x = (uint32_t)src.x1,
            .block_offset_y = (uint32_t)src.y1,
            .blend_cm = lv_color_format_to_ppa_blend(src_cf),
        },
        .out = {
            .buffer = dest_buf->data,
            .buffer_size = dest_buf->data_size,
            .pic_w = (uint32_t)dest_w,
            .pic_h = dest_buf->header.h,
            .block_offset_x = (uint32_t)dst.x1,
            .block_offset_y = (uint32_t)dst.y1,
            .blend_cm = lv_color_format_to_ppa_blend(dest_cf),
        },
        .mode = PPA_TRANS_MODE_BLOCKING,
        .user_data = u,
    };
    if (src_cf != LV_COLOR_FORMAT_ARGB8888) {
        cfg.fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
        cfg.fg_alpha_fix_val = dsc->opa;
    } else if (dsc->opa >= LV_OPA_MAX) {
        cfg.fg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
    } else {
        cfg.fg_alpha_update_mode = PPA_ALPHA_SCALE;
        cfg.fg_alpha_scale_ratio = (float)dsc->opa / 255.0f;
    }
    int64_t t0 = esp_timer_get_time();
    if (ppa_do_blend(u->blend_client, &cfg) != ESP_OK) lv_draw_sw_image(t, dsc, &t->area);
    t5_ui_ppa_draw_us += (uint32_t)(esp_timer_get_time() - t0);
    t5_ui_ppa_draw_n++;
}

void __wrap_lv_draw_ppa_img(lv_draw_task_t *t, const lv_draw_image_dsc_t *dsc, const lv_area_t *coords)
{
    if (!needs_alpha(dsc)) {
        __real_lv_draw_ppa_img(t, dsc, coords);
        return;
    }
    if (dsc->opa <= LV_OPA_MIN) return;
    lv_draw_image_normal_helper(t, dsc, coords, alpha_core, NULL);
}
