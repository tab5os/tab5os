/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"
#include "t5_ui/rotation.h"

/* Whether LVGL's PPA unit takes solid fills (ui.c, "PPA fills"). Software
 * fills into the internal bands are faster: the PPA's row cache maintenance
 * and blocking round trip cost more than the fill (2026-11-06). */
#define T5_UI_PPA_FILLS false

/* ppa_img_t5.c: may this image (faded, or with alpha) go to the PPA? */
#include "lvgl.h"
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
/* Copy a panel-space rectangle between framebuffers. */
t5_err_t t5_ui_hw_fb_copy_async(const uint16_t *src_fb, uint16_t *dst_fb, t5_rect_t panel, void *user);
/* Wait until every async transaction has finished. */
bool t5_ui_hw_wait_idle(uint32_t timeout_ms);
bool t5_ui_hw_busy(void);   /* async copies still queued or running */

