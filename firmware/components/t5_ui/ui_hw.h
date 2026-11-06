/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"
#include "t5_ui/rotation.h"

t5_err_t t5_ui_hw_init(void);
/* PSRAM, aligned for the PPA (cache line). */
void *t5_ui_hw_alloc(size_t bytes);
/* Rotate src_rect of a src_w x src_h RGB565 picture and write it into the
 * 720x1280 panel framebuffer fb where the logical rectangle dst (same size)
 * lands. Blocking. client 0 = renderer, 1 = watchdog overlay (separate PPA
 * clients for separate tasks). */
t5_err_t t5_ui_hw_copy(int client, const uint16_t *src, int32_t src_w, int32_t src_h, t5_rect_t src_rect,
                       t5_rect_t dst, uint16_t *fb, t5_rotation_t rot);
