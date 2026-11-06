/* SPDX-License-Identifier: Apache-2.0
 * LVGL port: display pipeline, touch input, UI task, rotation (plan 4.2, 4.9).
 * See t5_ui.h for the design. Hardware (PPA, aligned PSRAM) is in ui_hw.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "t5_core/event.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_display/t5_display.h"
#include "t5_imu/t5_imu.h"
#include "t5_input/t5_touch.h"
#include "t5_power/autosave.h"
#include "t5_power/power.h"
#include "t5_storage/t5_storage.h"
#include "t5_ui/fbtrack.h"
#include "t5_ui/orient.h"
#include "t5_ui/t5_ui.h"
#include "ui_hw.h"

static const char *TAG = "t5.ui";

#define UI_TASK_STACK (16 * 1024)
#define UI_TASK_PRIO 5
#define UI_CORE 1
#define REFR_MS 16
#define REFR_LOW_POWER_MS 33
#define MAX_FLUSH_AREAS 32
#define IMU_HZ 10
#define CFG_VERSION 1

static lv_display_t *s_disp;
static lv_indev_t *s_indev;
static uint16_t *s_canvas;               /* logical canvas, W*H RGB565 */
static t5_rotation_t s_rot = T5_ROT_90;  /* landscape first (plan defaults) */
static t5_fbt_t s_fbt;
static t5_mutex_t s_lock;
static t5_task_t s_task;
static volatile bool s_ready, s_paused;
static bool s_low_power;
static uint32_t s_allowed = T5_ROT_ALL;
static t5_ui_rotate_hook_t s_rot_hook;
static void *s_rot_arg;

static volatile bool s_reveal;           /* turn the backlight on after the next frame */

static t5_rect_t s_flush[MAX_FLUSH_AREAS];
static int s_nflush;
static bool s_flush_overflow;

/* Watchdog support: when an iteration started, 0 while waiting. */
static volatile int64_t s_busy_since_us;

/* ---- settings (rotation lock survives reboot) ---------------------------- */
typedef struct {
    uint8_t locked;
    uint8_t rotation;
    uint8_t auto_rotate;
    uint8_t pad;
} ui_cfg_t;
static ui_cfg_t s_cfg = { .locked = 0, .rotation = T5_ROT_90, .auto_rotate = 1 };
static bool s_cfg_dirty;

static void cfg_path(char *buf, size_t n)
{
    snprintf(buf, n, "%s/ui.cfg", t5_autosave_dir());
}

static void cfg_load(void)
{
    char p[96];
    cfg_path(p, sizeof p);
    ui_cfg_t c;
    size_t len = 0;
    if (t5_storage_blob_read(p, CFG_VERSION, &c, sizeof c, &len) == T5_OK && len == sizeof c && c.rotation < 4) {
        s_cfg = c;
    }
}

static t5_err_t cfg_save(void *arg)
{
    (void)arg;
    if (!s_cfg_dirty) return T5_OK;
    char p[96];
    cfg_path(p, sizeof p);
    t5_err_t e = t5_storage_blob_write(p, CFG_VERSION, &s_cfg, sizeof s_cfg);
    if (e == T5_OK) s_cfg_dirty = false;
    return e;
}

/* ---- stats --------------------------------------------------------------- */
static struct {
    uint32_t frames, frames_sec;
    uint64_t render_us_sum, copy_us_sum;
    uint32_t render_us_max, copy_bytes;
    int64_t render_start;
    uint32_t loop_max_ms;
    int64_t touch_press_us;
    uint32_t touch_to_flush_us;
    t5_ui_stats_t out;
} s_st;
static lv_obj_t *s_stats_label;

