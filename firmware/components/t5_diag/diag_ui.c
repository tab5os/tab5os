/* SPDX-License-Identifier: Apache-2.0
 * ui: display pipeline, rotation and the rendering gate (plan 4.2, 4.8, 4.9).
 *
 *   ui                         rotation, lock, frame counters
 *   ui rot 0|90|180|270        rotate now
 *   ui lock on|off             rotation lock (persists)
 *   ui auto on|off             auto-rotate from the IMU
 *   ui imu [s]                 orientation filter readout
 *   ui tap x y [ms]            a touch at panel-logical pixels
 *   ui swipe x0 y0 x1 y1 [ms]  a drag (scrolling), default 300 ms
 *   ui stats on|off            on-screen counters
 *   ui accent <name>           MD2 accent (purple, teal, blue...)
 *   ui gate [mode] [s]         run the rendering gate and report pass/fail
 *   ui mem [reset|check|heap]  where LVGL's allocations land; heap integrity
 *   ui freeze [on|off|<mask>]  snapshot compositing for transitions/dialogs
 *   ui frames                  the last 32 frames: render time, PPA time, area
 *   ui slowmo N                animations N times slower (1 = normal), to look at them
 *   ui dialog open|close       one test dialog on the current screen
 *   ui ppafill on|off          solid fills on the PPA or in software (test)
 *   ui ppascrim on|off         translucent fills (scrims) on the PPA or not (test)
 *   ui ppafilltest             translucent fills: PPA against software
 *   ui eat <KB>|free           hold internal RAM (test: a busy system)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "t5_power/power.h"
#include "md/md.h"
#include "t5_display/t5_display.h"
#include "t5_ui/t5_ui.h"
#include "t5_shell/shell.h"
#include "diag_priv.h"

void t5_ui_orient_debug(int16_t *angle, bool *flat, t5_rotation_t *pending);

static void print_status(void)
{
    t5_ui_stats_t s;
    t5_ui_get_stats(&s);
    int32_t w, h;
    t5_rot_size(t5_ui_rotation(), &w, &h);
    printf("rotation %s (%dx%d), lock %s, auto %s, allowed 0x%x, low-power %s\n", t5_rot_name(t5_ui_rotation()),
           (int)w, (int)h, t5_ui_rotation_locked() ? "on" : "off", t5_ui_auto_rotate() ? "on" : "off",
           (unsigned)t5_ui_allowed_rotations(), t5_ui_low_power() ? "on" : "off");
    printf("frames %u (panel %u), %u fps, render avg %u us max %u us, copy avg %u us, %u KB/s, loop max %u ms, "
           "touch->flush %u us, refresh %u ms\n",
           (unsigned)s.frames, (unsigned)s.panel_frames, (unsigned)s.fps, (unsigned)s.render_us_avg,
           (unsigned)s.render_us_max, (unsigned)s.copy_us_avg, (unsigned)s.copy_kb_s, (unsigned)s.ui_loop_max_ms,
           (unsigned)s.touch_to_flush_us, (unsigned)s.refresh_period_ms);
}

typedef struct {
    md_gate_mode_t mode;
} gate_arg_t;

static lv_obj_t *s_gate, *s_prev;

static void gate_start(void *p)
{
    gate_arg_t *a = p;
    if (!s_gate) {
        s_prev = lv_screen_active();
        s_gate = md_gate_create();
        lv_screen_load(s_gate);
    }
    md_gate_run(s_gate, a->mode);
}

static void gate_stop(void *p)
{
    (void)p;
    if (!s_gate) return;
    md_gate_run(s_gate, MD_GATE_IDLE);
    if (s_prev) lv_screen_load(s_prev);
    lv_obj_delete(s_gate);
    s_gate = NULL;
}

/* Targets from the design: 60 fps scrolling lists, 30 fps full-screen
 * transitions. The panel refreshes at 64.4 Hz, so "60" means at least 57
 * refreshes per second sustained (one dropped frame per second allowed). */
