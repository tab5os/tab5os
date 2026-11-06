/* SPDX-License-Identifier: Apache-2.0
 * Damage tracking for the triple-buffered display (plan 4.2). Pure logic.
 *
 * LVGL renders into one logical canvas. Each of the three panel framebuffers
 * lags the canvas by whatever changed since that buffer was last written, so
 * every new damage rectangle is added to all three lists, and writing a
 * buffer takes (and clears) its own list. Lists merge overlapping or nearby
 * rectangles and fall back to the bounding box when full, so the copy work
 * stays bounded.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "t5_ui/rotation.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_FBT_BUFS 3
#define T5_FBT_RECTS 12

typedef struct {
    t5_rect_t r[T5_FBT_RECTS];
    int n;
} t5_fbt_list_t;

typedef struct {
    t5_rect_t bounds;                 /* the whole canvas */
    t5_fbt_list_t list[T5_FBT_BUFS];
} t5_fbt_t;

void t5_fbt_init(t5_fbt_t *t, int32_t w, int32_t h);   /* every buffer fully dirty */
void t5_fbt_add(t5_fbt_t *t, t5_rect_t r);              /* clipped to bounds */
void t5_fbt_all_dirty(t5_fbt_t *t);
/* Move buffer i's damage into out and clear it. Returns the count. */
int t5_fbt_take(t5_fbt_t *t, int i, t5_rect_t *out, int max);
/* Pixels covered by buffer i's list (overlaps counted twice; an estimate). */
uint32_t t5_fbt_pixels(const t5_fbt_t *t, int i);

/* Pick a buffer to draw into: never the one scanning out, nor the one
 * already queued for the next frame (it may become scanout any moment).
 * Returns -1 only if scanout == queued is impossible to satisfy (n < 2). */
int t5_fbt_pick(int nbufs, int scanout, int queued);

#ifdef __cplusplus
}
#endif