static void stats_second(lv_timer_t *t)
{
    (void)t;
    uint32_t n = s_st.frames_sec;
    s_st.out.frames = s_st.frames;
    s_st.out.panel_frames = t5_display_frames();
    s_st.out.fps = n;
    s_st.out.render_us_avg = n ? (uint32_t)(s_st.render_us_sum / n) : 0;
    s_st.out.render_us_max = s_st.render_us_max;
    s_st.out.copy_us_avg = n ? (uint32_t)(s_st.copy_us_sum / n) : 0;
    s_st.out.copy_kb_s = s_st.copy_bytes / 1024;
    s_st.out.refresh_period_ms = s_low_power ? REFR_LOW_POWER_MS : REFR_MS;
    s_st.out.ui_loop_max_ms = s_st.loop_max_ms;
    s_st.out.touch_to_flush_us = s_st.touch_to_flush_us;
    s_st.frames_sec = 0;
    s_st.render_us_sum = s_st.copy_us_sum = 0;
    s_st.render_us_max = 0;
    s_st.copy_bytes = 0;
    s_st.loop_max_ms = 0;

    /* Low-power mode follows the power service (battery 10% or the user). */
    if (t5_power_low_power() != s_low_power) {
        t5_ui_set_low_power(t5_power_low_power());
    }
    if (s_stats_label) {
        lv_label_set_text_fmt(s_stats_label, "%u fps  render %u/%u us  copy %u us  %u KB/s  loop %u ms  touch %u us",
                              (unsigned)s_st.out.fps, (unsigned)s_st.out.render_us_avg,
                              (unsigned)s_st.out.render_us_max, (unsigned)s_st.out.copy_us_avg,
                              (unsigned)s_st.out.copy_kb_s, (unsigned)s_st.out.ui_loop_max_ms,
                              (unsigned)s_st.out.touch_to_flush_us);
    }
}

void t5_ui_get_stats(t5_ui_stats_t *out)
{
    if (out) *out = s_st.out;
}

void t5_ui_show_stats(bool on)
{
    t5_ui_lock();
    if (on && !s_stats_label && s_disp) {
        s_stats_label = lv_label_create(lv_display_get_layer_sys(s_disp));
        lv_obj_set_style_bg_color(s_stats_label, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_stats_label, LV_OPA_70, 0);
        lv_obj_set_style_text_color(s_stats_label, lv_color_hex(0x00FF7F), 0);
        lv_obj_set_style_pad_all(s_stats_label, 4, 0);
        lv_obj_align(s_stats_label, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_label_set_text(s_stats_label, "...");
    } else if (!on && s_stats_label) {
        lv_obj_delete(s_stats_label);
        s_stats_label = NULL;
    }
    t5_ui_unlock();
}

/* ---- display pipeline ---------------------------------------------------- */
static void present(void)
{
    for (int i = 0; i < s_nflush; i++) t5_fbt_add(&s_fbt, s_flush[i]);
    if (s_flush_overflow) t5_fbt_all_dirty(&s_fbt);
    s_nflush = 0;
    s_flush_overflow = false;

    int idx = t5_fbt_pick(T5_DISPLAY_FBS, t5_display_scanout(), t5_display_queued());
    if (idx < 0) return;
    uint16_t *fb = t5_display_fb_n(idx);
    t5_rect_t rects[T5_FBT_RECTS];
    int n = t5_fbt_take(&s_fbt, idx, rects, T5_FBT_RECTS);
    int32_t w, h;
    t5_rot_size(s_rot, &w, &h);
    int64_t t0 = t5_time_us();
    for (int i = 0; i < n; i++) {
        if (t5_ui_hw_copy(0, s_canvas, w, h, rects[i], rects[i], fb, s_rot) != T5_OK) {
            t5_fbt_add(&s_fbt, rects[i]);     /* retry next frame: still lagging */
            continue;
        }
        s_st.copy_bytes += (uint32_t)t5_rect_w(&rects[i]) * (uint32_t)t5_rect_h(&rects[i]) * 2;
    }
    if (n) {
        /* The other buffers lag by everything just copied; t5_fbt_add above
         * already recorded it for them. */
        t5_display_present(idx, false);
        if (s_reveal) {
            s_reveal = false;
            t5_display_release_hold();
        }
    }
    int64_t t1 = t5_time_us();
    s_st.copy_us_sum += (uint64_t)(t1 - t0);
    s_st.frames++;
    s_st.frames_sec++;
    if (s_st.touch_press_us) {
        s_st.touch_to_flush_us = (uint32_t)(t1 - s_st.touch_press_us);
        s_st.touch_press_us = 0;
    }
}

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)px;
    if (s_nflush < MAX_FLUSH_AREAS) {
        s_flush[s_nflush++] = (t5_rect_t){ a->x1, a->y1, a->x2, a->y2 };
    } else {
        s_flush_overflow = true;
    }
    if (lv_display_flush_is_last(d)) {
        present();
    }
    lv_display_flush_ready(d);
}