static int run_gate(md_gate_mode_t mode, uint32_t secs, bool keep)
{
    gate_arg_t a = { mode };
    if (t5_ui_sync(gate_start, &a, 2000) != T5_OK) {
        printf("UI not running\n");
        return 1;
    }
    vTaskDelay(pdMS_TO_TICKS(1500));            /* settle, first frames */
    md_gate_anim_stats_t as;
    t5_ui_lock();
    md_gate_anim_stats(NULL, true);
    t5_ui_unlock();
    uint32_t min_fps = UINT32_MAX, sum = 0, n = 0, render_max = 0, loop_max = 0;
    for (uint32_t i = 0; i < secs; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        t5_ui_stats_t s;
        t5_ui_get_stats(&s);
        if (s.fps < min_fps) min_fps = s.fps;
        sum += s.fps;
        n++;
        if (s.render_us_max > render_max) render_max = s.render_us_max;
        if (s.ui_loop_max_ms > loop_max) loop_max = s.ui_loop_max_ms;
        printf("  %2u s: %2u fps, render avg %5u us max %6u us (%4u kpx), copy %5u us, %6u KB/s\n",
               (unsigned)(i + 1), (unsigned)s.fps, (unsigned)s.render_us_avg, (unsigned)s.render_us_max,
               (unsigned)s.render_kpx_avg, (unsigned)s.copy_us_avg, (unsigned)s.copy_kb_s);
    }
    t5_ui_lock();
    md_gate_anim_stats(&as, false);
    t5_ui_unlock();
    if (!keep) t5_ui_sync(gate_stop, NULL, 2000);
    /* Scrolling: every second must make the target. Transitions and dialogs
     * (plan 4.8: "30 fps during full-screen transitions"): the frame rate
     * while animating; the per-second figure includes the idle time
     * between the gate's steps. */
    uint32_t target = mode == MD_GATE_SCROLL ? 57 : 30;
    uint32_t anim_fps10 = as.ms ? as.frames * 10000 / as.ms : 0;
    bool pass = mode == MD_GATE_SCROLL ? n && min_fps >= target : as.frames && anim_fps10 >= target * 10;
    if (mode != MD_GATE_SCROLL)
        printf("  animating: %u frames in %u ms = %u.%u fps; start latency max %u ms\n", (unsigned)as.frames,
               (unsigned)as.ms, (unsigned)(anim_fps10 / 10), (unsigned)(anim_fps10 % 10), (unsigned)as.start_ms_max);
    printf("gate %s, rotation %s: min %u fps, avg %u fps, render max %u us, loop max %u ms -> %s (target %u fps%s)\n",
           md_gate_mode_name(mode), t5_rot_name(t5_ui_rotation()), (unsigned)(n ? min_fps : 0),
           (unsigned)(n ? sum / n : 0), (unsigned)render_max, (unsigned)loop_max, pass ? "PASS" : "FAIL",
           (unsigned)target, mode == MD_GATE_SCROLL ? "" : " while animating");
    return pass ? 0 : 1;
}

static void set_accent(void *p)
{
    md_accent_t a = (md_accent_t)(intptr_t)p;
    md_set_accent(a, a == MD_ACCENT_TEAL ? MD_ACCENT_PURPLE : MD_ACCENT_TEAL);
}

static void stall_ui(void *p)
{
    /* Busy, like a hung app: no yielding, no LVGL. */
    int64_t until = esp_timer_get_time() + (int64_t)(intptr_t)p * 1000;
    while (esp_timer_get_time() < until) {
    }
}

static void set_freeze(void *p)
{
    md_freeze_set_enabled(p != NULL);
}

/* ui freeze test <s>: the active screen drawn from its snapshot, redrawn in
 * full every frame (the cost of compositing alone). */
static lv_timer_t *s_ftest;
static void ftest_tick(lv_timer_t *t)
{
    (void)t;
    lv_obj_invalidate(lv_layer_bottom());
}
static void ftest_start(void *p)
{
    (void)p;
    if (md_freeze_screen(lv_screen_active())) s_ftest = lv_timer_create(ftest_tick, 5, NULL);
}
static void ftest_stop(void *p)
{
    (void)p;
    if (s_ftest) lv_timer_delete(s_ftest);
    s_ftest = NULL;
    md_thaw_screens();
}

static lv_obj_t *s_kbta;
static void kb_test(void *p)
{
    if (p) {
        t5_shell_keyboard_hide();
        if (s_kbta) lv_obj_delete(s_kbta);
        s_kbta = NULL;
        return;
    }
    if (!s_kbta) {
        s_kbta = lv_textarea_create(lv_screen_active());
        lv_obj_set_width(s_kbta, LV_PCT(60));
        lv_obj_align(s_kbta, LV_ALIGN_TOP_MID, 0, T5_DP(80));
    }
    t5_shell_keyboard_show(s_kbta);
}

static void kb_dump(void *p)
{
    (void)p;
    lv_obj_t *top = lv_layer_top();
    lv_obj_update_layout(top);
    for (uint32_t i = 0; i < lv_obj_get_child_count(top); i++) {
        lv_obj_t *o = lv_obj_get_child(top, (int32_t)i);
        lv_area_t a;
        lv_obj_get_coords(o, &a);
        printf("  top[%u] %s %d,%d..%d,%d hidden %d opa %u bg_opa %u translate_y %d\n", (unsigned)i,
               lv_obj_check_type(o, &lv_keyboard_class) ? "keyboard" : "obj", (int)a.x1, (int)a.y1, (int)a.x2,
               (int)a.y2, lv_obj_is_hidden(o), lv_obj_get_style_opa(o, 0), lv_obj_get_style_bg_opa(o, 0),
               (int)lv_obj_get_style_translate_y(o, 0));
    }
    lv_area_t a;
    lv_obj_get_coords(top, &a);
    printf("  top layer %d,%d..%d,%d hidden %d\n", (int)a.x1, (int)a.y1, (int)a.x2, (int)a.y2, lv_obj_is_hidden(top));
}

