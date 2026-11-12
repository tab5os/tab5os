/* SPDX-License-Identifier: Apache-2.0
 * Showing a video plane (t5_core/plane.h) on screen.
 *
 * The plane is an LVGL image object the size of the plane, laid out like
 * any other. Drawing a full-screen picture through LVGL costs a render and
 * a band copy per frame (~100 ms at 1280x720, bench Phase 4/7), on top of
 * converting it into a buffer first. Instead the display pipeline takes
 * each posted frame from the producer's own buffer and scales, converts
 * and turns it into the framebuffer in one PPA pass. With nothing over the
 * picture that is a frame of its own; with objects over it (buttons,
 * captions) LVGL redraws just those, over the parts of the picture under
 * them, so they blend correctly.
 *
 * That fast path needs the image unscaled, unrotated, opaque, wholly on
 * screen, and on the active screen with no screen animation running;
 * otherwise the frame is drawn through LVGL like any image. Objects over
 * the picture should be small: the larger they are, the more LVGL draws
 * per frame (a full-screen scrim over it makes every frame a full redraw).
 */
#pragma once
#include "lvgl.h"
#include "t5_core/plane.h"

#ifdef __cplusplus
extern "C" {
#endif

/* UI task. An image showing plane in parent, the plane's size, with no
 * styles (position it as you like). It holds a reference to the plane
 * until it is deleted, then closes it (t5_plane_closed). */
lv_obj_t *t5_ui_plane_create(lv_obj_t *parent, t5_plane_t *plane);

typedef struct {
    uint32_t fast;      /* frames drawn straight into the framebuffer */
    uint32_t slow;      /* frames drawn through LVGL */
} t5_ui_plane_stats_t;
/* Totals over every plane since boot. */
void t5_ui_plane_stats(t5_ui_plane_stats_t *out);

#ifdef __cplusplus
}
#endif