static void render_event(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_RENDER_START) {
        s_st.render_start = t5_time_us();
    } else if (c == LV_EVENT_RENDER_READY && s_st.render_start) {
        uint32_t us = (uint32_t)(t5_time_us() - s_st.render_start);
        s_st.render_us_sum += us;
        if (us > s_st.render_us_max) s_st.render_us_max = us;
        s_st.render_start = 0;
    }
}

static void apply_buffers(void)
{
    int32_t w, h;
    t5_rot_size(s_rot, &w, &h);
    lv_display_set_resolution(s_disp, w, h);
    lv_display_set_buffers(s_disp, s_canvas, NULL, (uint32_t)(w * h * 2), LV_DISPLAY_RENDER_MODE_DIRECT);
    t5_fbt_init(&s_fbt, w, h);
}

/* ---- touch ---------------------------------------------------------------- */
void t5_ui_touch_to_logical(int32_t px, int32_t py, int32_t *lx, int32_t *ly)
{
    t5_rot_from_panel(s_rot, px, py, lx, ly);
}

bool t5_ui_touch_peek(int32_t *x, int32_t *y)
{
    t5_touch_state_t st;
    if (t5_touch_read(&st) != T5_OK || st.count == 0) return false;
    t5_ui_touch_to_logical(st.p[0].x, st.p[0].y, x, y);
    return true;
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static bool was_down;
    t5_touch_state_t st;
    if (t5_touch_read(&st) == T5_OK && st.count) {
        int32_t x, y;
        t5_ui_touch_to_logical(st.p[0].x, st.p[0].y, &x, &y);
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
        if (!was_down) s_st.touch_press_us = t5_time_us();
        was_down = true;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        was_down = false;
    }
}

/* ---- locking and the UI task ---------------------------------------------- */
void t5_ui_lock(void)
{
    t5_mutex_lock(s_lock);
}

bool t5_ui_lock_timeout(uint32_t ms)
{
    return t5_mutex_lock_timeout(s_lock, ms);
}

void t5_ui_unlock(void)
{
    t5_mutex_unlock(s_lock);
}

bool t5_ui_on_ui_task(void)
{
    return s_task && t5_task_self() == s_task;
}

bool t5_ui_ready(void)
{
    return s_ready;
}

void t5_ui_reveal(void)
{
    s_reveal = true;
    if (s_disp) {
        t5_ui_lock();
        lv_obj_invalidate(lv_display_get_screen_active(s_disp));
        t5_ui_unlock();
    }
}

uint32_t t5_ui_stalled_ms(void)
{
    int64_t b = s_busy_since_us;
    return b ? (uint32_t)((t5_time_us() - b) / 1000) : 0;
}

static uint32_t tick_cb(void)
{
    return t5_time_ms();
}

static void ui_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_paused) {
            s_busy_since_us = 0;
            t5_delay_ms(50);
            continue;
        }
        s_busy_since_us = t5_time_us();
        t5_ui_lock();
        uint32_t next = lv_timer_handler();
        t5_ui_unlock();
        uint32_t took = (uint32_t)((t5_time_us() - s_busy_since_us) / 1000);
        if (took > s_st.loop_max_ms) s_st.loop_max_ms = took;
        s_busy_since_us = 0;
        if (next == LV_NO_TIMER_READY || next > 10) next = 10;
        t5_delay_ms(next ? next : 1);
    }
}

typedef struct {
    t5_ui_fn_t fn;
    void *arg;
    volatile bool *done;
} async_t;

static void async_tramp(void *p)
{
    async_t a = *(async_t *)p;
    free(p);
    a.fn(a.arg);
    if (a.done) *a.done = true;
}

static t5_err_t post(t5_ui_fn_t fn, void *arg, volatile bool *done, async_t **out)
{
    if (!s_ready || !fn) return T5_ERR_STATE;
    async_t *a = malloc(sizeof *a);
    if (!a) return T5_ERR_NO_MEM;
    *a = (async_t){ fn, arg, done };
    t5_ui_lock();
    lv_result_t r = lv_async_call(async_tramp, a);
    t5_ui_unlock();
    if (r != LV_RESULT_OK) {
        free(a);
        return T5_ERR_NO_MEM;
    }
    if (out) *out = a;
    return T5_OK;
}

t5_err_t t5_ui_async(t5_ui_fn_t fn, void *arg)
{
    return post(fn, arg, NULL, NULL);
}

