/* SPDX-License-Identifier: Apache-2.0
 * shell_sys.h in the simulator: the mocked services (sim_services.c) and a
 * fixed app list, so the real launcher, status bar and quick settings run
 * on the desktop.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "md/md.h"
#include "md/md_icons.h"
#include "shell_sys.h"
#include "sim.h"

static const shell_app_t s_apps[] = {
    { "com.t5.settings", "Settings", MD_ICON_SETTINGS, true, false, false },
    { "com.t5.files", "Files", MD_ICON_FOLDER, true, false, false },
    { "com.t5.appmanager", "App Manager", MD_ICON_APPS, true, false, false },
    { "com.t5.power", "Power", MD_ICON_BATTERY_CHARGING_FULL, true, false, false },
    { "com.t5.ports", "Port Inspector", MD_ICON_CABLE, true, false, false },
    { "com.t5.camera", "Camera", MD_ICON_CAMERA_ALT, true, false, false },
    { "com.example.airlog", "Air Log", "", false, false, false },
    { "com.example.rs485", "RS-485 Console", "", false, true, false },
};
static int s_brightness = 80, s_volume = 60;
static bool s_muted, s_low, s_locked, s_ble;
static lv_obj_t *s_app_scr;
static char s_cur[64];

void shell_sys_status(shell_status_t *o)
{
    memset(o, 0, sizeof *o);
    o->battery_pct = g_sim.battery;
    o->charging = g_sim.charging;
    o->usb_power = g_sim.charging;
    o->minutes_left = g_sim.charging ? -1 : g_sim.battery * 4;
    o->low_power = s_low;
    o->wifi_on = true;
    o->wifi_connected = g_sim.wifi;
    o->wifi_rssi = g_sim.rssi;
    snprintf(o->ssid, sizeof o->ssid, "Workshop");
    o->ble_on = s_ble;
    o->mic_active = g_sim.grove_claimed;
    o->rotation_locked = s_locked;
    o->brightness = s_brightness;
    o->volume = s_volume;
    o->muted = s_muted;
}

bool sim_fixed_time;

bool shell_sys_time(struct tm *out)
{
    time_t t = sim_fixed_time ? (time_t)1793889000 : time(NULL);    /* 2026-11-05 14:30 UTC for screenshots */
    gmtime_r(&t, out);
    return true;
}

int shell_sys_apps(shell_app_t *out, int max)
{
    int n = (int)(sizeof s_apps / sizeof s_apps[0]);
    if (n > max) n = max;
    memcpy(out, s_apps, (size_t)n * sizeof *out);
    return n;
}

void shell_sys_launch(const char *id, const char *arg)
{
    (void)arg;
    /* A stand-in app screen: a page with the app's name. */
    snprintf(s_cur, sizeof s_cur, "%s", id);
    if (s_app_scr) lv_obj_delete(s_app_scr);
    s_app_scr = lv_obj_create(NULL);
    shell_prepare(s_app_scr, id);
    lv_obj_t *page = md_page_create(s_app_scr, id, NULL, NULL, NULL);
    sa_sim_fill(md_page_body(page));
    lv_screen_load_anim(s_app_scr, LV_SCREEN_LOAD_ANIM_FADE_IN, MD_DUR_DIALOG_IN_MS, 0, false);
    shell_changed(id);
}

bool shell_sys_dismiss_modal(void)
{
    return false;
}

void shell_sys_home(void)
{
    if (!s_cur[0]) return;
    s_cur[0] = 0;
    lv_screen_load(shell_home_screen());
    shell_changed("");
    lv_obj_delete(s_app_scr);
    s_app_scr = NULL;
}

void shell_sys_current(char *id, size_t n)
{
    snprintf(id, n, "%s", s_cur);
}

bool shell_sys_can_remove(const char *id)
{
    return strncmp(id, "com.t5.", 7) != 0;
}

void shell_sys_remove(const char *id) { (void)id; }
void shell_sys_set_wifi(bool on) { g_sim.wifi = on; }
void shell_sys_set_ble(bool on) { s_ble = on; }
void shell_sys_set_rotation_lock(bool on) { s_locked = on; }
void shell_sys_set_low_power(bool on) { s_low = on; }
void shell_sys_set_brightness(int pct) { s_brightness = pct; }
void shell_sys_set_volume(int pct) { s_volume = pct; }
void shell_sys_set_muted(bool on) { s_muted = on; }
void shell_sys_click(void) {}
void shell_sys_apply_prefs(void) {}
void *shell_sys_enter(void) { return NULL; }
void shell_sys_leave(void *saved) { (void)saved; }

bool shell_sys_safe_mode(char *why, size_t n)
{
    (void)why;
    (void)n;
    return false;
}

void shell_sys_restart(void)
{
}
