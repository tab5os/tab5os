/* SPDX-License-Identifier: Apache-2.0
 * LVGL port: display pipeline, touch input, UI task, rotation (plan 4.2, 4.9).
 * See t5_ui.h for the design. Hardware (PPA, aligned PSRAM) is in ui_hw.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "display/lv_display_private.h"   /* the frame's joined dirty areas */
#include "draw/lv_draw_private.h"         /* the PPA unit's evaluate hook */
#include "draw/sw/lv_draw_sw_private.h"   /* abandoning a frame: the unit's current task */
#include "draw/espressif/ppa/lv_draw_ppa_private.h"
#include "core/lv_global.h"
#include <setjmp.h>
#include "t5_core/event.h"
#include "t5_core/fault.h"
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
#include "t5_ui/ui_diag.h"
#include "ui_hw.h"
#include "ui_priv.h"

static const char *TAG = "t5.ui";

_Static_assert(T5_FBT_BUFS == T5_DISPLAY_FBS, "damage tracking keeps one list per framebuffer");

#define UI_TASK_STACK (16 * 1024)
#define UI_TASK_PRIO 5
#define UI_CORE 1
#define IMU_HZ 10
#define CFG_VERSION 1

static lv_display_t *s_disp;
static lv_indev_t *s_indev;
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
    T5_IGNORE(t5_sys_path(buf, n, "ui.cfg"));
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
    uint64_t render_px_sum;
    int64_t render_start;
    uint32_t loop_max_ms;
    int64_t touch_press_us;
    uint32_t touch_to_flush_us;
    t5_ui_stats_t out;
} s_st;
static lv_obj_t *s_stats_label;

/* Callers include the battery task: they only store the flag, and the UI
 * task applies it, so a hung UI can't stall them. */
void t5_ui_set_low_power(bool on)
{
    __atomic_store_n(&s_low_power, on, __ATOMIC_RELAXED);
}

/* UI task, under the lock. */
static void apply_low_power(void)
{
    static int applied = -1;
    bool on = __atomic_load_n(&s_low_power, __ATOMIC_RELAXED);
    if (applied == (int)on) return;
    lv_timer_t *t = lv_display_get_refr_timer(s_disp);
    if (!t) return;
    lv_timer_set_period(t, on ? T5_UI_REFRESH_LOW_POWER_MS : T5_UI_REFRESH_MS);
    applied = on;
}

