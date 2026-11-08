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
    bool grove_claimed;
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
lv_obj_t *shell_home_screen(void);
void shell_prepare(lv_obj_t *scr, const char *id);
void shell_changed(const char *id);
void shell_sim_reset(void);
