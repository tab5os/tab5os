/* SPDX-License-Identifier: Apache-2.0
 * shell_sys.h on the device: the shell's view of the OS services, and the
 * home hooks that make the shell the app runtime's home screen.
 */
#include "esp_system.h"
#include "t5_power/autosave.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "t5_app/apps.h"
#include "t5_core/event.h"
#include "t5_audio/audio.h"
#include "t5_battery/battery.h"
#include "t5_camera/camera.h"
#include "t5_display/t5_display.h"
#include "t5_net/ble.h"
#include "t5_net/net.h"
#include "t5_power/power.h"
#include "t5_time/t5_time.h"
#include "t5_ui/t5_ui.h"
#include "t5_usb/usb.h"
#include "shell_priv.h"
#include "t5_core/log.h"

static const char *TAG = "t5.shell";

void shell_sys_status(shell_status_t *o)
{
    memset(o, 0, sizeof *o);
    t5_battery_status_t b;
    t5_battery_status(&b);
    o->battery_pct = b.ready ? b.percent : -1;
    o->charging = b.charge == T5_CHG_CHARGING;
    o->usb_power = b.usb;
    o->low_power = t5_power_low_power();
    o->battery_mw = b.mw;
    o->minutes_left = b.minutes_left;
    t5_net_status_t n;
    t5_net_status(&n);
    o->wifi_on = n.enabled;
    o->wifi_connected = n.state == T5_NET_CONNECTED;
    o->wifi_rssi = n.rssi;
    snprintf(o->ssid, sizeof o->ssid, "%s", n.ssid);
    o->ble_on = t5_ble_ready();
    o->ble_connected = t5_ble_connected();
    /* What the user sees: the screen follows the board only with auto-rotate
     * on and no lock (auto-rotate alone is otherwise console-only). */
    o->rotation_locked = t5_ui_rotation_locked() || !t5_ui_auto_rotate();
    o->brightness = t5_display_brightness();
    o->volume = t5_audio_volume();
    o->muted = t5_audio_muted();
    o->headphones = t5_audio_headphones();
    o->mic_active = t5_audio_in_active();
    o->cam_active = t5_camera_active();
    o->usb_devices = t5_usb_device_count() > 0;
    o->usb_storage = t5_usbc_mode() == T5_USBC_STORAGE;
    o->hw_keyboard = t5_usb_keyboard_present();
}

bool shell_sys_time(struct tm *out)
{
    return t5_time_local(out);
}

int shell_sys_apps(shell_app_t *out, int max)
{
    t5_app_entry_t *e = calloc((size_t)max, sizeof *e);
    if (!e) return 0;
    int n = t5_apps_list(e, max);
    for (int i = 0; i < n; i++) {
        shell_app_t *a = &out[i];
        memset(a, 0, sizeof *a);
        snprintf(a->id, sizeof a->id, "%s", e[i].id);
        snprintf(a->name, sizeof a->name, "%s", e[i].name);
        snprintf(a->icon, sizeof a->icon, "%s", e[i].icon);
        a->builtin = e[i].builtin;
        a->blocked = e[i].autolaunch_blocked;
        a->running = e[i].running;
    }
    free(e);
    return n;
}

void shell_sys_launch(const char *id, const char *arg)
{
    T5_LOG_ON_ERR(TAG, t5_apps_launch_arg(id, arg));
}

void shell_sys_home(void)
{
    T5_LOG_ON_ERR(TAG, t5_apps_exit_current());
}

bool shell_sys_dismiss_modal(void)
{
    return t5_apps_modal_dismiss();
}

void shell_sys_current(char *id, size_t n)
{
    t5_apps_current(id, n);
}

bool shell_sys_can_remove(const char *id)
{
    shell_app_t *a = calloc(48, sizeof *a);
    if (!a) return false;
    int n = shell_sys_apps(a, 48);
    bool ok = false;
    for (int i = 0; i < n; i++) {
        if (!strcmp(a[i].id, id)) ok = !a[i].builtin && !a[i].running;
    }
    free(a);
    return ok;
}

void shell_sys_remove(const char *id)
{
    T5_LOG_ON_ERR(TAG, t5_apps_remove(id, true));
}

void shell_sys_set_wifi(bool on)
{
    t5_net_enable(on);
}

void shell_sys_set_ble(bool on)
{
    t5_ble_enable(on);
}

void shell_sys_set_rotation_lock(bool on)
{
    if (!on) t5_ui_set_auto_rotate(true);
    t5_ui_set_rotation_lock(on);   /* saves both */
}

void shell_sys_set_low_power(bool on)
{
    t5_power_set_low_power(on);
}

void shell_sys_set_brightness(int pct)
{
    t5_display_set_brightness(pct < 1 ? 1 : pct);
}

void shell_sys_set_volume(int pct)
{
    t5_audio_set_volume(pct);
}

void shell_sys_set_muted(bool on)
{
    t5_audio_set_muted(on);
}

void shell_sys_click(void)
{
    t5_audio_play_sound(T5_SOUND_CLICK);
}

void shell_sys_apply_prefs(void)
{
    FILE *f = fopen("/data/sys/accent", "rb");
    if (!f) return;
    int v = fgetc(f);
    fclose(f);
    if (v >= 0 && v < MD_ACCENT_COUNT) md_set_accent((md_accent_t)v, MD_ACCENT_TEAL);
}

void *shell_sys_enter(void)
{
    return t5_sys_enter();
}

void shell_sys_leave(void *saved)
{
    t5_sys_leave(saved);
}

/* ---- registration ------------------------------------------------------------------ */
static const t5_apps_home_t s_home = {
    .screen = shell_home_screen,
    .refresh = launcher_reload,
    .prepare = shell_prepare,
    .changed = shell_changed,
};

static void usbc_event(t5_event_id_t id, const void *data, size_t len, void *arg)
{
    (void)id; (void)len; (void)arg;
    const t5_ev_usb_t *e = data;
    if (e->mounted) t5_shell_toast("The SD card is with the computer. Eject it there to get it back.");
    else t5_shell_toast(t5_usbc_ejected() ? "The computer ejected the SD card. It's back."
                                          : "The SD card is back.");
}

t5_err_t t5_shell_register(void)
{
    t5_apps_set_home(&s_home);
    t5_event_subscribe(T5_EV_USBC_MODE, usbc_event, NULL);
    return T5_OK;
}

bool shell_sys_safe_mode(char *why, size_t n)
{
    if (!t5_apps_safe_mode()) return false;
    t5_crash_rec_t rec[8];
    int k = t5_apps_crashes(rec, 8);
    int last = -1;
    for (int i = 0; i < k; i++) {
        if (rec[i].total && (last < 0 || rec[i].last_time > rec[last].last_time)) last = i;
    }
    if (last >= 0) {
        snprintf(why, n, "Last crash: %s (%s).", rec[last].id, t5_crash_kind_name((t5_crash_kind_t)rec[last].last_kind));
    } else {
        snprintf(why, n, "The last boots didn't finish.");
    }
    return true;
}

void shell_sys_restart(void)
{
    T5_LOG_ON_ERR(TAG, t5_autosave_now());
    esp_restart();
}
