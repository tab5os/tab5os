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
/* As t5_ui_sync, but it also runs while rendering is paused (Screen off and
 * Sleep), when t5_ui_sync would time out: for the power service, delivering
 * on_suspend. One at a time (T5_ERR_BUSY). No LVGL timers run around it.
 * After T5_ERR_TIMEOUT the call may still be running: arg must outlive it. */
t5_err_t t5_ui_call_paused(t5_ui_fn_t fn, void *arg, uint32_t timeout_ms);

/* The UI draws in a 100 KB internal RAM block. lend frees it (the UI draws
 * in PSRAM meanwhile, slower) for a driver that needs contiguous internal RAM,
 * such as the H.264 encoder; reclaim takes it back if it is free again. Both
 * block until done; callable from any task and before t5_ui_init. */
void t5_ui_lend_internal(void);
void t5_ui_reclaim_internal(void);

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
    uint32_t render_kpx_avg;    /* pixels redrawn per refresh (thousands), last second */
    uint32_t render_us_max;
    uint32_t copy_us_avg;       /* PPA rotate+copy per refresh, last second */
    uint32_t copy_kb_s;         /* framebuffer bytes written per second */
    uint32_t refresh_period_ms; /* LVGL refresh period (16, or 33 in low-power mode) */
    uint32_t ui_loop_max_ms;    /* longest UI task iteration, last second */
    uint32_t touch_to_flush_us; /* last touch press to the end of its refresh */
} t5_ui_stats_t;
void t5_ui_get_stats(t5_ui_stats_t *out);
/* The last frames, newest last: when each started (ms since boot), how long
 * LVGL rendered, how many pixels it redrew, and the first redrawn area.
 * For finding what a frame redraws (`ui frames`). Returns the count. */
typedef struct {
    uint32_t start_ms, render_us, px, nareas;
    uint32_t ppa_us, ppa_n;     /* blocked in PPA draws (translucent fills, alpha images) */
    uint32_t wait_us;           /* frame end: waiting for the last band copy */
    uint32_t flush_wait_us, flushes;   /* waiting for a free band; bands copied */
    uint32_t copy_exec_us, copies;     /* PPA time on this frame's async copies */
    int16_t x1, y1, x2, y2;
} t5_ui_frame_rec_t;
#define T5_UI_FRAME_LOG 32
int t5_ui_frame_log(t5_ui_frame_rec_t *out, int max);
/* Where LVGL's allocations land (lv_mem_t5.c): its internal pool, other
 * internal RAM, or PSRAM. Counts since the last reset; live and peak bytes
 * outside the pool. */
typedef struct {
    uint32_t pool_allocs, int_allocs, psram_allocs, failed;
    uint32_t big_allocs, big_max;      /* over the pool's request limit */
    uint32_t overflow_allocs;          /* small, but the pool was full */
    int32_t int_live, int_peak, psram_live, psram_peak;
    uint32_t pool_bytes, pool_free, pool_min_free;
} t5_ui_lvmem_t;
void t5_ui_lvmem(t5_ui_lvmem_t *out);
void t5_ui_lvmem_reset(void);
/* Integrity check of LVGL's pool (prints what it finds wrong). */
bool t5_ui_lvmem_check(void);
/* Test switch: LVGL's PPA unit takes solid fills (else software does). */
void t5_ui_set_ppa_fills(bool on);
bool t5_ui_ppa_fills(void);
void t5_ui_set_ppa_images(bool on);
void t5_ui_set_ppa_alpha(bool on);   /* faded/ARGB8888 images on the PPA (bench A/B) */
void t5_ui_set_ppa_alpha_fills(bool on);
/* LVGL's clock n times slower: animations in slow motion (1 = normal). */
void t5_ui_set_slowmo(uint32_t n);
/* Starts copying the newest frame into the framebuffers that lack it, and
 * returns true once they all have it (after a full-screen change: a few
 * vsyncs). For starting an animation after one. UI task only. */
bool t5_ui_settled(void);   /* translucent fills (scrims) on the PPA (bench A/B) */
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

/* ---- surviving app faults (t5_core/fault.h) ------------------------------- */
/* The UI task runs each loop iteration under a recovery point. When app
 * code it was running faults, or is stopped while hung, the iteration is
 * abandoned: LVGL's in-progress state is reset, the UI lock released, and
 * the hook runs (UI task, UI lock held) to get rid of the app. */
typedef void (*t5_ui_unwind_fn_t)(const void *fault_info);
void t5_ui_set_unwind_hook(t5_ui_unwind_fn_t fn);
/* The UI task (for t5_fault_interrupt_task), NULL before t5_ui_init. */
void *t5_ui_task_handle(void);
/* False while a finished frame is being handed to the panel: abandoning
 * that would leave the display's buffers in flight, so a fault there still
 * reboots. A frame still being drawn can be abandoned. */
bool t5_ui_unwind_safe(void);
/* Lets LVGL's allocator cope with blocks from another allocator (an app's
 * heap), reached by lv_free or lv_realloc: free returns true if the block
 * was one, realloc sets *handled. */
void t5_ui_lv_set_foreign(bool (*free_fn)(void *p), void *(*realloc_fn)(void *p, size_t n, bool *handled));
/* The task holding the UI lock, NULL if none. */
void *t5_ui_lock_holder(void);
/* Releases the UI lock if the calling task holds it (a task being ended). */
void t5_ui_release_if_held(void);

#ifdef __cplusplus
}
#endif
