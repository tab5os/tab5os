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

#define SHELL_ID_LEN 64
#define SHELL_NAME_LEN 48

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
    bool hw_keyboard;         /* a USB keyboard is attached */
} shell_status_t;

typedef struct {
    char id[SHELL_ID_LEN];
    char name[SHELL_NAME_LEN];
    char icon[8];             /* MD_ICON_* glyph, "" = generic */
    bool builtin, blocked, running;
} shell_app_t;

void shell_sys_status(shell_status_t *out);
/* Local time; false if the clock isn't set. */
bool shell_sys_time(struct tm *out);

int shell_sys_apps(shell_app_t *out, int max);
void shell_sys_launch(const char *id, const char *arg);
void shell_sys_home(void);
bool shell_sys_dismiss_modal(void);    /* true if a system modal was open */
/* Safe mode (plan 9.3): true, and a line about the last crash in why. */
bool shell_sys_safe_mode(char *why, size_t n);
/* Restart normally (leaves safe mode). */
void shell_sys_restart(void);
void shell_sys_current(char *id, size_t n);
bool shell_sys_can_remove(const char *id);
void shell_sys_remove(const char *id);

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

/* System app ids the shell links to. */
#define SHELL_APP_SETTINGS "com.t5.settings"
#define SHELL_APP_MANAGER "com.t5.appmanager"
#define SHELL_APP_FILES "com.t5.files"
#define SHELL_APP_POWER "com.t5.power"