/* ui ppatest: draw faded RGB565 and ARGB8888 images with the PPA's alpha
 * path (ppa_img_t5.c) and in software, and compare the pixels. */
#define PT_W 64
#define PT_H 48
static void pt_src(lv_draw_buf_t *b)
{
    for (int y = 0; y < PT_H; y++) {
        uint8_t *row = b->data + y * b->header.stride;
        for (int x = 0; x < PT_W; x++) {
            if (b->header.cf == LV_COLOR_FORMAT_RGB565) {
                ((uint16_t *)row)[x] = (uint16_t)(((x * 31 / 63) << 11) | ((y * 63 / (PT_H - 1)) << 5) | ((x + y) & 31));
            } else {
                uint8_t *p = row + x * 4;      /* B, G, R, A */
                p[0] = 128; p[1] = (uint8_t)(255 - x * 4); p[2] = (uint8_t)(y * 5); p[3] = (uint8_t)(x * 255 / 63);
            }
        }
    }
}

static void pt_dst(lv_draw_buf_t *b)
{
    for (int y = 0; y < PT_H; y++) {
        uint16_t *row = (uint16_t *)(b->data + y * b->header.stride);
        for (int x = 0; x < PT_W; x++) row[x] = (uint16_t)(0x39e7 ^ (x * 977 + y * 131));
    }
}

static void pt_run(void *p)
{
    int *out = p;           /* max channel difference per case, or -1 */
    lv_draw_buf_t *src[2] = { lv_draw_buf_create(PT_W, PT_H, LV_COLOR_FORMAT_RGB565, 0),
                              lv_draw_buf_create(PT_W, PT_H, LV_COLOR_FORMAT_ARGB8888, 0) };
    lv_draw_buf_t *dst[2] = { lv_draw_buf_create(PT_W, PT_H, LV_COLOR_FORMAT_RGB565, 0),
                              lv_draw_buf_create(PT_W, PT_H, LV_COLOR_FORMAT_RGB565, 0) };
    lv_obj_t *cv = lv_canvas_create(lv_layer_top());
    static const lv_opa_t opas[] = { 128, 200, 255 };
    for (int i = 0; i < 6; i++) out[i] = -1;
    if (src[0] && src[1] && dst[0] && dst[1] && cv) {
        lv_obj_set_hidden(cv, true);
        pt_src(src[0]);
        pt_src(src[1]);
        lv_draw_buf_flush_cache(src[0], NULL);    /* the PPA reads memory, not the cache */
        lv_draw_buf_flush_cache(src[1], NULL);
        for (int c = 0; c < 6; c++) {
            lv_draw_buf_t *s = src[c / 3];
            lv_opa_t opa = opas[c % 3];
            for (int m = 0; m < 2; m++) {
                /* m 0: PPA (alpha path, or LVGL's own for c == 2); m 1: software */
                t5_ui_set_ppa_alpha(m == 0);
                t5_ui_set_ppa_images(m == 0);
                pt_dst(dst[m]);
                lv_canvas_set_draw_buf(cv, dst[m]);
                lv_layer_t layer;
                lv_canvas_init_layer(cv, &layer);
                lv_draw_image_dsc_t d;
                lv_draw_image_dsc_init(&d);
                d.src = s;
                d.opa = opa;
                lv_area_t a = { 0, 0, PT_W - 1, PT_H - 1 };
                lv_draw_image(&layer, &d, &a);
                lv_canvas_finish_layer(cv, &layer);   /* LVGL invalidates after PPA tasks */
            }
            int worst = 0;
            for (int y = 0; y < PT_H; y++) {
                const uint16_t *r0 = (const uint16_t *)(dst[0]->data + y * dst[0]->header.stride);
                const uint16_t *r1 = (const uint16_t *)(dst[1]->data + y * dst[1]->header.stride);
                for (int x = 0; x < PT_W; x++) {
                    int dr = abs((r0[x] >> 11) - (r1[x] >> 11)), dg = abs(((r0[x] >> 5) & 63) - ((r1[x] >> 5) & 63)),
                        db = abs((r0[x] & 31) - (r1[x] & 31));
                    int w = dr > dg ? dr : dg;
                    if (db > w) w = db;
                    if (w > worst) worst = w;
                }
            }
            out[c] = worst;
            const uint16_t *a0 = (const uint16_t *)(dst[0]->data + 10 * dst[0]->header.stride);
            const uint16_t *a1 = (const uint16_t *)(dst[1]->data + 10 * dst[1]->header.stride);
            const uint16_t *sr = (const uint16_t *)(src[0]->data + 10 * src[0]->header.stride);
            printf("  case %d row 10: ppa %04x %04x %04x %04x  sw %04x %04x %04x %04x  src565 %04x %04x\n", c,
                   a0[0], a0[1], a0[20], a0[40], a1[0], a1[1], a1[20], a1[40], sr[0], sr[20]);
        }
    }
    t5_ui_set_ppa_alpha(true);
    t5_ui_set_ppa_images(true);
    if (cv) lv_obj_delete(cv);
    for (int i = 0; i < 2; i++) {
        if (src[i]) lv_draw_buf_destroy(src[i]);
        if (dst[i]) lv_draw_buf_destroy(dst[i]);
    }
}