static void stats_second(lv_timer_t *t)
{
    (void)t;
    uint32_t n = s_st.frames_sec;
    s_st.out.frames = s_st.frames;
    s_st.out.panel_frames = t5_display_frames();
    s_st.out.fps = n;
    s_st.out.render_us_avg = n ? (uint32_t)(s_st.render_us_sum / n) : 0;
    s_st.out.render_us_max = s_st.render_us_max;
    s_st.out.render_kpx_avg = n ? (uint32_t)(s_st.render_px_sum / n / 1000) : 0;
    s_st.out.copy_us_avg = n ? (uint32_t)(s_st.copy_us_sum / n) : 0;
    s_st.out.copy_kb_s = s_st.copy_bytes / 1024;
    s_st.out.refresh_period_ms = s_low_power ? T5_UI_REFRESH_LOW_POWER_MS : T5_UI_REFRESH_MS;
    s_st.out.ui_loop_max_ms = s_st.loop_max_ms;
    s_st.out.touch_to_flush_us = s_st.touch_to_flush_us;
    s_st.frames_sec = 0;
    s_st.render_us_sum = s_st.copy_us_sum = 0;
    s_st.render_px_sum = 0;
    s_st.render_us_max = 0;
    s_st.copy_bytes = 0;
    s_st.loop_max_ms = 0;
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

static t5_ui_frame_rec_t s_flog[T5_UI_FRAME_LOG];
static uint32_t s_flog_n;   /* frames logged, ever */

int t5_ui_frame_log(t5_ui_frame_rec_t *out, int max)
{
    int n = s_flog_n < T5_UI_FRAME_LOG ? (int)s_flog_n : T5_UI_FRAME_LOG;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = s_flog[(s_flog_n - (uint32_t)n + (uint32_t)i) % T5_UI_FRAME_LOG];
    return n;
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

/* ---- display pipeline ----------------------------------------------------
 * LVGL renders in bands (partial mode, two band buffers) and each band is
 * rotated into the back framebuffer by the PPA asynchronously, while LVGL
 * draws the next band. There is no full-screen canvas: the newest complete
 * framebuffer is the reference.
 *
 * Three framebuffers: the one being scanned out, at most one queued for the
 * next vsync, and the back buffer this frame is drawn into. A back buffer
 * misses whatever changed since it was last on screen (fbtrack keeps that
 * per buffer); at the start of a frame the PPA copies those areas across
 * from the newest buffer, except where this frame redraws them anyway (for
 * a scrolling list, nothing). At the end of the frame, once the PPA is idle,
 * the back buffer is queued for the next vsync: tear-free.
 */
/* Bands are drawn in internal RAM: CPU writes there don't pull PSRAM lines
 * into the cache first or need writing back, and the PPA reads them off the
 * PSRAM bus. Scroll went from 37 to 55 fps (2026-11-06). The two 20-line
 * bands are one 100 KB block, which the camera's H.264 encoder uses as its
 * reference buffer while it records (t5_ui_lend_internal); meanwhile the UI
 * draws in 40-line PSRAM bands. Bigger bands don't pay: the rest of the
 * system (LVGL's objects especially) needs ~130 KB of internal RAM free, and
 * the UI slows down 2-4x once they spill into PSRAM. */
#define BAND_LINES_INT 20
#define BAND_LINES_PSRAM 40
#define BAND_INT_BYTES ((size_t)T5_PANEL_H * BAND_LINES_INT * 2)
#define BAND_INT_BLOCK (2 * BAND_INT_BYTES)
static uint16_t *s_band_psram[2];
static uint16_t *s_band_int;        /* both internal bands; NULL while lent */
static bool s_band_lent;            /* lent out: don't take it back on our own */
static bool s_band_warned;
static int s_band_lines;
#define FRAME_MAX_AREAS LV_INV_BUF_SIZE
#define FLUSH_TOKEN ((void *)1)

static uint16_t *s_band[2];       /* the bands LVGL draws in */
static int s_target = -1;           /* back buffer of the frame being drawn */
static int s_front = -1;            /* newest complete buffer */
static t5_rect_t s_frame[FRAME_MAX_AREAS];
static int s_nframe;
static bool s_flushed;
static bool s_redraw_all;

static SemaphoreHandle_t s_flush_sem;

static void ppa_done(void *user)
{
    /* PPA interrupt: a band is in the framebuffer, its buffer is free. */
    if (user == FLUSH_TOKEN && s_disp) {
        lv_display_flush_ready(s_disp);
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(s_flush_sem, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

/* LVGL waits for a band buffer here instead of spinning on a flag, so the
 * other tasks on this core keep running. */
static void flush_wait(lv_display_t *d)
{
    int64_t t0 = t5_time_us();
    int tries = 0;
    while (((volatile lv_display_t *)d)->flushing) {
        if (xSemaphoreTake(s_flush_sem, pdMS_TO_TICKS(50)) != pdTRUE && ++tries >= 4) {
            T5_LOGW(TAG, "band flush didn't complete in 200 ms");
            break;
        }
    }
    s_flog[s_flog_n % T5_UI_FRAME_LOG].flush_wait_us += (uint32_t)(t5_time_us() - t0);
}

static void frame_begin(void)
{
    s_target = t5_fbt_pick(T5_DISPLAY_FBS, t5_display_scanout(), t5_display_queued());
    s_flushed = false;
    s_nframe = 0;
    t5_ui_frame_rec_t *fr = &s_flog[s_flog_n % T5_UI_FRAME_LOG];
    *fr = (t5_ui_frame_rec_t){ .start_ms = t5_time_ms() };
    t5_ui_ppa_draw_us = t5_ui_ppa_draw_n = 0;
    for (uint32_t i = 0; i < s_disp->inv_p && s_nframe < FRAME_MAX_AREAS; i++) {
        if (s_disp->inv_area_joined[i]) continue;
        const lv_area_t *a = &s_disp->inv_areas[i];
        s_frame[s_nframe++] = (t5_rect_t){ a->x1, a->y1, a->x2, a->y2 };
        s_st.render_px_sum += (uint64_t)lv_area_get_size(a);
        if (!fr->nareas) {
            fr->x1 = (int16_t)a->x1;
            fr->y1 = (int16_t)a->y1;
            fr->x2 = (int16_t)a->x2;
            fr->y2 = (int16_t)a->y2;
        }
        fr->px += lv_area_get_size(a);
        fr->nareas++;
    }
    if (s_target < 0) return;
    /* Catch the back buffer up, except where this frame redraws. */
    t5_rect_t lag[T5_FBT_RECTS], need[48];
    int nl = t5_fbt_take(&s_fbt, s_target, lag, T5_FBT_RECTS);
    int nn = t5_fbt_subtract(lag, nl, s_frame, s_nframe, need, 48);
    if (!nn) return;
    if (s_front < 0 || s_front == s_target) {
        /* No reference yet: redraw everything next frame (not now: LVGL
         * ignores invalidation while it renders). */
        s_redraw_all = true;
        return;
    }
    uint16_t *src = t5_display_fb_n(s_front), *dst = t5_display_fb_n(s_target);
    for (int i = 0; i < nn; i++) {
        t5_rect_t p = t5_rot_rect_to_panel(s_rot, need[i]);
        if (t5_ui_hw_fb_copy_async(src, dst, p, NULL) == T5_OK)
            s_st.copy_bytes += (uint32_t)t5_rect_w(&p) * (uint32_t)t5_rect_h(&p) * 2;
    }
}

static void frame_end(void)
{
    if (s_target < 0) return;
    int64_t t0 = t5_time_us();
    /* The last band may still be in the PPA: wait, then queue for vsync. */
    if (!t5_ui_hw_wait_idle(100)) T5_LOGW(TAG, "PPA didn't finish a frame in 100 ms");
    s_flog[s_flog_n % T5_UI_FRAME_LOG].wait_us = (uint32_t)(t5_time_us() - t0);
    s_flog[s_flog_n % T5_UI_FRAME_LOG].copy_exec_us = t5_ui_copy_exec_us;
    s_flog[s_flog_n % T5_UI_FRAME_LOG].copies = t5_ui_copy_n;
    t5_ui_copy_exec_us = t5_ui_copy_n = 0;
    s_flog_n++;
    if (s_flushed) {
        for (int i = 0; i < s_nframe; i++) t5_fbt_add(&s_fbt, s_frame[i]);
        t5_rect_t drop[T5_FBT_RECTS];
        t5_fbt_take(&s_fbt, s_target, drop, T5_FBT_RECTS);   /* the target has all of it */
        t5_display_present(s_target, false);
        s_front = s_target;
        if (s_reveal) {
            s_reveal = false;
            t5_display_release_hold();
        }
    }
    int64_t t1 = t5_time_us();
    s_st.copy_us_sum += (uint64_t)(t1 - t0);     /* the copy time not hidden behind rendering */
    s_st.frames++;
    s_st.frames_sec++;
    if (s_st.touch_press_us) {
        s_st.touch_to_flush_us = (uint32_t)(t1 - s_st.touch_press_us);
        s_st.touch_press_us = 0;
    }
    s_target = -1;
    if (s_redraw_all) {
        s_redraw_all = false;
        t5_fbt_all_dirty(&s_fbt);
        lv_obj_invalidate(lv_display_get_screen_active(s_disp));
    }
}

/* Copy what each framebuffer is missing into the ones the panel isn't
 * using, from the newest frame. After a full-screen change (a dialog's
 * scrim) the next two frames would otherwise each carry a near-full
 * catch-up copy, ~20 ms, in the middle of the animation that follows.
 * Only on request (t5_ui_settled): done blindly it copies whole screens
 * that a following full-screen frame redraws anyway. UI task, between
 * frames; the copies queue on the bands' PPA client, so they stay in
 * order. */
static void catch_up_now(void)
{
    if (s_target >= 0 || s_front < 0) return;
    int scan = t5_display_scanout(), queued = t5_display_queued();
    for (int i = 0; i < T5_DISPLAY_FBS; i++) {
        if (i == s_front || i == scan || i == queued) continue;
        t5_rect_t lag[T5_FBT_RECTS];
        int n = t5_fbt_take(&s_fbt, i, lag, T5_FBT_RECTS);
        uint16_t *src = t5_display_fb_n(s_front), *dst = t5_display_fb_n(i);
        for (int k = 0; k < n; k++) {
            t5_rect_t p = t5_rot_rect_to_panel(s_rot, lag[k]);
            if (t5_ui_hw_fb_copy_async(src, dst, p, NULL) == T5_OK)
                s_st.copy_bytes += (uint32_t)t5_rect_w(&p) * (uint32_t)t5_rect_h(&p) * 2;
        }
    }
}

bool t5_ui_settled(void)
{
    catch_up_now();
    if (t5_ui_hw_busy()) return false;   /* queued is not done */
    /* Every framebuffer close to the newest frame: what's left is what a
     * small change (a clock, a cursor) adds, not a full-screen change. */
    uint32_t limit = (uint32_t)T5_PANEL_W * T5_PANEL_H / 20;
    for (int i = 0; i < T5_DISPLAY_FBS; i++) {
        if (t5_fbt_pixels(&s_fbt, i) > limit) return false;
    }
    return true;
}

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    if (s_target < 0) {
        lv_display_flush_ready(d);
        return;
    }
    int32_t stride_px = (int32_t)(lv_draw_buf_width_to_stride((uint32_t)lv_area_get_width(a), LV_COLOR_FORMAT_RGB565) / 2);
    t5_rect_t r = { a->x1, a->y1, a->x2, a->y2 };
    if (t5_ui_hw_band_async((const uint16_t *)px, stride_px, r, t5_display_fb_n(s_target), s_rot, FLUSH_TOKEN) != T5_OK) {
        lv_display_flush_ready(d);
        return;
    }
    s_flushed = true;
    s_st.copy_bytes += (uint32_t)t5_rect_w(&r) * (uint32_t)t5_rect_h(&r) * 2;
    s_flog[s_flog_n % T5_UI_FRAME_LOG].flushes++;
}

static void render_event(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_RENDER_START) {
        s_st.render_start = t5_time_us();
        frame_begin();
    } else if (c == LV_EVENT_RENDER_READY && s_st.render_start) {
        uint32_t us = (uint32_t)(t5_time_us() - s_st.render_start);
        s_st.render_us_sum += us;
        if (us > s_st.render_us_max) s_st.render_us_max = us;
        s_flog[s_flog_n % T5_UI_FRAME_LOG].render_us = us;
        s_flog[s_flog_n % T5_UI_FRAME_LOG].ppa_us = t5_ui_ppa_draw_us;
        s_flog[s_flog_n % T5_UI_FRAME_LOG].ppa_n = t5_ui_ppa_draw_n;
        s_st.render_start = 0;
    } else if (c == LV_EVENT_REFR_READY) {
        frame_end();
    }
}

static void pick_bands(void)
{
    if (s_band_int) {
        s_band[0] = s_band_int;
        s_band[1] = s_band_int + BAND_INT_BYTES / 2;
        s_band_lines = BAND_LINES_INT;
    } else {
        s_band[0] = s_band_psram[0];
        s_band[1] = s_band_psram[1];
        s_band_lines = BAND_LINES_PSRAM;
    }
    if (s_disp)
        lv_display_set_buffers(s_disp, s_band[0], s_band[1], (uint32_t)(T5_PANEL_H * s_band_lines * 2),
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
}

static void *s_band_out;     /* lent, not yet collected by the borrower */

/* The PPA has finished with the bands once it is idle. */
static void lend_internal(void *arg)
{
    (void)arg;
    s_band_lent = true;
    if (!s_band_int) return;
    if (!t5_ui_hw_wait_idle(200)) T5_LOGW(TAG, "PPA busy while lending the bands");
    __atomic_store_n(&s_band_out, (void *)s_band_int, __ATOMIC_SEQ_CST);
    s_band_int = NULL;
    pick_bands();
    T5_LOGI(TAG, "bands lent: drawing in PSRAM");
}

static void *band_alloc(void)
{
    void *p = t5_ui_hw_alloc_internal(BAND_INT_BLOCK);
    if (!p && !s_band_warned) T5_LOGW(TAG, "no 100 KB internal block: bands in PSRAM for now");
    if (!p) s_band_warned = true;
    return p;
}

static void band_use(void *arg)
{
    uint16_t *p = arg;
    s_band_lent = false;
    if (!p) return;
    if (s_band_int) {
        heap_caps_free(p);
        return;
    }
    if (!t5_ui_hw_wait_idle(200)) T5_LOGW(TAG, "PPA busy while taking the bands back");
    s_band_int = p;
    s_band_warned = false;
    pick_bands();
    T5_LOGI(TAG, "bands in internal RAM");
}

/* Retry now and then, in case there was no block at reclaim. */
static void band_retry_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_band_int && !s_band_lent) band_use(band_alloc());
}

void *t5_ui_lend_internal(size_t *len)
{
    if (s_ready) T5_IGNORE(t5_ui_sync(lend_internal, NULL, 1000));
    else lend_internal(NULL);
    void *b = __atomic_exchange_n(&s_band_out, NULL, __ATOMIC_SEQ_CST);
    if (len) *len = b ? BAND_INT_BLOCK : 0;
    return b;
}

void t5_ui_reclaim_internal(void *block)
{
    if (!block) block = __atomic_exchange_n(&s_band_out, NULL, __ATOMIC_SEQ_CST);
    if (!block && !s_band_int) block = band_alloc();
    if (s_ready) T5_IGNORE(t5_ui_sync(band_use, block, 1000));
    else band_use(block);
}

void t5_ui_reserve_internal(void)
{
    if (!s_band_int) band_use(band_alloc());
}

static void apply_buffers(void)
{
    int32_t w, h;
    t5_rot_size(s_rot, &w, &h);
    lv_display_set_resolution(s_disp, w, h);
    pick_bands();
    t5_fbt_init(&s_fbt, w, h);
    s_front = -1;                    /* nothing to catch up from: the next frame redraws everything */
}

/* ---- touch ---------------------------------------------------------------- */
bool t5_ui_touch_peek(int32_t *x, int32_t *y)
{
    t5_touch_state_t st;
    if (t5_touch_read(&st) != T5_OK || st.count == 0) return false;
    t5_rot_from_panel(s_rot, st.p[0].x, st.p[0].y, x, y);
    return true;
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static bool was_down;
    t5_touch_state_t st;
    if (t5_touch_read(&st) == T5_OK && st.count) {
        int32_t x, y;
        t5_rot_from_panel(s_rot, st.p[0].x, st.p[0].y, &x, &y);
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

/* Slow motion for looking at animations (`ui slowmo N`): LVGL's clock runs
 * N times slower, so every animation and timer stretches. 1 = normal. */
static volatile uint32_t s_slowmo = 1;

static uint32_t tick_cb(void)
{
    static uint32_t last_real, virt, rem;
    uint32_t now = t5_time_ms(), d = now - last_real, n = s_slowmo ? s_slowmo : 1;
    last_real = now;
    rem += d;
    virt += rem / n;
    rem %= n;
    return virt;
}

void t5_ui_set_slowmo(uint32_t n)
{
    s_slowmo = n ? n : 1;
}

/* ---- app faults ------------------------------------------------------------ */
static t5_ui_unwind_fn_t s_unwind_hook;
static volatile uint32_t s_unwinds;

void t5_ui_set_unwind_hook(t5_ui_unwind_fn_t fn)
{
    s_unwind_hook = fn;
}

void *t5_ui_task_handle(void)
{
    return s_task;
}

bool t5_ui_unwind_safe(void)
{
    /* A frame being drawn can be abandoned (render_abandoned). One being
     * handed to the panel can't: the flush owns the buffers and the DMA. */
    return !s_disp || !s_disp->flushing;
}

void *t5_ui_lock_holder(void)
{
    return s_lock ? (void *)xSemaphoreGetMutexHolder((SemaphoreHandle_t)s_lock) : NULL;
}

void t5_ui_release_if_held(void)
{
    t5_mutex_release_all(s_lock);
}

/* LVGL state that only says "something is in progress" and would stay set
 * after an iteration was abandoned half way. Rendering is excluded by
 * t5_ui_unwind_safe(); the rest of LVGL's state is consistent between
 * calls, which is where app code runs. */
/* A frame was abandoned half drawn (an app's draw callback faulted, or a
 * draw unit read bad app data). Everything it queued goes; the next frame is
 * drawn from scratch. LVGL runs its units on this task, so none is busy. */
static void render_abandoned(lv_display_t *d)
{
    for (lv_draw_unit_t *u = LV_GLOBAL_DEFAULT()->draw_info.unit_head; u; u = u->next) {
        if (!strncmp(u->name, "SW", 2)) ((lv_draw_sw_unit_t *)u)->task_act = NULL;
#if LV_USE_PPA
        else ((lv_draw_ppa_unit_t *)u)->task_act = NULL;
#endif
    }
    /* Cleaning up a layer task deletes the layer it draws, so the list
     * changes under us: one task at a time, from the top. */
    for (;;) {
        lv_layer_t *l = d->layer_head;
        while (l && !l->draw_task_head) l = l->next;
        if (!l) break;
        lv_draw_task_t *t = l->draw_task_head;
        l->draw_task_head = t->next;
        t->state = LV_DRAW_TASK_STATE_FINISHED;
        lv_draw_cleanup_task(t);
    }
    /* Layers whose task hadn't been queued yet. A tile (no parent) is part
     * of an array that the abandoned frame owned: at most a small leak. */
    while (d->layer_head && d->layer_head->next) {
        lv_layer_t *l = d->layer_head->next;
        if (l->parent) lv_draw_layer_delete(l);
        else lv_draw_layer_deinit(l);
    }
    d->rendering_in_progress = false;
}

static void lvgl_abandoned(void)
{
    if (s_disp && s_disp->rendering_in_progress) render_abandoned(s_disp);
    lv_global_t *g = LV_GLOBAL_DEFAULT();
    g->timer_state.already_running = false;
    g->event_header = NULL;          /* events in flight lived on the abandoned stack */
    g->layout_update_mutex = false;
    g->indev_active = NULL;
    g->indev_obj_active = NULL;
}

/* One call that has to run even while rendering is paused (t5_ui_call_paused).
 * The state is the only thing shared: whoever moves it owns the next step. */
enum { PC_IDLE, PC_CLAIMED, PC_POSTED, PC_RUNNING, PC_DONE, PC_ABANDONED };
static t5_ui_fn_t s_pc_fn;
static void *s_pc_arg;
static volatile int s_pc_state;
static SemaphoreHandle_t s_pc_done;   /* given on RUNNING -> DONE */
static SemaphoreHandle_t s_wake;      /* wakes the UI task while it is paused */

static bool pc_move(int from, int to)
{
    return __atomic_compare_exchange_n(&s_pc_state, &from, to, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* The call returned, or the app code in it faulted and was unwound. */
static void pc_finish(void)
{
    if (pc_move(PC_RUNNING, PC_DONE)) xSemaphoreGive(s_pc_done);
    else pc_move(PC_ABANDONED, PC_IDLE);
}

static void pc_run(void)
{
    if (!pc_move(PC_POSTED, PC_RUNNING)) return;
    s_pc_fn(s_pc_arg);
    pc_finish();
}

t5_err_t t5_ui_call_paused(t5_ui_fn_t fn, void *arg, uint32_t timeout_ms)
{
    if (!s_ready || !fn) return T5_ERR_STATE;
    if (t5_ui_on_ui_task()) {
        fn(arg);
        return T5_OK;
    }
    if (!pc_move(PC_IDLE, PC_CLAIMED)) return T5_ERR_BUSY;
    xSemaphoreTake(s_pc_done, 0);    /* a give from a call that timed out as it finished */
    s_pc_fn = fn;
    s_pc_arg = arg;
    pc_move(PC_CLAIMED, PC_POSTED);
    xSemaphoreGive(s_wake);
    xSemaphoreTake(s_pc_done, pdMS_TO_TICKS(timeout_ms));
    if (pc_move(PC_DONE, PC_IDLE)) return T5_OK;
    if (pc_move(PC_POSTED, PC_IDLE)) return T5_ERR_TIMEOUT;     /* never started */
    /* Still running: the UI task frees the slot when it finishes. arg must
     * outlive the call, which t5_ui.h asks of the caller. */
    if (!pc_move(PC_RUNNING, PC_ABANDONED)) pc_move(PC_DONE, PC_IDLE);
    return T5_ERR_TIMEOUT;
}

static void ui_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_paused && __atomic_load_n(&s_pc_state, __ATOMIC_ACQUIRE) != PC_POSTED) {
            s_busy_since_us = 0;
            xSemaphoreTake(s_wake, pdMS_TO_TICKS(50));
            continue;
        }
        uint32_t next;
        /* Here, in this frame: the stack between a fault and this point is
         * what decides whether an app is to blame (fault.h). */
        t5_fault_point_t fp;
        t5_fault_push(&fp, NULL);
        if (setjmp(fp.jb) == 0) {
            s_busy_since_us = t5_time_us();
            t5_ui_lock();
            pc_run();
            apply_low_power();
            next = s_paused ? LV_NO_TIMER_READY : lv_timer_handler();
            t5_ui_unlock();
            uint32_t took = (uint32_t)((t5_time_us() - s_busy_since_us) / 1000);
            if (took > s_st.loop_max_ms) s_st.loop_max_ms = took;
        } else {
            /* App code faulted or was stopped in this iteration. */
            s_unwinds++;
            pc_finish();
            lvgl_abandoned();
            t5_mutex_release_all(s_lock);
            t5_ui_lock();
            if (s_unwind_hook) s_unwind_hook(&fp.info);
            if (s_disp) lv_obj_invalidate(lv_display_get_screen_active(s_disp));
            t5_ui_unlock();
            next = 1;
        }
        t5_fault_pop(&fp);
        s_busy_since_us = 0;
        if (next == LV_NO_TIMER_READY || next > 10) next = 10;
        t5_delay_ms(next ? next : 1);
    }
}

/* A t5_ui_sync caller's wait, on its stack. */
typedef struct {
    volatile bool done;
    StaticSemaphore_t sem_buf;
    SemaphoreHandle_t sem;
} sync_wait_t;

typedef struct async {
    t5_ui_fn_t fn;
    void *arg;
    sync_wait_t *wait;
    struct async *next;
} async_t;

/* Calls waiting for the UI task. The common case comes from a fixed pool, so
 * posting doesn't touch the heap; a sender that floods the queue is refused
 * rather than using up memory. Guarded by the UI lock. */
#define ASYNC_POOL 64
#define ASYNC_MAX_PENDING 512
static async_t s_async_pool[ASYNC_POOL];
static async_t *s_async_free[ASYNC_POOL];
static int s_async_nfree = -1;
static uint32_t s_async_pending;

static bool async_in_pool(const async_t *a)
{
    return a >= s_async_pool && a < s_async_pool + ASYNC_POOL;
}

static async_t *async_get(void)
{
    if (s_async_nfree < 0) {
        for (int i = 0; i < ASYNC_POOL; i++) s_async_free[i] = &s_async_pool[i];
        s_async_nfree = ASYNC_POOL;
    }
    if (s_async_pending >= ASYNC_MAX_PENDING) return NULL;
    async_t *a = s_async_nfree > 0 ? s_async_free[--s_async_nfree]
                                   : heap_caps_malloc(sizeof *a, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (a) s_async_pending++;
    return a;
}

static void async_put(async_t *a)
{
    s_async_pending--;
    if (async_in_pool(a)) s_async_free[s_async_nfree++] = a;
    else heap_caps_free(a);
}

/* Posted calls run in the order they were posted. Each lv_async_call is a
 * one-shot timer, and LVGL puts new timers at the head of its list, so calls
 * posted within one frame used to run newest first (an app setting a label
 * to "A" then "B" from a task ended on "A"). Instead they wait in this FIFO
 * and one LVGL call drains it: the batch that was queued when it starts, so
 * a call that posts again runs in the next one. Guarded by the UI lock. */
static async_t *s_q_head, *s_q_tail;
static bool s_drain_posted;

static void async_drain(void *unused)
{
    (void)unused;
    async_t *a = s_q_head;
    s_q_head = s_q_tail = NULL;
    s_drain_posted = false;
    while (a) {
        async_t c = *a;
        async_put(a);
        c.fn(c.arg);
        if (c.wait) {
            c.wait->done = true;
            xSemaphoreGive(c.wait->sem);
        }
        a = c.next;
    }
}

/* Take a not-yet-run call back out of the queue (UI lock held). */
static bool async_unqueue(async_t *a)
{
    for (async_t **pp = &s_q_head, *prev = NULL; *pp; prev = *pp, pp = &(*pp)->next) {
        if (*pp != a) continue;
        *pp = a->next;
        if (s_q_tail == a) s_q_tail = prev;
        return true;
    }
    return false;
}

static t5_err_t post(t5_ui_fn_t fn, void *arg, sync_wait_t *wait, async_t **out)
{
    if (!s_ready || !fn) return T5_ERR_STATE;
    t5_ui_lock();
    async_t *a = async_get();
    if (!a) {
        t5_ui_unlock();
        return T5_ERR_BUSY;
    }
    *a = (async_t){ fn, arg, wait, NULL };
    lv_result_t r = s_drain_posted ? LV_RESULT_OK : lv_async_call(async_drain, NULL);
    if (r == LV_RESULT_OK) {
        s_drain_posted = true;
        if (s_q_tail) s_q_tail->next = a;
        else s_q_head = a;
        s_q_tail = a;
    } else {
        async_put(a);
    }
    t5_ui_unlock();
    if (r != LV_RESULT_OK) return T5_ERR_NO_MEM;
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
    sync_wait_t w = { .done = false };
    w.sem = xSemaphoreCreateBinaryStatic(&w.sem_buf);
    async_t *a = NULL;
    t5_err_t e = post(fn, arg, &w, &a);
    if (e == T5_OK && xSemaphoreTake(w.sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        /* Neither the caller's arg (often on its stack) nor w may be used
         * after we return. The UI task runs queued calls under the UI lock,
         * so with the lock held the call has either finished or not
         * started: cancel it in the second case. */
        t5_ui_lock();
        bool cancelled = !w.done && async_unqueue(a);
        if (cancelled) async_put(a);
        t5_ui_unlock();
        if (cancelled) e = T5_ERR_TIMEOUT;
    }
    vSemaphoreDelete(w.sem);
    return e;
}

/* ---- rotation ------------------------------------------------------------- */
static t5_orient_t s_orient;
static const t5_axis_map_t s_axes = { .src = { 0, 1, 2 }, .sign = { 1, 1, 1 } };   /* [unverified] */
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
    if (!t5_ui_follows_board() || s_paused) return;
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
    bool want = s_ready && !s_paused && t5_ui_follows_board() && t5_imu_status() == T5_IMU_READY;
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

bool t5_ui_follows_board(void)
{
    return s_cfg.auto_rotate && !s_cfg.locked;
}

t5_err_t t5_ui_set_follows_board(bool on)
{
    if (on) s_cfg.auto_rotate = true;
    return t5_ui_set_rotation_lock(!on);
}

/* The rotation an app with a fixed orientation moved us away from, -1 if
 * none: when the allowed set takes it back, so does the screen (with the
 * rotation lock on, the IMU never would). */
static int8_t s_rot_before_forced = -1;

void t5_ui_set_allowed_rotations(uint32_t mask)
{
    mask &= T5_ROT_ALL;
    if (!mask) mask = T5_ROT_ALL;
    s_allowed = mask;
    t5_rotation_t to = s_rot;
    if (!(mask & T5_ROT_MASK(s_rot))) {
        if (s_rot_before_forced < 0) s_rot_before_forced = (int8_t)s_rot;
        to = t5_orient_fallback(s_rot, mask);
    } else if (s_rot_before_forced >= 0 && (mask & T5_ROT_MASK(s_rot_before_forced))) {
        to = (t5_rotation_t)s_rot_before_forced;
        s_rot_before_forced = -1;
    }
    if (to == s_rot) return;
    if (t5_ui_on_ui_task()) t5_ui_set_rotation(to);
    else t5_ui_async(rotate_async, (void *)(intptr_t)to);
}

uint32_t t5_ui_allowed_rotations(void)
{
    return s_allowed;
}

/* ---- PPA fills ------------------------------------------------------------ */
/* Solid fills are faster in software than on LVGL's PPA unit: the bands are
 * in internal RAM (about 1 GB/s for the CPU), while each PPA fill writes
 * back and invalidates the rows it covers and blocks until the DMA is done.
 * Scroll render max 17 -> 14 ms, transition 100 -> 83 ms (2026-11-06).
 * Image blits (the camera preview) stay on the PPA. */
static int32_t (*s_ppa_eval)(lv_draw_unit_t *u, lv_draw_task_t *t);
static volatile bool s_ppa_fills = false;
static volatile bool s_ppa_images = true;
static volatile bool s_ppa_alpha = true;
static volatile bool s_ppa_alpha_fills = true;

/* LVGL's PPA unit's preference score and id (lv_draw_ppa.c). */
#define PPA_PREF_SCORE 70
#define PPA_UNIT_ID 80

static int32_t ppa_eval(lv_draw_unit_t *u, lv_draw_task_t *t)
{
    /* Translucent fills (scrims) on the blender (ppa_fill_t5.c). */
    if (s_ppa_alpha_fills && t->type == LV_DRAW_TASK_TYPE_FILL && t5_ui_ppa_fill_alpha_ok(t)) {
        if (t->preference_score > PPA_PREF_SCORE) {
            t->preference_score = PPA_PREF_SCORE;
            t->preferred_draw_unit_id = PPA_UNIT_ID;
        }
        return 1;
    }
    if (!s_ppa_fills && t->type == LV_DRAW_TASK_TYPE_FILL) return 0;
    if (!s_ppa_images && t->type == LV_DRAW_TASK_TYPE_IMAGE) return 0;
    int32_t r = s_ppa_eval(u, t);
    /* Faded and ARGB8888 images too (ppa_img_t5.c). */
    if (!r && s_ppa_alpha && t->type == LV_DRAW_TASK_TYPE_IMAGE && t5_ui_ppa_img_alpha_ok(t->draw_dsc)) {
        if (t->preference_score > PPA_PREF_SCORE) {
            t->preference_score = PPA_PREF_SCORE;
            t->preferred_draw_unit_id = PPA_UNIT_ID;
        }
        r = 1;
    }
    return r;
}

void t5_ui_set_ppa_alpha(bool on)
{
    s_ppa_alpha = on;
}

void t5_ui_set_ppa_alpha_fills(bool on)
{
    s_ppa_alpha_fills = on;
}

void t5_ui_set_ppa_images(bool on)
{
    s_ppa_images = on;
}

static void hook_ppa_unit(void)
{
    for (lv_draw_unit_t *u = LV_GLOBAL_DEFAULT()->draw_info.unit_head; u; u = u->next) {
        if (u->name && !strcmp(u->name, "ESP_PPA") && u->evaluate_cb && u->evaluate_cb != ppa_eval) {
            s_ppa_eval = u->evaluate_cb;
            u->evaluate_cb = ppa_eval;
        }
    }
}

void t5_ui_set_ppa_fills(bool on)
{
    s_ppa_fills = on;
}

bool t5_ui_ppa_fills(void)
{
    return s_ppa_fills;
}

/* ---- low power and power states ------------------------------------------ */
bool t5_ui_low_power(void)
{
    return __atomic_load_n(&s_low_power, __ATOMIC_RELAXED);
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
    xSemaphoreGive(s_wake);
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
    /* The overlay is in every buffer: redraw the whole screen. The newest
     * buffer is the reference for catch-up copies, so after a full redraw
     * the others copy clean pixels from it. */
    s_overlay_on = false;
    t5_ui_lock();
    if (s_disp) lv_obj_invalidate(lv_display_get_screen_active(s_disp));
    t5_ui_unlock();
    return T5_OK;
}

/* ---- init -------------------------------------------------------------------- */
static t5_err_t ui_suspend(void *arg)
{
    (void)arg;
    s_paused = true;
    /* The UI task renders only while holding the lock, and checks s_paused
     * under it: once we have had the lock, no frame is in progress. Bounded
     * by the hook budget (a UI task stuck in an app keeps the lock). */
    if (t5_ui_lock_timeout(100)) t5_ui_unlock();
    return T5_OK;
}

static t5_err_t ui_resume(void *arg)
{
    (void)arg;
    t5_power_state_t st = t5_power_state();
    s_paused = st == T5_PWR_SCREEN_OFF || st == T5_PWR_SLEEP;
    xSemaphoreGive(s_wake);
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

    /* LVGL's internal pool first (lv_mem_t5.c; lv_init finds it made). */
    lv_mem_init();
    for (int i = 0; i < 2; i++) {
        s_band_psram[i] = t5_ui_hw_alloc((size_t)T5_PANEL_H * BAND_LINES_PSRAM * 2);
        if (!s_band_psram[i]) return T5_ERR_NO_MEM;
    }
    t5_ui_reserve_internal();

    t5_ui_hw_set_done_cb(ppa_done);
    s_flush_sem = xSemaphoreCreateBinary();
    s_pc_done = xSemaphoreCreateBinary();
    s_wake = xSemaphoreCreateBinary();
    if (!s_flush_sem || !s_pc_done || !s_wake) return T5_ERR_NO_MEM;

    lv_init();
    hook_ppa_unit();
    lv_tick_set_cb(tick_cb);
    int32_t w, h;
    t5_rot_size(s_rot, &w, &h);
    s_disp = lv_display_create(w, h);
    if (!s_disp) return T5_ERR_NO_MEM;
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_flush_wait_cb(s_disp, flush_wait);
    lv_group_t *group = lv_group_create();
    if (!group) return T5_ERR_NO_MEM;
    lv_group_set_default(group);
    apply_buffers();
    lv_timer_create(band_retry_cb, 5000, NULL);
    lv_display_add_event_cb(s_disp, render_event, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(s_disp, render_event, LV_EVENT_RENDER_READY, NULL);
    lv_display_add_event_cb(s_disp, render_event, LV_EVENT_REFR_READY, NULL);
    lv_timer_create(stats_second, 1000, NULL);

    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, touch_read);
    lv_indev_set_display(s_indev, s_disp);

    T5_LOG_ON_ERR(TAG, t5_autosave_register("ui", cfg_save, NULL));
    T5_RETURN_ON_ERR(t5_event_subscribe(T5_EV_POWER_STATE, on_power, NULL));
    T5_RETURN_ON_ERR(t5_event_subscribe(T5_EV_IMU_FAILED, on_imu, NULL));
    const t5_suspend_hook_t hook = {
        .name = "ui", .prio = T5_SUSPEND_PRIO_STORAGE + 50, .budget_ms = 100,
        .suspend = ui_suspend, .resume = ui_resume,
    };
    T5_LOG_ON_ERR(TAG, t5_power_register_hook(&hook));
    ui_md_probes_init();

    s_ready = true;
    T5_RETURN_ON_ERR(t5_task_create("t5_ui", ui_task, NULL, UI_TASK_STACK, UI_TASK_PRIO, UI_CORE, &s_task));
    t5_task_guard_margin(s_task);    /* an app's recursion ends up here */
    T5_LOG_ON_ERR(TAG, ui_hid_input_init());
    /* The IMU starts in the background; try now and again once it's ready. */
    imu_stream_update();
    T5_LOGI(TAG, "LVGL %d.%d.%d, %dx%d, rotation %s%s", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
            LVGL_VERSION_PATCH, (int)w, (int)h, t5_rot_name(s_rot), s_cfg.locked ? " (locked)" : "");
    return T5_OK;
}

void t5_ui_orient_debug(int16_t *angle, bool *flat, t5_rotation_t *pending)
{
    t5_mutex_lock(s_orient_lock);
    if (angle) *angle = s_orient.angle_deg;
    if (flat) *flat = s_orient.flat;
    if (pending) *pending = s_orient.pending ? s_orient.candidate : s_orient.current;
    t5_mutex_unlock(s_orient_lock);
}