t5_err_t t5_ui_sync(t5_ui_fn_t fn, void *arg, uint32_t timeout_ms)
{
    if (t5_ui_on_ui_task()) {
        fn(arg);
        return T5_OK;
    }
    volatile bool *done = calloc(1, sizeof *done);
    if (!done) return T5_ERR_NO_MEM;
    async_t *a = NULL;
    t5_err_t e = post(fn, arg, done, &a);
    if (e != T5_OK) {
        free((void *)done);
        return e;
    }
    uint32_t t0 = t5_time_ms();
    while (!*done && t5_time_ms() - t0 <= timeout_ms) t5_delay_ms(1);
    if (!*done) {
        /* The caller's arg (often on its stack) must not be used after we
         * return. The UI task runs queued calls under the UI lock, so with
         * the lock held the call has either finished or not started: cancel
         * it in the second case. */
        t5_ui_lock();
        bool cancelled = !*done && lv_async_call_cancel(async_tramp, a) == LV_RESULT_OK;
        t5_ui_unlock();
        if (cancelled) {
            free(a);
            free((void *)done);
            return T5_ERR_TIMEOUT;
        }
    }
    free((void *)done);
    return T5_OK;
}

/* ---- rotation ------------------------------------------------------------- */
static t5_orient_t s_orient;
static t5_axis_map_t s_axes = { .src = { 0, 1, 2 }, .sign = { 1, 1, 1 } };   /* [unverified] */
static t5_mutex_t s_orient_lock;

t5_rotation_t t5_ui_rotation(void)
{
    return s_rot;
}

t5_err_t t5_ui_set_rotation(t5_rotation_t r)
{
    if (r > T5_ROT_270) return T5_ERR_INVALID_ARG;
    if (!s_ready) {
        s_rot = r;
        return T5_OK;
    }
    t5_ui_lock();
    if (r != s_rot) {
        s_rot = r;
        apply_buffers();
        lv_obj_invalidate(lv_display_get_screen_active(s_disp));
        lv_obj_invalidate(lv_display_get_layer_top(s_disp));
        lv_obj_invalidate(lv_display_get_layer_sys(s_disp));
        int32_t w, h;
        t5_rot_size(r, &w, &h);
        T5_LOGI(TAG, "rotation %s (%dx%d)", t5_rot_name(r), (int)w, (int)h);
        t5_ev_ui_t ev = { .rotation = (uint8_t)r, .w = (uint16_t)w, .h = (uint16_t)h };
        t5_event_post(T5_EV_UI_ROTATE, &ev, sizeof ev);
        if (s_rot_hook) s_rot_hook(r, s_rot_arg);
    }
    if (s_cfg.rotation != r) {
        s_cfg.rotation = (uint8_t)r;
        s_cfg_dirty = true;
    }
    t5_ui_unlock();
    t5_mutex_lock(s_orient_lock);
    s_orient.current = r;
    s_orient.pending = false;
    t5_mutex_unlock(s_orient_lock);
    return T5_OK;
}

void t5_ui_set_rotate_hook(t5_ui_rotate_hook_t fn, void *arg)
{
    s_rot_arg = arg;
    s_rot_hook = fn;
}

static void rotate_async(void *p)
{
    t5_ui_set_rotation((t5_rotation_t)(intptr_t)p);
}

static void imu_cb(const t5_imu_accel_t *a, void *arg)
{
    (void)arg;
    if (!s_cfg.auto_rotate || s_cfg.locked || s_paused) return;
    int16_t chip[3] = { a->x, a->y, a->z };
    int32_t up[3];
    t5_axis_map_apply(&s_axes, chip, up);
    t5_rotation_t r;
    t5_mutex_lock(s_orient_lock);
    bool changed = t5_orient_update(&s_orient, up[0], up[1], up[2], t5_time_ms(), s_allowed, &r);
    t5_mutex_unlock(s_orient_lock);
    if (changed) t5_ui_async(rotate_async, (void *)(intptr_t)r);
}

static void imu_stream_update(void)
{
    bool want = s_ready && !s_paused && s_cfg.auto_rotate && !s_cfg.locked && t5_imu_status() == T5_IMU_READY;
    static bool on;
    if (want == on) return;
    if (t5_imu_stream(want ? IMU_HZ : 0, want ? imu_cb : NULL, NULL) == T5_OK) on = want;
}

void t5_ui_set_auto_rotate(bool on)
{
    s_cfg.auto_rotate = on;
    s_cfg_dirty = true;
    imu_stream_update();
}

