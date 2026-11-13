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
 *   gesture bar  16 dp at the bottom with a handle: swipe up from here to go
 *                home
 *
 * The app's screen (t5_app_screen) is padded clear of them, so content
 * built under it never sits beneath a bar. t5_shell_get_insets() says how
 * much they take (dp), and T5_EV_UI_INSETS is posted when that changes.
 *
 * There is no app bar or title bar: an app that wants one draws its own
 * (a T5UI_MD_APPBAR, or a T5UI_MD_PAGE for a page with a back arrow).
 *
 * Full screen hides both and the padding goes to 0. The swipes still
 * work: touching the bottom edge shows the handle for a moment, and so does
 * entering full screen. Everything resets when the app leaves the
 * foreground. Any task, unless it says UI task. Off the UI task the setters
 * are queued, in order, and take effect on the UI task a moment later:
 * t5_shell_fullscreen() and t5_shell_get_insets() show the change then. */
t5_err_t t5_shell_set_fullscreen(bool on);
bool t5_shell_fullscreen(void);
typedef struct {
    int32_t top, bottom, left, right;   /* dp */
} t5_shell_insets_t;
/* T5_ERR_STATE (and zeros) on the home screen. */
t5_err_t t5_shell_get_insets(t5_shell_insets_t *out);
/* ---- App API: ongoing activities ---------------------------------------------
 *
 * Something the app is doing that people should be able to see while it
 * lasts (a sync, a recording, a long download): an icon in the status bar
 * (up to three apps' icons there, the rest as "+N"), and a line in quick
 * settings with the app's name, the text and, if given, a progress bar.
 * Ended by t5_shell_activity_end() or when the app exits. At most
 * T5_SHELL_ACTIVITY_MAX per app. Any task.
 *
 *   t5_shell_activity_t *a;
 *   t5_shell_activity_start(T5UI_ICON_DOWNLOAD, "Syncing photos", &a);
 *   t5_shell_activity_update(a, "Syncing photos: 12 of 40", 30);
 *   t5_shell_activity_end(a);
 */
#define T5_SHELL_ACTIVITY_MAX 4
typedef struct t5_shell_activity t5_shell_activity_t;
/* icon: one T5UI_ICON_* glyph (NULL: a clock). text is copied (47 bytes kept). */
t5_err_t t5_shell_activity_start(const char *icon, const char *text, t5_shell_activity_t **out);
/* text NULL keeps it; percent 0..100, or -1 for no progress bar. */
t5_err_t t5_shell_activity_update(t5_shell_activity_t *a, const char *text, int percent);
t5_err_t t5_shell_activity_end(t5_shell_activity_t *a);

/* ---- App API: a card on the standby screen (needs ambient) -------------------
 *
 * While the Tab5 charges and sits idle, the shell shows the standby screen:
 * a large clock and up to two cards. One is the music card, drawn by the
 * OS from the active media session (t5_audio/media.h). The other is the
 * most recently updated app card, set here: weather, the next alarm, a
 * calendar entry, a sensor. Each app has one card; setting it again
 * replaces it. It stays (across the app's exit and a restart) until the app
 * clears it or it expires, so an app that updates it from its background
 * alarm keeps it current without running. Any task. */
typedef struct {
    char icon[8];             /* one T5UI_ICON_* glyph, "" for the app's */
    char title[32];           /* "Weather", "Next alarm" */
    char value[24];           /* the big line: "21°", "07:30" */
    char detail[64];          /* "Cloudy, high 24°" */
    uint32_t expires_s;       /* seconds from now it goes; 0 = until cleared */
} t5_ambient_card_t;
t5_err_t t5_ambient_set(const t5_ambient_card_t *card);
t5_err_t t5_ambient_clear(void);

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
