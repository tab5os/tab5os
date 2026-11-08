/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "lvgl.h"
#include "md/md.h"
#include "md/md_icons.h"
#include "shell_sys.h"
#include "t5_shell/shell.h"

#define SHELL_STATUS_H_DP 24
#define SHELL_RECENTS 6

/* shell.c */
void shell_chrome_init(void);          /* status bar, gestures, keyboard; once, UI task */
bool shell_in_app(void);
const char *shell_current_id(void);
void shell_note_launch(const char *id);
int shell_recents(char ids[][SHELL_ID_LEN], int max);
/* Forget every object and timer (the simulator deletes its display between
 * screenshots). */
void shell_reset(void);
void launcher_reset(void);
void statusbar_reset(void);
void qs_reset(void);
void keyboard_reset(void);
void gesture_reset(void);
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
void keyboard_init(void);
void keyboard_show(lv_obj_t *ta);
void keyboard_hide(void);
bool keyboard_visible(void);
bool keyboard_owns(lv_obj_t *obj);
lv_obj_t *keyboard_textarea(void);
bool keyboard_hw_present(void);
/* gesture.c */
void gesture_init(void);
void shell_attach_indev(lv_indev_t *indev);   /* a pointer device (touch, USB mouse) */
/* shell.c: the home hooks (registered by shell_sys_*.c) */
lv_obj_t *shell_home_screen(void);
void shell_prepare(lv_obj_t *scr, const char *id);
void shell_changed(const char *id);
/* recents menu, shared by the launcher and the status bar */
void shell_show_recents(lv_obj_t *anchor);
void shell_app_menu(lv_obj_t *anchor, const char *id, const char *name);
