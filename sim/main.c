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
#include "md/md_diag.h"
#include "md/md_icons.h"
#include "sim.h"
#include "t5_shell/standby.h"
#include "t5_ui/t5_ui.h"
#include "t5_shell/shell.h"
#include "shell_priv.h"

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

typedef enum { SCENE_GATE, SCENE_GATE_DIALOG, SCENE_LAUNCHER, SCENE_QS, SCENE_WIDGETS, SCENE_DRAWER, SCENE_MENU, SCENE_APPS,
               SCENE_APP_BAR, SCENE_FULLSCREEN, SCENE_STANDBY } scene_t;

typedef struct {
    const char *name;
    t5_rotation_t rot;
    scene_t scene;
} shot_t;

/* ---- the stand-in app page: every Phase 6.1 widget ------------------------- */
void sa_sim_fill(lv_obj_t *body)
{
    md_section_create(body, "Controls");
    lv_obj_t *r = md_row_create(body, MD_ICON_WIFI, "Switch", "With a secondary line");
    md_row_switch(r, true);
    r = md_row_create(body, MD_ICON_BLUETOOTH, "Switch off", NULL);
    md_row_switch(r, false);
    lv_obj_t *box = lv_obj_create(body);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(box, T5_DP(16), 0);
    lv_obj_set_style_pad_gap(box, T5_DP(12), 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_t *s = md_slider_create(box, 0, 100, 40);
    lv_obj_set_width(s, LV_PCT(45));
    md_checkbox_create(box, "Checkbox", true);
    md_checkbox_create(box, "Unchecked", false);
    lv_obj_t *tf = md_textfield_create(box, "Text field", false);
    lv_obj_set_width(tf, LV_PCT(45));
    md_textfield_set_text(tf, "Hello");
    lv_obj_t *tf2 = md_textfield_create(box, "Empty field", false);
    lv_obj_set_width(tf2, LV_PCT(45));
    md_textfield_set_helper(tf2, "Helper text", false);
    md_section_create(body, "Chips");
    lv_obj_t *c = lv_obj_create(body);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(c, T5_DP(16), 0);
    lv_obj_set_style_pad_gap(c, T5_DP(8), 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW_WRAP);
    md_chip_set_selected(md_chip_create(c, "Filter on", NULL, MD_CHIP_FILTER), true);
    md_chip_create(c, "Filter off", NULL, MD_CHIP_FILTER);
    md_chip_set_selected(md_chip_create(c, "Choice", MD_ICON_PALETTE, MD_CHIP_CHOICE), true);
    md_chip_create(c, "Action", MD_ICON_SHARE, MD_CHIP_ACTION);
    lv_obj_t *br = lv_obj_create(body);
    lv_obj_remove_style_all(br);
    lv_obj_set_size(br, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(br, T5_DP(16), 0);
    lv_obj_set_style_pad_gap(br, T5_DP(8), 0);
    lv_obj_set_flex_flow(br, LV_FLEX_FLOW_ROW);
    md_button_create(br, "Contained", MD_BUTTON_CONTAINED);
    md_button_create(br, "Outlined", MD_BUTTON_OUTLINED);
    md_button_create(br, "Text", MD_BUTTON_TEXT);
}

static int shots(const char *dir, const char *check, const char *update)
{
    static const shot_t list[] = {
        { "gate_land", T5_ROT_90, SCENE_GATE },
        { "gate_land_dialog", T5_ROT_90, SCENE_GATE_DIALOG },
        { "gate_port", T5_ROT_0, SCENE_GATE },
        { "gate_port_dialog", T5_ROT_0, SCENE_GATE_DIALOG },
        { "shell_launcher_land", T5_ROT_90, SCENE_LAUNCHER },
        { "shell_launcher_port", T5_ROT_0, SCENE_LAUNCHER },
        { "shell_qs_land", T5_ROT_90, SCENE_QS },
        { "shell_widgets_land", T5_ROT_90, SCENE_WIDGETS },
        { "shell_drawer_land", T5_ROT_90, SCENE_DRAWER },
        { "shell_menu_port", T5_ROT_0, SCENE_MENU },
        { "shell_apps_land", T5_ROT_90, SCENE_APPS },
        { "shell_apps_port", T5_ROT_0, SCENE_APPS },
        { "shell_appbar_land", T5_ROT_90, SCENE_APP_BAR },
        { "shell_appbar_port", T5_ROT_0, SCENE_APP_BAR },
        { "shell_fullscreen_land", T5_ROT_90, SCENE_FULLSCREEN },
        { "shell_standby_land", T5_ROT_90, SCENE_STANDBY },
        { "shell_standby_port", T5_ROT_0, SCENE_STANDBY },
    };
    sim_fixed_time = true;
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
        if (s->scene == SCENE_GATE || s->scene == SCENE_GATE_DIALOG) {
            lv_obj_t *gate = md_gate_create();
            lv_screen_load(gate);
            run_virtual(500);
            if (s->scene == SCENE_GATE_DIALOG) {
                md_gate_run(gate, MD_GATE_DIALOG);   /* opens it on the first tick */
                run_virtual(450);
                md_gate_run(gate, MD_GATE_IDLE);
                run_virtual(400);
            }
        } else {
            static lv_obj_t *home;
            home = shell_home_screen();
            lv_screen_load(home);
            run_virtual(500);
            if (s->scene == SCENE_QS) t5_shell_quick_settings(true);
            if (s->scene == SCENE_WIDGETS) sim_show_widgets();
            if (s->scene == SCENE_APP_BAR) sim_show_app(true, false);
            if (s->scene == SCENE_FULLSCREEN) sim_show_app(false, true);   /* the handle peeks at first */
            if (s->scene == SCENE_STANDBY) T5_IGNORE(t5_shell_standby(true));
            if (s->scene == SCENE_APPS) {
                shell_note_launch(T5_APP_ID_CAMERA);
                shell_note_launch(T5_APP_ID_FILES);
                launcher_tick();
                launcher_drawer_open();
            }
            if (s->scene == SCENE_MENU) {
                static const char *items[] = { "Refresh", "Format this drive", "Delete this folder" };
                lv_obj_t *anchor = lv_obj_create(lv_layer_top());
                lv_obj_remove_style_all(anchor);
                lv_obj_set_size(anchor, T5_DP(48), T5_DP(48));
                lv_obj_align(anchor, LV_ALIGN_TOP_RIGHT, 0, T5_DP(24));
                md_menu_show(anchor, items, 3, NULL, NULL);
            }
            if (s->scene == SCENE_DRAWER) {
                lv_obj_t *d = md_drawer_open("Tab5 OS", "Material-inspired");
                md_drawer_add_item(d, MD_ICON_HOME, "Home", true, NULL, NULL);
                md_drawer_add_item(d, MD_ICON_SETTINGS, "Settings", false, NULL, NULL);
                md_drawer_add_item(d, MD_ICON_FOLDER, "Files", false, NULL, NULL);
            }
            run_virtual(600);
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
        if (s->scene >= SCENE_LAUNCHER) {
            shell_reset();
            sim_shell_sys_reset();
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
    bool shell = false;
    int seconds = 10;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rot") && i + 1 < argc) rot = parse_rot(argv[++i]);
        else if (!strcmp(argv[i], "--gate") && i + 1 < argc) gate = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shots") && i + 1 < argc) shot_dir = argv[++i];
        else if (!strcmp(argv[i], "--check") && i + 1 < argc) check = argv[++i];
        else if (!strcmp(argv[i], "--update") && i + 1 < argc) update = argv[++i];
        else if (!strcmp(argv[i], "--shell")) shell = true;
        else {
            fprintf(stderr, "usage: t5sim [--rot deg] [--shell | --gate scroll|transition|dialog [--seconds N]]\n"
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
    if (shell) {
        lv_screen_load(shell_home_screen());
        lv_obj_t *b = md_button_create(lv_layer_sys(), "Services", MD_BUTTON_OUTLINED);
        lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -T5_DP(8), -T5_DP(8));
        lv_obj_add_event_cb(b, services, LV_EVENT_CLICKED, NULL);
        for (;;) {
            uint32_t next = lv_timer_handler();
            usleep((next > 5 ? 5 : next) * 1000);
        }
    }
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
