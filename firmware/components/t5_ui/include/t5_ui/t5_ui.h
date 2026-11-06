/* SPDX-License-Identifier: Apache-2.0
 * UI service: LVGL on the Tab5 (plan 4.2, 4.9).
 *
 * Threading (plan, "Gaps"): every LVGL call runs on the UI task (core 1).
 * App and shell callbacks run there too. Any other task must either post
 * work with t5_ui_async() or bracket LVGL calls with t5_ui_lock()/unlock().
 *
 * Pipeline: LVGL renders in RGB565 into one logical canvas (1280x720 in
 * landscape, 720x1280 in portrait), redrawing only what changed. At the end
 * of each refresh the damaged rectangles are rotated and copied by the PPA
 * into a free one of three panel framebuffers, which is then queued; the
 * panel switches at the end of a frame, so there is no tearing and the
 * renderer never waits for vsync.
 *
 * The simulator (sim/) implements this same header on SDL.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "t5_core/err.h"
#include "t5_ui/rotation.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start LVGL, the display pipeline, touch input and the UI task.
 * Needs t5_display_init() and t5_touch_init() first. */
t5_err_t t5_ui_init(void);
bool t5_ui_ready(void);
/* The display's backlight stays off from boot; this turns it on once the
 * next frame (with whatever the caller just put up) is on the panel. */
void t5_ui_reveal(void);

/* Recursive lock around LVGL for tasks other than the UI task. */
void t5_ui_lock(void);
bool t5_ui_lock_timeout(uint32_t ms);
void t5_ui_unlock(void);
bool t5_ui_on_ui_task(void);

/* Run fn(arg) on the UI task soon. Safe from any task. */
typedef void (*t5_ui_fn_t)(void *arg);
t5_err_t t5_ui_async(t5_ui_fn_t fn, void *arg);
/* Run fn(arg) on the UI task and wait for it (up to timeout_ms). */
t5_err_t t5_ui_sync(t5_ui_fn_t fn, void *arg, uint32_t timeout_ms);

/* ---- rotation (plan 4.9) ----------------------------------------------- */
t5_rotation_t t5_ui_rotation(void);
/* Rotate now (UI task or under the lock). Posts T5_EV_UI_ROTATE and calls
 * the rotate hook. */
t5_err_t t5_ui_set_rotation(t5_rotation_t r);
/* Auto-rotate from the accelerometer. Off keeps the current rotation. */
void t5_ui_set_auto_rotate(bool on);
bool t5_ui_auto_rotate(void);
/* Rotation lock (quick settings): persists across reboots. While locked,
 * auto-rotate does nothing. */
t5_err_t t5_ui_set_rotation_lock(bool locked);
bool t5_ui_rotation_locked(void);
/* Orientations the foreground app accepts (T5_ROT_* masks from orient.h). */
void t5_ui_set_allowed_rotations(uint32_t mask);
uint32_t t5_ui_allowed_rotations(void);
/* Called on the UI task after every rotation (the app runtime's on_rotate). */
typedef void (*t5_ui_rotate_hook_t)(t5_rotation_t r, void *arg);
void t5_ui_set_rotate_hook(t5_ui_rotate_hook_t fn, void *arg);
/* Map a raw touch point (panel pixels) to logical coordinates. */
void t5_ui_touch_to_logical(int32_t px, int32_t py, int32_t *lx, int32_t *ly);

/* ---- performance counters (plan 4.2) ------------------------------------ */
typedef struct {
    uint32_t frames;            /* refreshes that changed pixels */
    uint32_t panel_frames;      /* frames the panel scanned out */
    uint32_t fps;               /* refreshes in the last second */
    uint32_t render_us_avg;     /* LVGL render time per refresh, last second */
    uint32_t render_us_max;
    uint32_t copy_us_avg;       /* PPA rotate+copy per refresh, last second */
    uint32_t copy_kb_s;         /* framebuffer bytes written per second */
    uint32_t refresh_period_ms; /* LVGL refresh period (16, or 33 in low-power mode) */
    uint32_t ui_loop_max_ms;    /* longest UI task iteration, last second */
    uint32_t touch_to_flush_us; /* last touch press to the end of its refresh */
} t5_ui_stats_t;
void t5_ui_get_stats(t5_ui_stats_t *out);
/* On-screen overlay with the counters (sys layer, top right). */
void t5_ui_show_stats(bool on);

/* ---- low-power mode ------------------------------------------------------ */
/* Caps the refresh rate at 30 fps; md uses t5_ui_low_power() to drop ripples. */
void t5_ui_set_low_power(bool on);
bool t5_ui_low_power(void);

/* ---- watchdog support (plan 5.12) ---------------------------------------- */
/* Milliseconds since the UI task last finished an iteration (0 if idle-waiting). */
uint32_t t5_ui_stalled_ms(void);
/* While the UI task is stuck, draw a pre-rendered RGB565 image straight into
 * the framebuffer on screen at logical (x, y); restore() puts the last good
 * frame back. Both run outside LVGL and never take the UI lock. */
t5_err_t t5_ui_overlay_show(const uint16_t *rgb565, int32_t w, int32_t h, int32_t x, int32_t y);
t5_err_t t5_ui_overlay_restore(void);
/* The last touch, in logical coordinates (works while the UI task is stuck). */
bool t5_ui_touch_peek(int32_t *x, int32_t *y);

#ifdef __cplusplus
}
#endif
