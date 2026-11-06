/* SPDX-License-Identifier: Apache-2.0
 * Tab5 OS simulator (plan 4.1, 4.8).
 *
 *   t5sim [--rot 0|90|180|270]              interactive, SDL window
 *   t5sim --gate scroll|transition|dialog [--seconds N]
 *                                           run the rendering gate, print fps
 *   t5sim --shots DIR [--check DIR | --update DIR]
 *                                           headless: render the gate in both
 *                                           orientations to PNGs, compare (or
 *                                           record) their hashes
 *
 * The gate's numbers here are desktop numbers; the pass/fail gate is the
 * device run (`ui gate` on the console). The simulator shows the same
 * screens, which is what the screenshot check guards.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include "lvgl.h"
#include "md/md.h"
#include "sim.h"
#include "t5_ui/t5_ui.h"

/* ---- clock: real for the window, virtual (deterministic) for screenshots -- */
static bool s_virtual;
static uint32_t s_vms;

static uint32_t tick(void)
{
    if (s_virtual) return s_vms;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

static t5_rotation_t parse_rot(const char *s)
{
    int d = atoi(s);
    return d == 0 ? T5_ROT_0 : d == 180 ? T5_ROT_180 : d == 270 ? T5_ROT_270 : T5_ROT_90;
}

/* ---- headless screenshots ------------------------------------------------- */
static uint16_t *s_fb;
static int32_t s_w, s_h;

static void mem_flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    lv_display_flush_ready(d);
}

static void run_virtual(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 16) {
        s_vms += 16;
        lv_timer_handler();
    }
}

typedef struct {
    const char *name;
    t5_rotation_t rot;
    bool dialog;
} shot_t;

static int shots(const char *dir, const char *check, const char *update)
{
    static const shot_t list[] = {
        { "gate_land", T5_ROT_90, false },
        { "gate_land_dialog", T5_ROT_90, true },
        { "gate_port", T5_ROT_0, false },
        { "gate_port_dialog", T5_ROT_0, true },
    };
    s_virtual = true;
    int fails = 0;
    lv_init();
    lv_tick_set_cb(tick);
    for (size_t i = 0; i < sizeof list / sizeof list[0]; i++) {
        const shot_t *s = &list[i];
        t5_rot_size(s->rot, &s_w, &s_h);
        s_fb = calloc((size_t)s_w * (size_t)s_h, 2);
        lv_display_t *d = lv_display_create(s_w, s_h);
        lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
        lv_display_set_buffers(d, s_fb, NULL, (uint32_t)(s_w * s_h * 2), LV_DISPLAY_RENDER_MODE_DIRECT);
        lv_display_set_flush_cb(d, mem_flush);
        lv_display_set_default(d);
        if (i == 0) sim_ui_attach(d, s->rot);
        md_init(d);
        lv_obj_t *gate = md_gate_create();
        lv_screen_load(gate);
        run_virtual(500);
        if (s->dialog) {
            md_gate_run(gate, MD_GATE_DIALOG);   /* opens it on the first tick */
            run_virtual(450);
            md_gate_run(gate, MD_GATE_IDLE);
            run_virtual(400);
        }
        lv_obj_invalidate(lv_screen_active());
        run_virtual(100);

        char path[512];
        snprintf(path, sizeof path, "%s/%s.png", dir, s->name);
        sim_png_write(path, s_fb, s_w, s_h, s_w);
        uint64_t h = sim_hash(s_fb, s_w, s_h, s_w);
        printf("%-18s %4dx%-4d %016llx  %s\n", s->name, (int)s_w, (int)s_h, (unsigned long long)h, path);
        if (update) {
            snprintf(path, sizeof path, "%s/%s.hash", update, s->name);
            FILE *f = fopen(path, "w");
            if (f) { fprintf(f, "%016llx\n", (unsigned long long)h); fclose(f); }
        }
        if (check) {
            snprintf(path, sizeof path, "%s/%s.hash", check, s->name);
            FILE *f = fopen(path, "r");
            unsigned long long want = 0;
            if (!f || fscanf(f, "%llx", &want) != 1 || want != h) {
                printf("  MISMATCH against %s (expected %016llx)\n", path, want);
                fails++;
            }
            if (f) fclose(f);
        }
        lv_display_delete(d);
        free(s_fb);
    }
    if (check) printf("screenshots: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}

/* ---- interactive ------------------------------------------------------------ */
static lv_obj_t *s_gate;

static void rotate(lv_event_t *e)
{
    t5_ui_set_rotation((t5_rotation_t)((t5_ui_rotation() + 1) % 4));
}

static void services(lv_event_t *e)
{
    sim_services_toggle();
}

static void mode(lv_event_t *e)
{
    md_gate_run(s_gate, (md_gate_mode_t)(intptr_t)lv_event_get_user_data(e));
}

static void toolbar(void)
{
    lv_obj_t *bar = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, T5_DP(4), 0);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, T5_DP(8), -T5_DP(8));
    static const struct { const char *t; lv_event_cb_t cb; intptr_t ud; } b[] = {
        { "Rotate", rotate, 0 }, { "Services", services, 0 }, { "Scroll", mode, MD_GATE_SCROLL },
        { "Transition", mode, MD_GATE_TRANSITION }, { "Dialog", mode, MD_GATE_DIALOG }, { "Stop", mode, MD_GATE_IDLE },
    };
    for (size_t i = 0; i < sizeof b / sizeof b[0]; i++) {
        lv_obj_t *btn = md_button_create(bar, b[i].t, MD_BUTTON_OUTLINED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x000000), 0);
        lv_obj_add_event_cb(btn, b[i].cb, LV_EVENT_CLICKED, (void *)b[i].ud);
    }
}

