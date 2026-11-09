/* SPDX-License-Identifier: Apache-2.0
 * The shell (plan 6.2–6.4): launcher, status bar, quick settings, the
 * on-screen keyboard and system gestures. Apps see the parts in t5/shell.h.
 *
 * Gestures: swipe down from the top edge for quick settings, up from the
 * bottom edge to go home, and up anywhere on home for the app drawer. The status bar's home button does the same, and a
 * long press on it lists recent apps. Input devices that join later (a USB
 * mouse) get the gestures too; while a USB keyboard is attached the
 * on-screen keyboard stays hidden.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the shell as the app runtime's home screen. Call before
 * t5_apps_init(); the chrome is built the first time the home screen is
 * shown. */
t5_err_t t5_shell_register(void);

/* ---- App API: the foreground app's chrome ------------------------------------
 *
 *   status bar   24 dp at the top, always (except in full screen)
 *   app bar      48 dp under it with the app's title: off unless the
 *                manifest says [shell] app_bar = true or the app turns it on
 *   gesture bar  16 dp at the bottom with a handle: swipe up from here to go
 *                home
 *
 * The app's screen (t5_app_screen) is padded clear of them, so content
 * built under it never sits beneath a bar. t5_shell_get_insets() says how
 * much they take (dp), and T5_EV_UI_INSETS is posted when that changes.
 *
 * Full screen hides all three and the padding goes to 0. The swipes still
 * work: touching the bottom edge shows the handle for a moment, and so does
 * entering full screen. Everything resets when the app leaves the
 * foreground. Any task, unless it says UI task. */
t5_err_t t5_shell_set_fullscreen(bool on);
bool t5_shell_fullscreen(void);
t5_err_t t5_shell_set_app_bar(bool shown);
/* The app bar's text. NULL goes back to the manifest's title (or name). */
t5_err_t t5_shell_set_title(const char *title);
/* The app bar, to add actions to (md_appbar_add_action), while it is shown.
 * NULL otherwise, or off the UI task. It is deleted with the app's screen. */
struct _lv_obj_t;
struct _lv_obj_t *t5_shell_app_bar(void);
typedef struct {
    int32_t top, bottom, left, right;   /* dp */
} t5_shell_insets_t;
/* T5_ERR_STATE (and zeros) on the home screen. */
t5_err_t t5_shell_get_insets(t5_shell_insets_t *out);
/* A snackbar from any task (copied). */
t5_err_t t5_shell_toast(const char *text);
/* Open or close quick settings, go home. */
t5_err_t t5_shell_quick_settings(bool open);
t5_err_t t5_shell_home(void);
/* Show the on-screen keyboard for a text area (UI task). The shell does
 * this by itself when a text area is tapped. */
void t5_shell_keyboard_show(void *textarea);
void t5_shell_keyboard_hide(void);

#ifdef __cplusplus
}
#endif
