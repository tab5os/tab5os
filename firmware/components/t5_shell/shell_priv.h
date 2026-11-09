/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "lvgl.h"
#include "md/md.h"
#include "md/md_icons.h"
#include "shell_logic.h"
#include "shell_sys.h"
#include "t5_shell/shell.h"

#define SHELL_STATUS_H_DP 24

/* shell.c */
void shell_chrome_init(void);          /* status bar, gestures, keyboard; once, UI task */
bool shell_in_app(void);
void shell_note_launch(const char *id);
int shell_recents(char ids[][T5_APP_ID_LEN], int max);
/* An input device joined LVGL: gestures for it, and the on-screen keyboard
 * goes if it was a keyboard. */
void shell_input_added(void);
/* Forget every object and timer (the simulator deletes its display between
 * screenshots). */
void shell_reset(void);
void launcher_reset(void);
void statusbar_reset(void);
void qs_reset(void);
void keyboard_reset(void);
void gesture_reset(void);
/* Labels are only set when their text changes: LVGL redraws on every set. */
void shell_set_text(lv_obj_t *label, const char *text);
/* An lv_anim exec callback that moves an object vertically. */
void shell_anim_y(void *obj, int32_t y);
/* The clock and the date (either may be NULL), or the "not set" texts. */
void shell_show_time(lv_obj_t *clock, lv_obj_t *date);
/* statusbar.c */
lv_obj_t *statusbar_create(void);
void statusbar_update(void);
void statusbar_set_hidden(bool hidden);
void statusbar_set_in_app(bool in_app);
/* qs.c */
void qs_open(void);
void qs_close(void);
bool qs_is_open(void);
/* launcher.c */
lv_obj_t *launcher_get(void);
void launcher_reload(void);
void launcher_tick(void);
/* The app drawer over the home screen. */
void launcher_drawer_open(void);
void launcher_drawer_close(bool animate);
bool launcher_drawer_is_open(void);
/* keyboard.c */
void keyboard_show(lv_obj_t *ta);
void keyboard_hide(void);
bool keyboard_visible(void);
bool keyboard_owns(lv_obj_t *obj);
/* gesture.c */
/* Every pointer device (touch, USB mouse) gets the gestures; again is a no-op. */
void gesture_attach_all(void);
/* shell.c: the home hooks (registered by shell_sys_*.c) */
lv_obj_t *shell_home_screen(void);
void shell_prepare(lv_obj_t *scr, const char *id);
void shell_changed(const char *id);
/* recents menu, shared by the launcher and the status bar */
void shell_show_recents(lv_obj_t *anchor);
void shell_app_menu(lv_obj_t *anchor, const char *id, const char *name);
