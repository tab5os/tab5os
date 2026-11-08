/* SPDX-License-Identifier: Apache-2.0
 * The shell (plan 6.2–6.4): launcher, status bar, quick settings, the
 * on-screen keyboard and system gestures. Apps see the parts in t5/shell.h.
 *
 * Gestures: swipe down from the top edge for quick settings, up from the
 * bottom edge to go home, and up anywhere on home for the app drawer. The status bar's home button does the same, and a
 * long press on it lists recent apps.
 */
#pragma once
#include <stdbool.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the shell as the app runtime's home screen. Call before
 * t5_apps_init(); the chrome is built the first time the home screen is
 * shown. */
t5_err_t t5_shell_register(void);
bool t5_shell_ready(void);

/* App API (any task). Hide the status bar for the foreground app (camera,
 * games). Reset when the app leaves the foreground. */
t5_err_t t5_shell_set_fullscreen(bool on);
/* A snackbar from any task (copied). action may be NULL. */
t5_err_t t5_shell_toast(const char *text);
/* Open or close quick settings, go home. */
t5_err_t t5_shell_quick_settings(bool open);
t5_err_t t5_shell_home(void);
/* A hardware keyboard came or went (USB HID): the on-screen keyboard stays
 * hidden while one is attached. */
void t5_shell_set_hw_keyboard(bool present);
/* Show the on-screen keyboard for a text area (UI task). The shell does
 * this by itself when a text area is tapped. */
void t5_shell_keyboard_show(void *textarea);
void t5_shell_keyboard_hide(void);

#ifdef __cplusplus
}
#endif
