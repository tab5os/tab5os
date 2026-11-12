/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "shell_logic.h"
#include "shell_sys.h"
#include "t5_gui/t5ui.h"
#include "t5_gui/t5ui_icons.h"
#include "t5_gui/t5ui_sys.h"
#include "t5_shell/shell.h"

#define SHELL_STATUS_H_DP 24
#define SHELL_GESTURE_H_DP 16                    /* the gesture bar's strip at the bottom */
#define SHELL_PEEK_MS 2500                       /* the handle's showing in full screen */

/* Material's tokens the shell uses (the Tab5 Material theme's values). */
#define SHELL_ICON_DP 24                         /* an icon's usual size */
#define SHELL_MARGIN_DP 16
#define SHELL_SHAPE_MEDIUM_DP 4                  /* cards, dialogs, menus */
#define SHELL_ELEV_CARD 1
#define SHELL_ELEV_MENU 8
#define SHELL_DUR_DIALOG_IN_MS 150
#define SHELL_DUR_DRAWER_IN_MS 250
#define SHELL_DUR_DRAWER_OUT_MS 200
/* Material's "sharp" curve: leaving for a while. */
#define SHELL_EASE_SHARP T5UI_EASE_SHARP

/* shell.c */
void shell_chrome_init(void);          /* status bar, gestures, keyboard; once, UI task */
bool shell_in_app(void);
void shell_note_launch(const char *id);
int shell_recents(char ids[][T5_APP_ID_LEN], int max);
/* An input device joined: gestures for it, and the on-screen keyboard
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
/* Text (T5UI_P_TEXT) is only set when it changes: every set redraws. */
void shell_set_text(t5ui_obj_t *label, const char *text);
/* The clock and the date (either may be NULL), or the "not set" texts. */
void shell_show_time(t5ui_obj_t *clock, t5ui_obj_t *date);
/* Small builders shared by the shell's files. */
t5ui_obj_t *shell_box(t5ui_obj_t *parent);                                  /* no look, sized to content */
t5ui_obj_t *shell_text(t5ui_obj_t *parent, const char *text, t5ui_text_t style);   /* on-surface, high emphasis */
t5ui_obj_t *shell_icon(t5ui_obj_t *parent, const char *glyph, int32_t size_dp);
void shell_flex(t5ui_obj_t *o, t5ui_flow_t flow, t5ui_place_t main, t5ui_place_t cross, t5ui_place_t track);
void shell_pad_hor(t5ui_obj_t *o, int32_t px);
void shell_pad_ver(t5ui_obj_t *o, int32_t px);
void shell_size(t5ui_obj_t *o, int32_t w, int32_t h);
/* Material elevation dp, with no shadow in the dark theme: the surface
 * tint shows the height there. */
void shell_elevate(t5ui_obj_t *o, int32_t dp);
/* statusbar.c */
t5ui_obj_t *statusbar_create(void);
void statusbar_update(void);
void statusbar_set_hidden(bool hidden);
void statusbar_set_in_app(bool in_app);
/* bars.c: the gesture bar */
void bars_handle_show(bool shown);
void bars_handle_peek(void);
void bars_reset(void);
/* shell.c: in an app, in full screen (the gesture bar peeks on an edge touch) */
bool shell_fullscreen(void);
/* activity.c (device): the activities apps posted; app_name fills who. */
int shell_activity_list(shell_activity_t *out, int max, const char *(*app_name)(void *app, char *buf, size_t n));
/* standby.c */
void standby_show(void);
void standby_hide(void);
bool standby_shown(void);
void standby_tick(void);              /* once a second from the shell's timer */
void standby_reset(void);
/* ambient.c (device): app cards, newest first; name fills who from an app id. */
/* Cards newest first; name() fills in the app's name and says whether it
 * is still installed (a removed app's card is left out). */
int ambient_list(shell_card_t *out, int max, bool (*name)(const char *id, char *buf, size_t n));
/* When the app last set its card this boot (t5_time_us), or 0. */
int64_t ambient_set_at(const char *id);
/* standby_refresh.c (device): standby is showing (called each second) or gone. */
void standby_refresh_set(bool on);
/* qs.c */
void qs_open(void);
void qs_close(void);
bool qs_is_open(void);
/* launcher.c */
t5ui_obj_t *launcher_get(void);
void launcher_reload(void);
void launcher_restyle(void);   /* after an accent change */
void launcher_tick(void);
/* The app drawer over the home screen. */
void launcher_drawer_open(void);
void launcher_drawer_close(bool animate);
bool launcher_drawer_is_open(void);
/* keyboard.c */
void keyboard_show(t5ui_obj_t *ta);
void keyboard_hide(void);
bool keyboard_visible(void);
bool keyboard_owns(t5ui_obj_t *obj);
/* gesture.c */
/* Every pointer device (touch, USB mouse) gets the gestures; again is a no-op. */
void gesture_attach_all(void);
/* shell.c: the home hooks (registered by shell_sys_*.c, which converts the
 * runtime's screen objects at that boundary) */
t5ui_obj_t *shell_home_screen(void);
void shell_prepare(t5ui_obj_t *scr, const char *id);
void shell_changed(const char *id);
/* recents menu, shared by the launcher and the status bar */
void shell_show_recents(t5ui_obj_t *anchor);
void shell_app_menu(t5ui_obj_t *anchor, const char *id, const char *name);
