/* SPDX-License-Identifier: Apache-2.0
 * shell_sys.h in the simulator: the mocked services (sim_services.c) and a
 * fixed app list, so the real launcher, status bar and quick settings run
 * on the desktop. Launching keeps the runtime's order (t5_app/apps.h,
 * t5_apps_home_t) with a stand-in page for the app.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "md/md.h"
#include "md/md_icons.h"
#include "shell_priv.h"
#include "t5_storage/paths.h"
#include "sim.h"

/* The device's built-in apps (sysapps) that the launcher shows, and two
 * installed ones, one of them blocked after crashes. */
static const shell_app_t s_apps[] = {
    { T5_APP_ID_SETTINGS, "Settings", MD_ICON_SETTINGS, true, false, false },
    { T5_APP_ID_FILES, "Files", MD_ICON_FOLDER, true, false, false },
    { T5_APP_ID_MANAGER, "App Manager", MD_ICON_APPS, true, false, false },
    { T5_APP_ID_POWER, "Power", MD_ICON_BATTERY_CHARGING_FULL, true, false, false },
    { T5_APP_ID_PORTS, "Port Inspector", MD_ICON_CABLE, true, false, false },
    { T5_APP_ID_CAMERA, "Camera", MD_ICON_CAMERA_ALT, true, false, false },
    { "com.example.airlog", "Air Log", "", false, false, false },
    { "com.example.rs485", "RS-485 Console", "", false, true, false },
};
#define NAPPS ((int)(sizeof s_apps / sizeof s_apps[0]))
static int s_brightness = 80, s_volume = 60;
static bool s_muted, s_low, s_locked, s_ble;
static lv_obj_t *s_app_scr;
static char s_cur[T5_APP_ID_LEN];
static shell_chrome_t s_chrome;     /* the next launch's [shell] (sim_show_app) */

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
    o->mic_active = g_sim.mic;
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
    int n = NAPPS < max ? NAPPS : max;
    memcpy(out, s_apps, (size_t)n * sizeof *out);
    return n;
}

/* A page titled with the id, holding every widget (sa_sim_fill). */
static void open_stand_in(const char *id)
{
    snprintf(s_cur, sizeof s_cur, "%s", id);
    if (s_app_scr) lv_obj_delete(s_app_scr);
    s_app_scr = lv_obj_create(NULL);
    shell_prepare(s_app_scr, id);
    shell_changed(id);
    lv_obj_t *page = md_page_create(s_app_scr, id, NULL, NULL, NULL);
    sa_sim_fill(md_page_body(page));
    lv_screen_load_anim(s_app_scr, LV_SCREEN_LOAD_ANIM_FADE_IN, MD_DUR_DIALOG_IN_MS, 0, false);
}

void shell_sys_launch(const char *id, const char *arg)
{
    (void)arg;
    for (int i = 0; i < NAPPS; i++) {
        if (!strcmp(s_apps[i].id, id)) {
            open_stand_in(id);
            return;
        }
    }
}

void sim_show_widgets(void)
{
    open_stand_in(SIM_WIDGETS_PAGE);
}

/* An app as the SDK's ui_gallery would look: the shell's app bar (or full
 * screen) over plain content, no page of its own. */
void sim_show_app(bool app_bar, bool fullscreen)
{
    memset(&s_chrome, 0, sizeof s_chrome);
    s_chrome.app_bar = app_bar;
    s_chrome.fullscreen = fullscreen;
    snprintf(s_chrome.title, sizeof s_chrome.title, "Air Log");
    snprintf(s_cur, sizeof s_cur, "com.example.airlog");
    if (s_app_scr) lv_obj_delete(s_app_scr);
    s_app_scr = lv_obj_create(NULL);
    shell_prepare(s_app_scr, s_cur);
    shell_changed(s_cur);
    lv_obj_set_flex_flow(s_app_scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(s_app_scr, T5_DP(16), 0);
    lv_obj_set_style_pad_row(s_app_scr, T5_DP(8), 0);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *card = md_card_create(s_app_scr);
        lv_obj_set_size(card, LV_PCT(100), LV_PCT(45));
        md_label_create(card, i ? "Humidity 41%" : "CO2 612 ppm", MD_TYPE_HEADLINE5);
    }
    if (app_bar) md_appbar_add_action(t5_shell_app_bar(), MD_ICON_REFRESH, NULL, NULL);
    lv_screen_load(s_app_scr);
    memset(&s_chrome, 0, sizeof s_chrome);
}

/* Between screenshots: the display, and every object on it, is gone. */
void sim_shell_sys_reset(void)
{
    s_app_scr = NULL;
    s_cur[0] = 0;
}

void shell_sys_chrome(shell_chrome_t *out)
{
    *out = s_chrome;
    if (!out->title[0]) {
        for (int i = 0; i < NAPPS; i++) {
            if (!strcmp(s_apps[i].id, s_cur)) snprintf(out->title, sizeof out->title, "%s", s_apps[i].name);
        }
    }
}

void shell_sys_insets_changed(void)
{
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

void shell_sys_data_dir(const char *id, char *out, size_t n)
{
    snprintf(out, n, T5_APPDATA_DIR "/%s", id);
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
