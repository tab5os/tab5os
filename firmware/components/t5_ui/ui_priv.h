/* SPDX-License-Identifier: Apache-2.0
 * Shared between t5_ui's files (ui_md.c also by the simulator's t5_ui).
 */
#pragma once
#include "t5_core/err.h"

/* hid_input.c: USB keyboards and mice as LVGL input devices. */
t5_err_t ui_hid_input_init(void);
/* ui_md.c: md's ripple and settled probes. */
void ui_md_probes_init(void);

#ifdef ESP_PLATFORM
#include <stdint.h>
#include "t5_ui/rotation.h"
/* plane.c: video planes drawn straight into the framebuffer. At the start
 * of a frame whose first n_lv damage areas LVGL is about to draw: queue the
 * planes that have a new frame or lie under those areas into fb, add their
 * rectangles to the damage (returns the new count) and their bytes; *wait
 * if LVGL must wait for the PPA before drawing (it reads parts back). */
int ui_planes_begin(uint16_t *fb, t5_rotation_t rot, t5_rect_t *frame, int n_lv, int max, uint32_t *bytes, bool *wait);
/* ui.c: a frame with nothing for LVGL to draw, for a plane's new frame
 * (UI task, between frames). */
void ui_plane_present(void);
#endif