/* ui ppafilltest: translucent fills drawn by the PPA's blender against the
 * same fills in software. 128 x 64 so the fill clears the PPA's size floor. */
#define PF_W 128
#define PF_H 64
static void pf_run(void *p)
{
    int *out = p;
    static const lv_opa_t opas[] = { 30, 82, 128, 220, 82, 200 };
    lv_draw_buf_t *dst[2] = { lv_draw_buf_create(PF_W, PF_H, LV_COLOR_FORMAT_RGB565, 0),
                              lv_draw_buf_create(PF_W, PF_H, LV_COLOR_FORMAT_RGB565, 0) };
    lv_obj_t *cv = lv_canvas_create(lv_layer_top());
    for (int i = 0; i < 6; i++) out[i] = -1;
    if (dst[0] && dst[1] && cv) {
        lv_obj_set_hidden(cv, true);
        for (int c = 0; c < 6; c++) {
            for (int m = 0; m < 2; m++) {
                t5_ui_set_ppa_alpha_fills(m == 0);
                for (int y = 0; y < PF_H; y++) {
                    uint16_t *row = (uint16_t *)(dst[m]->data + y * dst[m]->header.stride);
                    for (int x = 0; x < PF_W; x++) row[x] = (uint16_t)(0x39e7 ^ (x * 977 + y * 131));
                }
                lv_draw_buf_flush_cache(dst[m], NULL);
                lv_canvas_set_draw_buf(cv, dst[m]);
                lv_layer_t layer;
                lv_canvas_init_layer(cv, &layer);
                lv_draw_rect_dsc_t r;
                lv_draw_rect_dsc_init(&r);
                r.bg_color = lv_color_hex(c & 1 ? 0x000000 : 0x6200EE);
                r.bg_opa = opas[c];
                r.radius = c >= 4 ? 12 : 0;   /* rounded: corner rows in software, the rest on the PPA */
                lv_area_t a = { 2, 3, PF_W - 3, PF_H - 2 };
                lv_draw_rect(&layer, &r, &a);
                lv_canvas_finish_layer(cv, &layer);
            }
            int worst = 0;
            for (int y = 0; y < PF_H; y++) {
                const uint16_t *r0 = (const uint16_t *)(dst[0]->data + y * dst[0]->header.stride);
                const uint16_t *r1 = (const uint16_t *)(dst[1]->data + y * dst[1]->header.stride);
                for (int x = 0; x < PF_W; x++) {
                    int dr = abs((r0[x] >> 11) - (r1[x] >> 11)), dg = abs(((r0[x] >> 5) & 63) - ((r1[x] >> 5) & 63)),
                        db = abs((r0[x] & 31) - (r1[x] & 31));
                    int w = dr > dg ? dr : dg;
                    if (db > w) w = db;
                    if (w > worst) worst = w;
                }
            }
            out[c] = worst;
        }
    }
    t5_ui_set_ppa_alpha_fills(true);
    if (cv) lv_obj_delete(cv);
    for (int i = 0; i < 2; i++) if (dst[i]) lv_draw_buf_destroy(dst[i]);
}

/* ui dialog open|close: one test dialog on the current screen, to step
 * through a dialog's frames (`ui frames`) one action at a time. */
static lv_obj_t *s_test_dialog;
static void test_dialog_gone(lv_event_t *e)
{
    (void)e;
    s_test_dialog = NULL;
}

static void test_dialog(void *p)
{
    bool open = p != NULL;
    if (open && !s_test_dialog) {
        s_test_dialog = md_dialog_show("Use location service?",
                                       "Let apps find your location. This sends anonymous data even when no apps are running.",
                                       "Agree", "Disagree", NULL, NULL);
        if (s_test_dialog) lv_obj_add_event_cb(s_test_dialog, test_dialog_gone, LV_EVENT_DELETE, NULL);
    } else if (!open && s_test_dialog) {
        md_dialog_close(s_test_dialog);
    }
}

