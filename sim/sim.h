/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "t5_ui/rotation.h"

typedef struct {
    int battery;
    bool charging;
    bool wifi;
    int rssi;
    bool mic;             /* an app records: the status bar's privacy dot */
} sim_state_t;
extern sim_state_t g_sim;

void sim_services_toggle(void);
void sim_ui_attach(lv_display_t *d, t5_rotation_t r);
/* RGB565 -> PNG (stored deflate, no zlib needed). */
int sim_png_write(const char *path, const uint16_t *px, int w, int h, int stride_px);
uint64_t sim_hash(const uint16_t *px, int w, int h, int stride_px);

/* Shell in the simulator (sim_shell_sys.c, sim_main shell mode). */
extern bool sim_fixed_time;
void sa_sim_fill(lv_obj_t *body);      /* widgets for the stand-in app page */
/* Every widget on a stand-in page, in front like an app, though no app. */
#define SIM_WIDGETS_PAGE "com.t5.widgets"
void sim_show_widgets(void);
void sim_show_app(bool app_bar, bool fullscreen);
void sim_shell_sys_reset(void);   /* a stand-in app with the shell's bars */
