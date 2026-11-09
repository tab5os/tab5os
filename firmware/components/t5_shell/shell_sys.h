/* SPDX-License-Identifier: Apache-2.0
 * What the shell needs from the rest of the OS, in one narrow interface:
 * shell_sys_idf.c on the device, sim/sim_shell_sys.c in the simulator, so
 * the launcher, status bar and quick settings are the same code in both.
 * Everything here is called on the UI task.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "t5_app/registry.h"

typedef struct {
    int battery_pct;          /* -1 unknown */
    bool charging, usb_power, low_power;
    int32_t battery_mw;       /* + discharging */
    int32_t minutes_left;     /* -1 unknown */
    bool wifi_on, wifi_connected;
    int wifi_rssi;
    char ssid[33];
    bool ble_on, ble_connected;
    bool mic_active, cam_active;
    bool usb_devices;         /* something on USB-A */
    bool usb_storage;         /* USB-C mass storage on */
    bool rotation_locked;
    int brightness;           /* 1..100 */
    int volume;               /* 0..100 */
    bool muted, headphones;
    /* Background work, for the status bar's indicators. */
    bool audio_playing;       /* an app's sound is playing */
    int net_handles;          /* open network handles, every app's */
    bool transfer;            /* an OS download or update in progress */
    bool background;          /* an app other than the one in front is working: it holds a
                               * wake lock, or runs its alarm headless */
    bool alarm;               /* an app has a background wake scheduled */
} shell_status_t;

/* An ongoing activity: one an app posted (t5_shell_activity_*), or one the
 * shell sees for itself (an update downloading, an app in the background). */
#define SHELL_ACTIVITY_TEXT_LEN 48
#define SHELL_ACTIVITY_MAX 12
typedef struct {
    char icon[T5_APP_ICON_LEN];         /* MD_ICON_* */
    char who[T5_APP_NAME_LEN];          /* the app's name, or "" for the system */
    char text[SHELL_ACTIVITY_TEXT_LEN];
    int8_t percent;                     /* 0..100, -1: no progress */
    bool app;                           /* posted by an app */
} shell_activity_t;
int shell_sys_activities(shell_activity_t *out, int max);

/* ---- the standby screen (standby.c) ---- */
/* An app's card (t5_ambient_set), with the app's name. */
typedef struct {
    char icon[T5_APP_ICON_LEN];
    char title[32];
    char value[24];
    char detail[64];
    char who[T5_APP_NAME_LEN];
} shell_card_t;
/* Unexpired cards, the most recently set first. */
int shell_sys_cards(shell_card_t *out, int max);
/* The active media session (t5_media_now). */
typedef struct {
    bool active, playing;
    char title[64], artist[64];
    char who[T5_APP_NAME_LEN];
    uint32_t position_ms, duration_ms;
} shell_media_t;
bool shell_sys_media(shell_media_t *out);
/* A command for the active session. */
typedef enum { SHELL_MEDIA_PREVIOUS, SHELL_MEDIA_TOGGLE, SHELL_MEDIA_NEXT } shell_media_cmd_t;
void shell_sys_media_cmd(shell_media_cmd_t cmd);
/* Standby on: hold the screen awake and cap the backlight at percent; off:
 * release both. */
void shell_sys_standby_hold(bool on, int percent);
bool shell_sys_standby_enabled(void);

/* One entry of the app registry (t5_app_entry_t on the device). */
typedef struct {
    char id[T5_APP_ID_LEN];
    char name[T5_APP_NAME_LEN];
    char icon[T5_APP_ICON_LEN];   /* MD_ICON_* glyph, "" = generic */
    bool builtin, blocked, running;
} shell_app_t;

void shell_sys_status(shell_status_t *out);
/* Local time; false if the clock isn't set. */
bool shell_sys_time(struct tm *out);

/* Up to max (T5_APPS_MAX lists them all). */
int shell_sys_apps(shell_app_t *out, int max);
/* The runtime's side of t5_apps_home_t (t5_app/apps.h), in its order:
 * launching creates the app's screen, calls shell_prepare(screen, id) and
 * shell_changed(id), then loads the screen; going home loads
 * shell_home_screen(), calls shell_changed("") and then deletes the app's
 * screen. An unknown id launches nothing. */
void shell_sys_launch(const char *id, const char *arg);
void shell_sys_home(void);
bool shell_sys_dismiss_modal(void);    /* true if a system modal was open */
/* Safe mode (plan 9.3): true, and a line about the last crash in why. */
bool shell_sys_safe_mode(char *why, size_t n);
/* Restart normally (leaves safe mode). */
void shell_sys_restart(void);
void shell_sys_current(char *id, size_t n);
/* What the foreground app's manifest asks of the shell ([shell]); title is
 * its name when it set none. */
typedef struct {
    bool app_bar, fullscreen;
    char title[T5_APP_NAME_LEN];
} shell_chrome_t;
void shell_sys_chrome(shell_chrome_t *out);
/* The shell's bars changed what they take from the app's screen. */
void shell_sys_insets_changed(void);
void shell_sys_remove(const char *id);
/* Where an app's data lives (kept when it is removed). */
void shell_sys_data_dir(const char *id, char *out, size_t n);

void shell_sys_set_wifi(bool on);
void shell_sys_set_ble(bool on);
void shell_sys_set_rotation_lock(bool on);
void shell_sys_set_low_power(bool on);
void shell_sys_set_brightness(int pct);
void shell_sys_set_volume(int pct);
void shell_sys_set_muted(bool on);
/* A short click from the system sound mixer (keyboard, toggles). */
void shell_sys_click(void);
/* Saved look (accent colour), applied once when the chrome is built. */
void shell_sys_apply_prefs(void);

/* Objects the shell makes for itself while an app is in front belong to
 * the system, not the app (t5_sys_enter/leave on the device). */
void *shell_sys_enter(void);
void shell_sys_leave(void *saved);