/* ---- scripted touch: ui tap X Y [ms] (logical coordinates) ------------------------------------ */
static lv_indev_t *s_tap_indev;
static volatile int32_t s_tap_x, s_tap_y;
static volatile uint32_t s_tap_until;     /* lv_tick while pressed; 0 = released */

static void tap_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = s_tap_x;
    data->point.y = s_tap_y;
    data->state = s_tap_until && lv_tick_get() < s_tap_until ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if (data->state == LV_INDEV_STATE_RELEASED) s_tap_until = 0;
}

static void tap_start(void *arg)
{
    uint32_t ms = (uint32_t)(uintptr_t)arg;
    if (!s_tap_indev) {
        s_tap_indev = lv_indev_create();
        lv_indev_set_type(s_tap_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_tap_indev, tap_read);
    }
    s_tap_until = lv_tick_get() + ms;
}

/* ---- scripted keys: as a USB keyboard would send them ----------------------------------------- */
typedef struct {
    const char *text;
    uint32_t key;
} keys_t;

static void send_keys(void *arg)
{
    keys_t *k = arg;
    lv_group_t *g = lv_group_get_default();
    if (!g) return;
    if (k->text) {
        for (const char *p = k->text; *p; p++) lv_group_send_data(g, (uint32_t)(unsigned char)*p);
    } else {
        lv_group_send_data(g, k->key);
    }
}

