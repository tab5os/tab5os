/* SPDX-License-Identifier: Apache-2.0
 * t5_ui.h on the simulator: one thread, so locking is trivial; rotation
 * changes the window's resolution. Counters come from LVGL's render events.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "lvgl.h"
#include "t5_ui/orient.h"
#include "t5_ui/t5_ui.h"
#include "ui_priv.h"

static t5_rotation_t s_rot = T5_ROT_90;
static uint32_t s_allowed = T5_ROT_ALL;
static bool s_ready;
static t5_ui_rotate_hook_t s_hook;
static void *s_hook_arg;
static t5_ui_stats_t s_stats;
static uint32_t s_frames_sec, s_render_sum, s_render_max;
static int64_t s_render_t0;

static int64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static void render_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_RENDER_START) {
        s_render_t0 = now_us();
    } else if (s_render_t0) {
        uint32_t us = (uint32_t)(now_us() - s_render_t0);
        s_render_sum += us;
        if (us > s_render_max) s_render_max = us;
        s_frames_sec++;
        s_stats.frames++;
        s_render_t0 = 0;
    }
}

static void second(lv_timer_t *t)
{
    s_stats.fps = s_frames_sec;
    s_stats.render_us_avg = s_frames_sec ? s_render_sum / s_frames_sec : 0;
    s_stats.render_us_max = s_render_max;
    s_stats.refresh_period_ms = T5_UI_REFRESH_MS;
    s_frames_sec = s_render_sum = s_render_max = 0;
}

void sim_ui_attach(lv_display_t *d, t5_rotation_t r)
{
    s_rot = r;
    lv_display_add_event_cb(d, render_event, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(d, render_event, LV_EVENT_RENDER_READY, NULL);
    lv_timer_create(second, 1000, NULL);
    ui_md_probes_init();
    s_ready = true;
}

t5_err_t t5_ui_init(void) { return T5_OK; }
bool t5_ui_ready(void) { return s_ready; }
void t5_ui_lock(void) {}
bool t5_ui_lock_timeout(uint32_t ms) { return true; }
void t5_ui_unlock(void) {}
bool t5_ui_on_ui_task(void) { return true; }

/* In posting order, as on the device (t5_ui/ui.c): one LVGL call drains a
 * FIFO, since LVGL runs timers made in one frame newest first. */
typedef struct sim_async {
    t5_ui_fn_t fn;
    void *arg;
    struct sim_async *next;
} sim_async_t;
static sim_async_t *s_qh, *s_qt;
static bool s_drain;

static void sim_drain(void *unused)
{
    (void)unused;
    sim_async_t *a = s_qh;
    s_qh = s_qt = NULL;
    s_drain = false;
    while (a) {
        sim_async_t *next = a->next;
        a->fn(a->arg);
        free(a);
        a = next;
    }
}

t5_err_t t5_ui_async(t5_ui_fn_t fn, void *arg)
{
    sim_async_t *a = calloc(1, sizeof *a);
    if (!a) return T5_ERR_NO_MEM;
    if (!s_drain && lv_async_call(sim_drain, NULL) != LV_RESULT_OK) {
        free(a);
        return T5_ERR_NO_MEM;
    }
    s_drain = true;
    a->fn = fn;
    a->arg = arg;
    if (s_qt) s_qt->next = a;
    else s_qh = a;
    s_qt = a;
    return T5_OK;
}

t5_err_t t5_ui_sync(t5_ui_fn_t fn, void *arg, uint32_t timeout_ms)
{
    fn(arg);
    return T5_OK;
}

t5_rotation_t t5_ui_rotation(void) { return s_rot; }

t5_err_t t5_ui_set_rotation(t5_rotation_t r)
{
    if (r == s_rot) return T5_OK;
    s_rot = r;
    int32_t w, h;
    t5_rot_size(r, &w, &h);
    lv_display_set_resolution(lv_display_get_default(), w, h);
    if (s_hook) s_hook(r, s_hook_arg);
    return T5_OK;
}

void t5_ui_set_auto_rotate(bool on) {}
bool t5_ui_auto_rotate(void) { return false; }
t5_err_t t5_ui_set_rotation_lock(bool locked) { return T5_OK; }
bool t5_ui_rotation_locked(void) { return false; }
bool t5_ui_follows_board(void) { return false; }
t5_err_t t5_ui_set_follows_board(bool on) { return T5_OK; }
void t5_ui_set_allowed_rotations(uint32_t mask) { s_allowed = mask ? mask : T5_ROT_ALL; }
uint32_t t5_ui_allowed_rotations(void) { return s_allowed; }
void t5_ui_set_rotate_hook(t5_ui_rotate_hook_t fn, void *arg) { s_hook = fn; s_hook_arg = arg; }
void t5_ui_get_stats(t5_ui_stats_t *out) { *out = s_stats; }
void t5_ui_show_stats(bool on) {}
bool t5_ui_low_power(void) { return false; }
bool t5_ui_hw_keyboard(void) { return false; }
bool t5_ui_settled(void) { return true; }     /* one framebuffer, always current */
uint32_t t5_ui_stalled_ms(void) { return 0; }
t5_err_t t5_ui_overlay_show(const uint16_t *img, int32_t w, int32_t h, int32_t x, int32_t y) { return T5_ERR_NOT_SUPPORTED; }
t5_err_t t5_ui_overlay_restore(void) { return T5_OK; }
bool t5_ui_touch_peek(int32_t *x, int32_t *y) { return false; }
int t5_ui_touch_points(int32_t *x, int32_t *y, int max) { return 0; }
void t5_ui_reveal(void) {}
