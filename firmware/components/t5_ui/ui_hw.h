/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"
#include "t5_core/plane.h"
#include "t5_ui/rotation.h"

#include "lvgl.h"

/* ppa_img_t5.c: may this image (faded, or with alpha) go to the PPA? */
bool t5_ui_ppa_img_alpha_ok(const lv_draw_image_dsc_t *dsc);
bool t5_ui_ppa_fill_alpha_ok(const lv_draw_task_t *t);
/* Time spent blocked in the PPA draw paths (fills, alpha images), for the
 * frame log. */
extern volatile uint32_t t5_ui_ppa_draw_us, t5_ui_ppa_draw_n;
extern volatile uint32_t t5_ui_copy_exec_us, t5_ui_copy_n;   /* async copies: PPA time */

t5_err_t t5_ui_hw_init(void);
/* PSRAM, aligned for the PPA (cache line). */
void *t5_ui_hw_alloc(size_t bytes);
/* Internal RAM, DMA-capable, same alignment; NULL if it doesn't fit. */
void *t5_ui_hw_alloc_internal(size_t bytes);
/* Rotate src_rect of a src_w x src_h RGB565 picture and write it into the
 * 720x1280 panel framebuffer fb where the logical rectangle dst (same size)
 * lands. Blocking. client 0 = renderer, 1 = watchdog overlay (separate PPA
 * clients for separate tasks). */
t5_err_t t5_ui_hw_copy(int client, const uint16_t *src, int32_t src_w, int32_t src_h, t5_rect_t src_rect,
                       t5_rect_t dst, uint16_t *fb, t5_rotation_t rot);

/* ---- the pipelined renderer (async, client 2) --------------------------------
 * Transactions run in submission order. done(user) is called from the PPA's
 * interrupt for each one submitted with a non-NULL user. */
typedef void (*t5_ui_hw_done_t)(void *user);
void t5_ui_hw_set_done_cb(t5_ui_hw_done_t cb);
/* Rotate a band (w x h at stride_px) into fb where the logical rectangle dst
 * lands. */
t5_err_t t5_ui_hw_band_async(const uint16_t *src, int32_t stride_px, t5_rect_t dst, uint16_t *fb, t5_rotation_t rot,
                             void *user);
/* Rotate the block a of a src_w x src_h RGB565 picture into fb where the
 * logical rectangle dst (the same size) lands. */
t5_err_t t5_ui_hw_blit_async(const uint16_t *src, int32_t src_w, int32_t src_h, t5_rect_t a, t5_rect_t dst,
                             uint16_t *fb, t5_rotation_t rot, void *user);
/* A video plane's frame (t5_core/plane.h) into fb where the logical
 * rectangle dst (the frame's landed size) lands, turned for the panel. */
t5_err_t t5_ui_hw_plane_async(const t5_plane_src_t *s, t5_rect_t dst, uint16_t *fb, t5_rotation_t rot);
/* The same into a logical RGB565 picture at the frame's (dx, dy).
 * Blocking, renderer client. */
t5_err_t t5_ui_hw_plane_to(const t5_plane_src_t *s, uint16_t *dst, int32_t dst_w, int32_t dst_h, size_t dst_bytes);
/* The logical rectangle a of fb, turned back upright, into dst at (dx, dy). */
t5_err_t t5_ui_hw_readback_async(const uint16_t *fb, t5_rect_t a, t5_rotation_t rot, uint16_t *dst, int32_t dst_w,
                                 int32_t dst_h, size_t dst_bytes, int32_t dx, int32_t dy);
/* Black into the rectangle r of a pic_w x pic_h RGB565 picture. Blocking. */
t5_err_t t5_ui_hw_fill_black(uint16_t *buf, int32_t pic_w, int32_t pic_h, size_t bytes, t5_rect_t r);
/* Copy a panel-space rectangle between framebuffers. */
t5_err_t t5_ui_hw_fb_copy_async(const uint16_t *src_fb, uint16_t *dst_fb, t5_rect_t panel, void *user);
/* Wait until every async transaction has finished. */
bool t5_ui_hw_wait_idle(uint32_t timeout_ms);
bool t5_ui_hw_busy(void);   /* async copies still queued or running */