static int cmd_ui(int argc, char **argv)
{
    if (!t5_ui_ready()) {
        printf("UI not running\n");
        return 1;
    }
    if (argc == 1) {
        print_status();
        return 0;
    }
    const char *c = argv[1];
    if (!strcmp(c, "tap") && argc >= 4) {
        uint32_t x, y, ms = 80;
        if (!diag_parse_u32(argv[2], 0, 4000, &x) || !diag_parse_u32(argv[3], 0, 4000, &y)) goto usage;
        if (argc >= 5 && !diag_parse_u32(argv[4], 10, 5000, &ms)) goto usage;
        t5_power_user_activity();
        s_tap_x = (int32_t)x;
        s_tap_y = (int32_t)y;
        t5_ui_sync(tap_start, (void *)(uintptr_t)ms, 2000);
        vTaskDelay(pdMS_TO_TICKS(ms + 60));
        printf("tapped %u,%u for %u ms\n", (unsigned)x, (unsigned)y, (unsigned)ms);
        return 0;
    }
    if (!strcmp(c, "swipe") && argc >= 6) {
        uint32_t x0, y0, x1, y1, ms = 300;
        if (!diag_parse_u32(argv[2], 0, 4000, &x0) || !diag_parse_u32(argv[3], 0, 4000, &y0) ||
            !diag_parse_u32(argv[4], 0, 4000, &x1) || !diag_parse_u32(argv[5], 0, 4000, &y1)) goto usage;
        if (argc >= 7 && !diag_parse_u32(argv[6], 50, 5000, &ms)) goto usage;
        t5_power_user_activity();
        s_tap_x = (int32_t)x0;
        s_tap_y = (int32_t)y0;
        /* Pressed for the whole move plus a moment, so the release lands at the end point. */
        t5_ui_sync(tap_start, (void *)(uintptr_t)(ms + 40), 2000);
        const int steps = 20;
        for (int i = 1; i <= steps; i++) {
            vTaskDelay(pdMS_TO_TICKS(ms / steps));
            s_tap_x = (int32_t)x0 + ((int32_t)x1 - (int32_t)x0) * i / steps;
            s_tap_y = (int32_t)y0 + ((int32_t)y1 - (int32_t)y0) * i / steps;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        printf("swiped %u,%u -> %u,%u in %u ms\n", (unsigned)x0, (unsigned)y0, (unsigned)x1, (unsigned)y1, (unsigned)ms);
        return 0;
    }
    if (!strcmp(c, "type") && argc >= 3) {
        char buf[256];
        size_t n = 0;
        for (int i = 2; i < argc && n < sizeof buf - 2; i++) {
            n += (size_t)snprintf(buf + n, sizeof buf - n, "%s%s", i > 2 ? " " : "", argv[i]);
        }
        keys_t k = { buf, 0 };
        t5_power_user_activity();
        t5_ui_sync(send_keys, &k, 2000);
        return 0;
    }
    if (!strcmp(c, "key") && argc == 3) {
        static const struct { const char *name; uint32_t key; } names[] = {
            { "enter", LV_KEY_ENTER }, { "up", LV_KEY_UP }, { "down", LV_KEY_DOWN }, { "left", LV_KEY_LEFT },
            { "right", LV_KEY_RIGHT }, { "tab", LV_KEY_NEXT }, { "bs", LV_KEY_BACKSPACE }, { "del", LV_KEY_DEL },
            { "home", LV_KEY_HOME }, { "end", LV_KEY_END }, { "esc", LV_KEY_ESC },
        };
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
            if (!strcmp(argv[2], names[i].name)) {
                keys_t k = { NULL, names[i].key };
                t5_ui_sync(send_keys, &k, 2000);
                return 0;
            }
        }
        goto usage;
    }
    if (!strcmp(c, "rot") && argc == 3) {
        uint32_t deg;
        if (!diag_parse_u32(argv[2], 0, 270, &deg) || deg % 90) goto usage;
        t5_ui_lock();
        t5_err_t e = t5_ui_set_rotation((t5_rotation_t)(deg / 90));
        t5_ui_unlock();
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
    if (!strcmp(c, "frames")) {
        t5_ui_frame_rec_t f[T5_UI_FRAME_LOG];
        t5_ui_lock();
        int n = t5_ui_frame_log(f, T5_UI_FRAME_LOG);
        t5_ui_unlock();
        for (int i = 0; i < n; i++)
            printf("  %8u ms  +%4u  render %6u us (ppa %5u us in %3u, band wait %5u us, %2u bands, copies %5u us in %2u)"
                   "  end wait %5u us  %4u kpx in %u area%s, first %d,%d-%d,%d\n", (unsigned)f[i].start_ms,
                   i ? (unsigned)(f[i].start_ms - f[i - 1].start_ms) : 0u, (unsigned)f[i].render_us,
                   (unsigned)f[i].ppa_us, (unsigned)f[i].ppa_n, (unsigned)f[i].flush_wait_us, (unsigned)f[i].flushes,
                   (unsigned)f[i].copy_exec_us, (unsigned)f[i].copies, (unsigned)f[i].wait_us,
                   (unsigned)(f[i].px / 1000), (unsigned)f[i].nareas, f[i].nareas == 1 ? "" : "s", f[i].x1, f[i].y1,
                   f[i].x2, f[i].y2);
        return 0;
    }
    if ((!strcmp(c, "lock") || !strcmp(c, "auto") || !strcmp(c, "stats")) && argc == 3) {
        bool on = !strcmp(argv[2], "on");
        if (!on && strcmp(argv[2], "off")) goto usage;
        if (!strcmp(c, "lock")) t5_ui_set_rotation_lock(on);
        else if (!strcmp(c, "auto")) t5_ui_set_auto_rotate(on);
        else t5_ui_show_stats(on);
        print_status();
        return 0;
    }
    if (!strcmp(c, "mem")) {
        if (argc == 3 && !strcmp(argv[2], "reset")) t5_ui_lvmem_reset();
        if (argc == 3 && !strcmp(argv[2], "check")) {
            bool heap = heap_caps_check_integrity_all(true), pool = t5_ui_lvmem_check();
            printf("integrity: heap %s, lvgl pool %s\n", heap ? "ok" : "CORRUPT", pool ? "ok" : "CORRUPT");
            return !(heap && pool);
        }
        if (argc == 3 && !strcmp(argv[2], "heap")) {
            heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
            return 0;
        }
        t5_ui_lvmem_t m;
        t5_ui_lvmem(&m);
        printf("lvgl allocs: pool %u, internal %u, psram %u, failed %u\n", (unsigned)m.pool_allocs,
               (unsigned)m.int_allocs, (unsigned)m.psram_allocs, (unsigned)m.failed);
        printf("  outside pool: %u big (max %u B), %u overflow\n", (unsigned)m.big_allocs, (unsigned)m.big_max,
               (unsigned)m.overflow_allocs);
        printf("  outside pool: internal %d B (peak %d), psram %d B (peak %d)\n", (int)m.int_live, (int)m.int_peak,
               (int)m.psram_live, (int)m.psram_peak);
        printf("  pool %u B, free %u, min free %u\n", (unsigned)m.pool_bytes, (unsigned)m.pool_free,
               (unsigned)m.pool_min_free);
        return 0;
    }
    if (!strcmp(c, "freeze") && argc == 4 && !strcmp(argv[2], "test")) {
        uint32_t secs;
        if (!diag_parse_u32(argv[3], 1, 60, &secs)) goto usage;
        t5_ui_sync(ftest_start, NULL, 2000);
        for (uint32_t i = 0; i < secs; i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            t5_ui_stats_t st;
            t5_ui_get_stats(&st);
            printf("  %2u s: %2u fps, render avg %5u us max %6u us\n", (unsigned)(i + 1), (unsigned)st.fps,
                   (unsigned)st.render_us_avg, (unsigned)st.render_us_max);
        }
        t5_ui_sync(ftest_stop, NULL, 2000);
        return 0;
    }
    if (!strcmp(c, "freeze")) {
        if (argc == 3 && (!strcmp(argv[2], "on") || !strcmp(argv[2], "off")))
            t5_ui_sync(set_freeze, (void *)(intptr_t)!strcmp(argv[2], "on"), 2000);
        else if (argc == 3) md_freeze_set_mode((uint32_t)strtoul(argv[2], NULL, 0));
        md_freeze_stats_t f;
        md_freeze_stats(&f);
        printf("snapshot compositing %s (mask %u): %u transitions (%u fell back), %u snapshots (%u failed), last %u ms, "
               "max %u ms\n",
               md_freeze_mode() ? "on" : "off", (unsigned)md_freeze_mode(), (unsigned)f.transitions, (unsigned)f.fallbacks,
               (unsigned)f.snapshots, (unsigned)f.failed, (unsigned)f.snap_ms_last, (unsigned)f.snap_ms_max);
        t5_ui_lock();
        md_freeze_print();
        t5_ui_unlock();
        printf("last dialog: prep %u ms (box snapshot %u, compose %u), bake %u ms\n", (unsigned)f.dialog_prep_ms_last,
               (unsigned)f.box_snap_ms_last, (unsigned)f.compose_ms_last, (unsigned)f.bake_ms_last);
        return 0;
    }
    if (!strcmp(c, "kbtest")) {    /* a text field and the on-screen keyboard, to look at */
        if (argc == 3 && !strcmp(argv[2], "dump")) {
            t5_ui_sync(kb_dump, NULL, 2000);
            return 0;
        }
        t5_ui_sync(kb_test, (void *)(intptr_t)(argc == 3 && !strcmp(argv[2], "off")), 2000);
        return 0;
    }
    if (!strcmp(c, "ppafilltest")) {
        int r[6];
        if (t5_ui_sync(pf_run, r, 3000) != T5_OK) return 1;
        static const char *const names[] = { "purple opa 30", "black opa 82", "purple opa 128", "black opa 220",
                                             "purple r12 opa 82", "black r12 opa 200" };
        int bad = 0;
        for (int i = 0; i < 6; i++) {
            printf("  %-16s PPA vs software: max diff %d (RGB565 steps)\n", names[i], r[i]);
            if (r[i] < 0 || r[i] > 2) bad++;
        }
        printf("ppafilltest %s\n", bad ? "FAIL" : "PASS");
        return bad ? 1 : 0;
    }
    if (!strcmp(c, "ppatest")) {
        int r[6];
        if (t5_ui_sync(pt_run, r, 3000) != T5_OK) return 1;
        static const char *const names[] = { "RGB565 opa 128", "RGB565 opa 200", "RGB565 opaque", "ARGB8888 opa 128",
                                             "ARGB8888 opa 200", "ARGB8888 opa 255" };
        int bad = 0;
        for (int i = 0; i < 6; i++) {
            printf("  %-18s PPA vs software: max diff %d (RGB565 steps)\n", names[i], r[i]);
            if (r[i] < 0 || r[i] > 2) bad++;
        }
        printf("ppatest %s\n", bad ? "FAIL" : "PASS");
        return bad ? 1 : 0;
    }
    if (!strcmp(c, "dialog") && argc == 3) {
        t5_ui_sync(test_dialog, !strcmp(argv[2], "open") ? (void *)1 : NULL, 2000);
        return 0;
    }
    if (!strcmp(c, "slowmo") && argc == 3) {
        t5_ui_set_slowmo((uint32_t)strtoul(argv[2], NULL, 0));
        return 0;
    }
    if (!strcmp(c, "ppascrim") && argc == 3) {
        t5_ui_set_ppa_alpha_fills(!strcmp(argv[2], "on"));
        return 0;
    }
    if (!strcmp(c, "ppaalpha") && argc == 3) {
        t5_ui_set_ppa_alpha(!strcmp(argv[2], "on"));
        return 0;
    }
    if (!strcmp(c, "ppaimg") && argc == 3) {
        t5_ui_set_ppa_images(!strcmp(argv[2], "on"));
        return 0;
    }
    if (!strcmp(c, "ppafill") && argc == 3) {
        t5_ui_set_ppa_fills(!strcmp(argv[2], "on"));
        printf("PPA fills %s\n", t5_ui_ppa_fills() ? "on" : "off");
        return 0;
    }
    if (!strcmp(c, "eat") && argc == 3) {
        /* Test aid: hold internal RAM in 4 KB pieces to simulate a busy system. */
        static void *eaten[256];
        static int neaten;
        uint32_t kb;
        if (!strcmp(argv[2], "free")) {
            while (neaten) heap_caps_free(eaten[--neaten]);
        } else if (diag_parse_u32(argv[2], 4, 1024, &kb)) {
            for (uint32_t i = 0; i < kb / 4 && neaten < 256; i++) {
                void *p = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                if (!p) break;
                eaten[neaten++] = p;
            }
        } else goto usage;
        printf("holding %d KB; internal free %u\n", neaten * 4,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return 0;
    }
    if (!strcmp(c, "stall") && argc == 3) {
        uint32_t ms;
        if (!diag_parse_u32(argv[2], 1, 30000, &ms)) goto usage;
        t5_ui_async(stall_ui, (void *)(intptr_t)ms);
        printf("UI task busy for %u ms\n", (unsigned)ms);
        return 0;
    }
    if (!strcmp(c, "peek") && argc == 4) {
        /* Logical (x, y) -> the panel pixel in each framebuffer. Reads PSRAM
         * through the cache: invalidate first, the PPA writes behind it. */
        uint32_t x, y;
        int32_t w, h;
        t5_rot_size(t5_ui_rotation(), &w, &h);
        if (!diag_parse_u32(argv[2], 0, (uint32_t)w - 1, &x) || !diag_parse_u32(argv[3], 0, (uint32_t)h - 1, &y)) goto usage;
        int32_t px, py;
        t5_rot_to_panel(t5_ui_rotation(), (int32_t)x, (int32_t)y, &px, &py);
        printf("logical %u,%u = panel %d,%d; scanout fb %d, queued %d, stalled %u ms\n", (unsigned)x, (unsigned)y,
               (int)px, (int)py, t5_display_scanout(), t5_display_queued(), (unsigned)t5_ui_stalled_ms());
        for (int i = 0; i < T5_DISPLAY_FBS; i++) {
            volatile uint16_t *fb = t5_display_fb_n(i);
            printf("  fb %d: 0x%04x\n", i, fb[(size_t)py * T5_DISPLAY_W + (size_t)px]);
        }
        return 0;
    }
    if (!strcmp(c, "imu")) {
        uint32_t secs = 5;
        if (argc == 3 && !diag_parse_u32(argv[2], 1, 300, &secs)) goto usage;
        for (uint32_t i = 0; i < secs * 4; i++) {
            int16_t ang;
            bool flat;
            t5_rotation_t pend;
            t5_ui_orient_debug(&ang, &flat, &pend);
            printf("angle %3d deg, %s, heading for %s, now %s\n", ang, flat ? "flat" : "upright",
                   t5_rot_name(pend), t5_rot_name(t5_ui_rotation()));
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        return 0;
    }
    if (!strcmp(c, "accent") && argc == 3) {
        for (int i = 0; i < MD_ACCENT_COUNT; i++) {
            if (!strcmp(argv[2], md_accent((md_accent_t)i)->name)) {
                t5_ui_sync(set_accent, (void *)(intptr_t)i, 2000);
                return 0;
            }
        }
        goto usage;
    }
    if (!strcmp(c, "gate")) {
        md_gate_mode_t mode = MD_GATE_SCROLL;
        uint32_t secs = 10;
        bool all = argc < 3 || !strcmp(argv[2], "all");
        if (argc >= 3 && !all) {
            if (!strcmp(argv[2], "scroll")) mode = MD_GATE_SCROLL;
            else if (!strcmp(argv[2], "transition")) mode = MD_GATE_TRANSITION;
            else if (!strcmp(argv[2], "dialog")) mode = MD_GATE_DIALOG;
            else if (!strcmp(argv[2], "show")) { t5_ui_sync(gate_start, &(gate_arg_t){ MD_GATE_IDLE }, 2000); return 0; }
            else if (!strcmp(argv[2], "stop")) { t5_ui_sync(gate_stop, NULL, 2000); return 0; }
            else goto usage;
        }
        if (argc >= 4 && !diag_parse_u32(argv[3], 2, 120, &secs)) goto usage;
        if (!all) return run_gate(mode, secs, false);
        int fails = 0;
        fails += run_gate(MD_GATE_SCROLL, secs, true);
        fails += run_gate(MD_GATE_TRANSITION, secs, true);
        fails += run_gate(MD_GATE_DIALOG, secs, false);
        printf("rendering gate: %s\n", fails ? "FAIL" : "PASS");
        return fails != 0;
    }
usage:
    printf("usage: ui [rot 0|90|180|270 | lock on|off | auto on|off | imu [s] | stats on|off | accent <name> |\n"
           "           gate [all|scroll|transition|dialog|show|stop] [s] | frames | mem [reset|check|heap] |\n"
           "           freeze [on|off|<mask>] | dialog open|close | slowmo N | ppafill on|off | ppascrim on|off |\n"
           "           ppatest | ppafilltest | eat <KB>|free]\n");
    return 1;
}

void diag_register_ui(void)
{
    const esp_console_cmd_t c = { .command = "ui", .help = "Display pipeline, rotation, rendering gate",
                                  .hint = "[rot|lock|auto|imu|stats|accent|gate]", .func = cmd_ui };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}