int main(int argc, char **argv)
{
    t5_rotation_t rot = T5_ROT_90;
    const char *gate = NULL, *shot_dir = NULL, *check = NULL, *update = NULL;
    int seconds = 10;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rot") && i + 1 < argc) rot = parse_rot(argv[++i]);
        else if (!strcmp(argv[i], "--gate") && i + 1 < argc) gate = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shots") && i + 1 < argc) shot_dir = argv[++i];
        else if (!strcmp(argv[i], "--check") && i + 1 < argc) check = argv[++i];
        else if (!strcmp(argv[i], "--update") && i + 1 < argc) update = argv[++i];
        else {
            fprintf(stderr, "usage: t5sim [--rot deg] [--gate scroll|transition|dialog [--seconds N]]\n"
                            "       t5sim --shots DIR [--check DIR | --update DIR]\n");
            return 2;
        }
    }
    if (shot_dir) return shots(shot_dir, check, update);

    lv_init();
    lv_tick_set_cb(tick);
    int32_t w, h;
    t5_rot_size(rot, &w, &h);
    lv_display_t *d = lv_sdl_window_create(w, h);
    lv_sdl_window_set_title(d, "Tab5 OS simulator");
    lv_sdl_mouse_create();
    sim_ui_attach(d, rot);
    md_init(d);
    s_gate = md_gate_create();
    lv_screen_load(s_gate);
    toolbar();

    md_gate_mode_t m = MD_GATE_IDLE;
    if (gate) {
        m = !strcmp(gate, "transition") ? MD_GATE_TRANSITION : !strcmp(gate, "dialog") ? MD_GATE_DIALOG : MD_GATE_SCROLL;
        md_gate_run(s_gate, m);
    }
    uint32_t start = tick(), last = start;
    uint32_t min_fps = UINT32_MAX, sum = 0, n = 0;
    for (;;) {
        uint32_t next = lv_timer_handler();
        usleep((next > 5 ? 5 : next) * 1000);
        if (!gate) continue;
        uint32_t now = tick();
        if (now - last >= 1000) {
            last = now;
            t5_ui_stats_t s;
            t5_ui_get_stats(&s);
            if (now - start > 1500) {        /* skip the first second */
                if (s.fps < min_fps) min_fps = s.fps;
                sum += s.fps;
                n++;
            }
            printf("%s: %u fps, render avg %u us max %u us\n", gate, s.fps, s.render_us_avg, s.render_us_max);
        }
        if (now - start >= (uint32_t)seconds * 1000 + 1500) {
            printf("sim gate %s: min %u fps, avg %u fps (desktop; the device run is the gate)\n", gate,
                   n ? min_fps : 0, n ? sum / n : 0);
            return 0;
        }
    }
}