bool t5_ui_auto_rotate(void)
{
    return s_cfg.auto_rotate;
}

t5_err_t t5_ui_set_rotation_lock(bool locked)
{
    s_cfg.locked = locked;
    s_cfg.rotation = (uint8_t)s_rot;
    s_cfg_dirty = true;
    imu_stream_update();
    return cfg_save(NULL);
}

bool t5_ui_rotation_locked(void)
{
    return s_cfg.locked;
}

void t5_ui_set_allowed_rotations(uint32_t mask)
{
    mask &= T5_ROT_ALL;
    if (!mask) mask = T5_ROT_ALL;
    s_allowed = mask;
    if (!(mask & T5_ROT_MASK(s_rot))) {
        /* Same-aspect partner first. */
        t5_rotation_t alt = (t5_rotation_t)((s_rot + 2) % 4);
        if (!(mask & T5_ROT_MASK(alt))) {
            for (int i = 0; i < 4; i++) if (mask & T5_ROT_MASK(i)) { alt = (t5_rotation_t)i; break; }
        }
        if (t5_ui_on_ui_task()) t5_ui_set_rotation(alt);
        else t5_ui_async(rotate_async, (void *)(intptr_t)alt);
    }
}

uint32_t t5_ui_allowed_rotations(void)
{
    return s_allowed;
}

/* ---- low power and power states ------------------------------------------ */
void t5_ui_set_low_power(bool on)
{
    s_low_power = on;
    if (!s_disp) return;
    t5_ui_lock();
    lv_timer_t *t = lv_display_get_refr_timer(s_disp);
    if (t) lv_timer_set_period(t, on ? REFR_LOW_POWER_MS : REFR_MS);
    t5_ui_unlock();
}

bool t5_ui_low_power(void)
{
    return s_low_power;
}

static void on_power(t5_event_id_t id, const void *data, size_t len, void *arg)
{
    (void)id; (void)len; (void)arg;
    const t5_ev_power_t *p = data;
    /* Screen off and Sleep: nothing is visible, so don't render. Touch still
     * wakes the device through the power service's activity hook. */
    bool pause = p->to == T5_PWR_SCREEN_OFF || p->to == T5_PWR_SLEEP;
    if (pause == s_paused) return;
    s_paused = pause;
    if (!pause && s_disp) {
        t5_ui_lock();
        lv_obj_invalidate(lv_display_get_screen_active(s_disp));
        t5_ui_unlock();
    }
    imu_stream_update();
}

static void on_imu(t5_event_id_t id, const void *data, size_t len, void *arg)
{
    (void)id; (void)data; (void)len; (void)arg;
    imu_stream_update();
}

/* ---- watchdog overlay ------------------------------------------------------ */
/* One buffer per panel frame: three back-to-back copies starve the display
 * DMA and the panel drops sync (a frame of cyan, then black). */
static void wait_frame(void)
{
    uint32_t f = t5_display_frames();
    for (int i = 0; i < 50 && t5_display_frames() == f; i++) t5_delay_ms(1);
}

static t5_rect_t s_overlay;
static bool s_overlay_on;

t5_err_t t5_ui_overlay_show(const uint16_t *img, int32_t w, int32_t h, int32_t x, int32_t y)
{
    if (!s_ready || !img) return T5_ERR_STATE;
    int32_t lw, lh;
    t5_rot_size(s_rot, &lw, &lh);
    if (x < 0 || y < 0 || x + w > lw || y + h > lh) return T5_ERR_INVALID_ARG;
    /* Copy into every buffer: the renderer is stuck, so none will be swapped
     * in over it, but the queued one may become scanout at any moment. */
    t5_rect_t src = { 0, 0, w - 1, h - 1 };
    t5_rect_t dst = { x, y, x + w - 1, y + h - 1 };
    for (int i = 0; i < T5_DISPLAY_FBS; i++) {
        wait_frame();
        T5_RETURN_ON_ERR(t5_ui_hw_copy(1, img, w, h, src, dst, t5_display_fb_n(i), s_rot));
    }
    s_overlay = (t5_rect_t){ x, y, x + w - 1, y + h - 1 };
    s_overlay_on = true;
    return T5_OK;
}

