/* SPDX-License-Identifier: Apache-2.0
 * ui: display pipeline, rotation and the rendering gate (plan 4.2, 4.8, 4.9).
 *
 *   ui                         rotation, lock, frame counters
 *   ui rot 0|90|180|270        rotate now
 *   ui lock on|off             rotation lock (persists)
 *   ui auto on|off             auto-rotate from the IMU
 *   ui imu [s]                 orientation filter readout
 *   ui stats on|off            on-screen counters
 *   ui accent <name>           MD2 accent (purple, teal, blue...)
 *   ui gate [mode] [s]         run the rendering gate and report pass/fail
 */
#include <stdio.h>
#include <string.h>
#include "esp_console.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "md/md.h"
#include "t5_display/t5_display.h"
#include "t5_ui/t5_ui.h"
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
        printf("  %2u s: %2u fps, render avg %5u us max %6u us, copy %5u us, %6u KB/s\n", (unsigned)(i + 1),
               (unsigned)s.fps, (unsigned)s.render_us_avg, (unsigned)s.render_us_max, (unsigned)s.copy_us_avg,
               (unsigned)s.copy_kb_s);
    }
    if (!keep) t5_ui_sync(gate_stop, NULL, 2000);
    uint32_t target = mode == MD_GATE_SCROLL ? 57 : 30;
    bool pass = n && min_fps >= target;
    printf("gate %s, rotation %s: min %u fps, avg %u fps, render max %u us, loop max %u ms -> %s (target %u fps)\n",
           md_gate_mode_name(mode), t5_rot_name(t5_ui_rotation()), (unsigned)(n ? min_fps : 0),
           (unsigned)(n ? sum / n : 0), (unsigned)render_max, (unsigned)loop_max, pass ? "PASS" : "FAIL",
           (unsigned)target);
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
    if (!strcmp(c, "rot") && argc == 3) {
        uint32_t deg;
        if (!diag_parse_u32(argv[2], 0, 270, &deg) || deg % 90) goto usage;
        t5_ui_lock();
        t5_err_t e = t5_ui_set_rotation((t5_rotation_t)(deg / 90));
        t5_ui_unlock();
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
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
           "           gate [all|scroll|transition|dialog|show|stop] [s]]\n");
    return 1;
}

void diag_register_ui(void)
{
    const esp_console_cmd_t c = { .command = "ui", .help = "Display pipeline, rotation, rendering gate",
                                  .hint = "[rot|lock|auto|imu|stats|accent|gate]", .func = cmd_ui };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}