t5_err_t t5_ui_overlay_restore(void)
{
    if (!s_overlay_on) return T5_OK;
    int32_t w, h;
    t5_rot_size(s_rot, &w, &h);
    for (int i = 0; i < T5_DISPLAY_FBS; i++) {
        /* The canvas holds the last complete frame. Buffers that lagged it
         * still lag by their damage lists, which the next present() fixes. */
        wait_frame();
        T5_RETURN_ON_ERR(t5_ui_hw_copy(1, s_canvas, w, h, s_overlay, s_overlay, t5_display_fb_n(i), s_rot));
    }
    s_overlay_on = false;
    return T5_OK;
}

/* ---- init -------------------------------------------------------------------- */
static t5_err_t ui_suspend(void *arg)
{
    (void)arg;
    s_paused = true;
    /* Wait for the current iteration to finish (bounded by the hook budget). */
    for (int i = 0; i < 100 && s_busy_since_us; i++) t5_delay_ms(1);
    return T5_OK;
}

static t5_err_t ui_resume(void *arg)
{
    (void)arg;
    t5_power_state_t st = t5_power_state();
    s_paused = st == T5_PWR_SCREEN_OFF || st == T5_PWR_SLEEP;
    return T5_OK;
}

t5_err_t t5_ui_init(void)
{
    if (s_ready) return T5_OK;
    if (!t5_display_ready()) return T5_ERR_STATE;
    s_lock = t5_mutex_create();
    s_orient_lock = t5_mutex_create();
    if (!s_lock || !s_orient_lock) return T5_ERR_NO_MEM;
    T5_RETURN_ON_ERR(t5_ui_hw_init());

    cfg_load();
    s_rot = (t5_rotation_t)s_cfg.rotation;
    t5_orient_init(&s_orient, NULL, s_rot);

    s_canvas = t5_ui_hw_alloc((size_t)T5_PANEL_W * T5_PANEL_H * 2);
    if (!s_canvas) return T5_ERR_NO_MEM;
    memset(s_canvas, 0, (size_t)T5_PANEL_W * T5_PANEL_H * 2);

    lv_init();
    lv_tick_set_cb(tick_cb);
    int32_t w, h;
    t5_rot_size(s_rot, &w, &h);
    s_disp = lv_display_create(w, h);
    if (!s_disp) return T5_ERR_NO_MEM;
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, flush_cb);
    apply_buffers();
    lv_display_add_event_cb(s_disp, render_event, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(s_disp, render_event, LV_EVENT_RENDER_READY, NULL);
    lv_timer_create(stats_second, 1000, NULL);

    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, touch_read);
    lv_indev_set_display(s_indev, s_disp);

    t5_autosave_register("ui", cfg_save, NULL);
    t5_event_subscribe(T5_EV_POWER_STATE, on_power, NULL);
    t5_event_subscribe(T5_EV_IMU_FAILED, on_imu, NULL);
    const t5_suspend_hook_t hook = {
        .name = "ui", .prio = T5_SUSPEND_PRIO_STORAGE + 50, .budget_ms = 100,
        .suspend = ui_suspend, .resume = ui_resume,
    };
    t5_power_register_hook(&hook);

    s_ready = true;
    T5_RETURN_ON_ERR(t5_task_create("t5_ui", ui_task, NULL, UI_TASK_STACK, UI_TASK_PRIO, UI_CORE, &s_task));
    /* The IMU starts in the background; try now and again once it's ready. */
    imu_stream_update();
    T5_LOGI(TAG, "LVGL %d.%d.%d, %dx%d, rotation %s%s", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
            LVGL_VERSION_PATCH, (int)w, (int)h, t5_rot_name(s_rot), s_cfg.locked ? " (locked)" : "");
    return T5_OK;
}

/* Diagnostics: change how the IMU's axes map onto the panel. */
void t5_ui_set_axis_map(const uint8_t src[3], const int8_t sign[3])
{
    t5_mutex_lock(s_orient_lock);
    memcpy(s_axes.src, src, 3);
    memcpy(s_axes.sign, sign, 3);
    t5_mutex_unlock(s_orient_lock);
}

void t5_ui_orient_debug(int16_t *angle, bool *flat, t5_rotation_t *pending)
{
    t5_mutex_lock(s_orient_lock);
    if (angle) *angle = s_orient.angle_deg;
    if (flat) *flat = s_orient.flat;
    if (pending) *pending = s_orient.pending ? s_orient.candidate : s_orient.current;
    t5_mutex_unlock(s_orient_lock);
}
