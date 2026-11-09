/* SPDX-License-Identifier: Apache-2.0
 * shell_sys.h on the device: the shell's view of the OS services, and the
 * home hooks that make the shell the app runtime's home screen.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "t5_app/apps.h"
#include "t5_core/event.h"
#include "t5_audio/audio.h"
#include "t5_audio/media_sys.h"
#include "t5_battery/battery.h"
#include "t5_camera/camera.h"
#include "t5_display/t5_display.h"
#include "t5_net/ble_sys.h"
#include "t5_net/download.h"
#include "t5_net/net.h"
#include "t5_net/sock.h"
#include "t5_ota/ota.h"
#include "t5_pkg/pkg.h"
#include "t5_power/power.h"
#include "t5_time/t5_time.h"
#include "t5_ui/t5_ui.h"
#include "t5_usb/usb.h"
#include "t5_shell/accent.h"
#include "t5_shell/standby.h"
#include "shell_priv.h"
#include "t5_core/log.h"

static const char *TAG = "t5.shell";

/* An app's name from its id (the id itself if it isn't installed, and
 * then false). */
static bool app_name_of(const char *id, char *out, size_t n)
{
    snprintf(out, n, "%s", id);
    bool found = false;
    t5_app_entry_t *e = calloc(T5_APPS_MAX, sizeof *e);
    int k = e ? t5_apps_list(e, T5_APPS_MAX) : 0;
    for (int i = 0; i < k; i++) {
        if (strcmp(e[i].id, id)) continue;
        found = true;
        if (e[i].name[0]) snprintf(out, n, "%s", e[i].name);
    }
    free(e);
    return found;
}

static const char *ctx_name(void *app, char *buf, size_t n)
{
    const t5_app_ctx_hdr_t *h = app;
    if (!h || h->magic != T5_APP_CTX_MAGIC) snprintf(buf, n, "%s", "");
    else app_name_of(h->id, buf, n);
    return buf;
}

/* Wake locks held by apps other than the one in front: how many, and the
 * index-th one's app context and type. */
static int bg_lock_holder(void **app, t5_lock_type_t *type, int index)
{
    t5_wakelock_info_t *l = calloc(T5_WAKELOCK_MAX, sizeof *l);
    if (!l) return 0;
    int k = t5_wakelock_list(l, T5_WAKELOCK_MAX), n = 0;
    char fg[T5_APP_ID_LEN];
    t5_apps_current(fg, sizeof fg);
    for (int i = 0; i < k; i++) {
        const t5_app_ctx_hdr_t *h = l[i].app;
        if (!h || h->magic != T5_APP_CTX_MAGIC || !strcmp(h->id, fg)) continue;
        if (n == index && app) {
            *app = l[i].app;
            *type = l[i].type;
        }
        n++;
    }
    free(l);
    return n;
}

static void act(shell_activity_t *a, const char *icon, const char *who, const char *text, int pct)
{
    memset(a, 0, sizeof *a);
    snprintf(a->icon, sizeof a->icon, "%s", icon);
    snprintf(a->who, sizeof a->who, "%s", who);
    snprintf(a->text, sizeof a->text, "%s", text);
    a->percent = (int8_t)pct;
}

int shell_sys_activities(shell_activity_t *out, int max)
{
    int n = 0;
    t5_ota_status_t ota;
    t5_ota_status(&ota);
    if (n < max && (ota.state == T5_OTA_RECEIVING || ota.state == T5_OTA_VERIFYING)) {
        int pct = ota.total ? (int)((uint64_t)ota.done * 100 / ota.total) : -1;
        act(&out[n++], MD_ICON_SYSTEM_UPDATE, "", ota.state == T5_OTA_VERIFYING ? "Checking the update" : "Installing an update", pct);
    }
    if (n < max && t5_download_active() > 0) act(&out[n++], MD_ICON_DOWNLOAD, "", "Downloading", -1);
    char id[T5_APP_ID_LEN], name[T5_APP_NAME_LEN];
    if (n < max && t5_apps_headless(id, sizeof id)) {
        app_name_of(id, name, sizeof name);
        act(&out[n++], MD_ICON_ALARM, name, "Running in the background", -1);
    }
    int locks = bg_lock_holder(NULL, NULL, 0);
    for (int i = 0; i < locks && n < max; i++) {
        void *app = NULL;
        t5_lock_type_t type = T5_LOCK_CPU;
        bg_lock_holder(&app, &type, i);
        ctx_name(app, name, sizeof name);
        bool dup = false;
        for (int j = 0; j < n; j++) dup |= !strcmp(out[j].who, name) && !strcmp(out[j].icon, MD_ICON_BOLT);
        if (dup) continue;
        act(&out[n++], MD_ICON_BOLT, name, type == T5_LOCK_NET ? "Using the network in the background"
                                           : type == T5_LOCK_AUDIO ? "Playing in the background"
                                                                   : "Working in the background", -1);
    }
    time_t when;
    if (n < max && t5_apps_alarm_pending(id, sizeof id, &when)) {
        struct tm tm;
        char text[SHELL_ACTIVITY_TEXT_LEN];
        if (localtime_r(&when, &tm)) snprintf(text, sizeof text, "Wakes the device at %02d:%02d", tm.tm_hour, tm.tm_min);
        else snprintf(text, sizeof text, "Wakes the device later");
        app_name_of(id, name, sizeof name);
        act(&out[n++], MD_ICON_ALARM, name, text, -1);
    }
    return n + shell_activity_list(out + n, max - n, ctx_name);
}

/* ---- the standby screen ---- */
static bool name_fill(const char *id, char *out, size_t n)
{
    return app_name_of(id, out, n);
}

int shell_sys_cards(shell_card_t *out, int max)
{
    return ambient_list(out, max, name_fill);
}

bool shell_sys_media(shell_media_t *out)
{
    memset(out, 0, sizeof *out);
    t5_media_now_t m;
    if (!t5_media_now(&m)) return false;
    out->active = true;
    out->playing = m.state == T5_MEDIA_PLAYING;
    snprintf(out->title, sizeof out->title, "%s", m.info.title);
    snprintf(out->artist, sizeof out->artist, "%s", m.info.artist);
    app_name_of(m.app_id, out->who, sizeof out->who);
    out->position_ms = m.position_ms;
    out->duration_ms = m.info.duration_ms;
    return true;
}

void shell_sys_media_cmd(shell_media_cmd_t cmd)
{
    static const t5_media_cmd_type_t map[] = {
        [SHELL_MEDIA_PREVIOUS] = T5_MEDIA_CMD_PREVIOUS,
        [SHELL_MEDIA_TOGGLE] = T5_MEDIA_CMD_TOGGLE,
        [SHELL_MEDIA_NEXT] = T5_MEDIA_CMD_NEXT,
    };
    if ((unsigned)cmd >= sizeof map / sizeof map[0]) return;
    t5_media_cmd_t c = { .type = map[cmd] };
    T5_LOG_ON_ERR(TAG, t5_media_command(&c));
}

static t5_wakelock_t *s_standby_lock;

void shell_sys_standby_hold(bool on, int percent)
{
    if (on && !s_standby_lock) s_standby_lock = t5_wakelock_acquire_system(T5_LOCK_SCREEN, "standby");
    if (!on && s_standby_lock) {
        t5_wakelock_release(s_standby_lock);
        s_standby_lock = NULL;
    }
    T5_LOG_ON_ERR(TAG, t5_display_standby_cap(on ? percent : -1));
}

bool shell_sys_standby_enabled(void)
{
    return t5_shell_standby_enabled();
}

/* Dim while charging: standby instead. Unplugged: standby goes. */
static void standby_power(t5_event_id_t id, const void *data, size_t len, void *arg)
{
    (void)len; (void)arg;
    t5_battery_status_t b;
    t5_battery_status(&b);
    if (id == T5_EV_POWER_STATE) {
        const t5_ev_power_t *e = data;
        if (e && e->to == T5_PWR_DIM && b.usb && t5_shell_standby_enabled()) T5_LOG_ON_ERR(TAG, t5_shell_standby(true));
    } else if (!b.usb && t5_shell_standby_shown()) {
        T5_LOG_ON_ERR(TAG, t5_shell_standby(false));
    }
}

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
    o->ble_connected = t5_ble_links() > 0;
    o->rotation_locked = !t5_ui_follows_board();
    o->brightness = t5_display_brightness();
    o->volume = t5_audio_volume();
    o->muted = t5_audio_muted();
    o->headphones = t5_audio_headphones();
    o->mic_active = t5_audio_in_active();
    o->cam_active = t5_camera_active();
    o->usb_devices = t5_usb_device_count() > 0;
    o->usb_storage = t5_usbc_mode() == T5_USBC_STORAGE;
    o->audio_playing = t5_audio_out_active();
    o->net_handles = t5_net_handles_open();
    t5_ota_status_t ota;
    t5_ota_status(&ota);
    o->transfer = t5_download_active() > 0 || ota.state == T5_OTA_RECEIVING || ota.state == T5_OTA_VERIFYING;
    char id[T5_APP_ID_LEN];
    o->background = t5_apps_headless(id, sizeof id) || bg_lock_holder(NULL, NULL, 0) > 0;
    o->alarm = t5_apps_alarm_pending(id, sizeof id, NULL);
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

void shell_sys_chrome(shell_chrome_t *out)
{
    memset(out, 0, sizeof *out);
    t5_manifest_t *m = malloc(sizeof *m);
    if (m && t5_apps_fg_manifest(m)) {
        out->fullscreen = m->fullscreen;
    }
    free(m);
}

void shell_sys_insets_changed(void)
{
    T5_LOG_ON_ERR(TAG, t5_event_post(T5_EV_UI_INSETS, NULL, 0));
}

void shell_sys_remove(const char *id)
{
    T5_LOG_ON_ERR(TAG, t5_apps_remove(id, true));
}

void shell_sys_data_dir(const char *id, char *out, size_t n)
{
    if (t5_pkg_data_dir(id, out, n) != T5_OK) snprintf(out, n, "the data folder");
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
    T5_LOG_ON_ERR(TAG, t5_ui_set_follows_board(!on));
}

void shell_sys_set_low_power(bool on)
{
    t5_power_set_low_power(on);
}

void shell_sys_set_brightness(int pct)
{
    /* The quick settings slider starts at 0. */
    T5_LOG_ON_ERR(TAG, t5_display_set_brightness(pct < T5_DISPLAY_BRIGHTNESS_MIN ? T5_DISPLAY_BRIGHTNESS_MIN : pct));
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
    t5_shell_accent_restore();
}

void *shell_sys_enter(void)
{
    return t5_sys_enter();
}

void shell_sys_leave(void *saved)
{
    t5_sys_leave(saved);
}

static void app_notice(t5_apps_notice_t what, const char *name)
{
    const char *why;
    switch (what) {
    case T5_APPS_NOTICE_SAFE_MODE:
        t5_shell_toast("Safe mode: installed apps are off until you restart");
        return;
    case T5_APPS_NOTICE_HUNG: why = "stopped responding and was closed"; break;
    case T5_APPS_NOTICE_LOW_MEMORY: why = "was closed: the device is low on memory"; break;
    default: why = "stopped: it crashed"; break;
    }
    char msg[128];
    snprintf(msg, sizeof msg, "%.40s %s", name ? name : "The app", why);
    t5_shell_toast(msg);
}

static const t5_apps_home_t s_home = {
    .screen = shell_home_screen,
    .refresh = launcher_reload,
    .prepare = shell_prepare,
    .changed = shell_changed,
    .notice = app_notice,
};

static void input_added_ui(void *arg)
{
    (void)arg;
    shell_input_added();
}

static void input_added(t5_event_id_t id, const void *data, size_t len, void *arg)
{
    (void)id; (void)data; (void)len; (void)arg;
    T5_LOG_ON_ERR(TAG, t5_ui_async(input_added_ui, NULL));
}

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
    T5_RETURN_ON_ERR(t5_event_subscribe(T5_EV_USBC_MODE, usbc_event, NULL));
    T5_RETURN_ON_ERR(t5_event_subscribe(T5_EV_POWER_STATE, standby_power, NULL));
    T5_RETURN_ON_ERR(t5_event_subscribe(T5_EV_USB_POWER, standby_power, NULL));
    return t5_event_subscribe(T5_EV_UI_INPUT_ADDED, input_added, NULL);
}

bool shell_sys_safe_mode(char *why, size_t n)
{
    if (!t5_apps_safe_mode()) return false;
    t5_crash_rec_t *rec = calloc(T5_CRASH_MAX_APPS, sizeof *rec);
    int k = rec ? t5_apps_crashes(rec, T5_CRASH_MAX_APPS) : 0;
    int last = -1;
    for (int i = 0; i < k; i++) {
        if (rec[i].total && (last < 0 || rec[i].last_time > rec[last].last_time)) last = i;
    }
    if (last >= 0) {
        snprintf(why, n, "Last crash: %s (%s).", rec[last].id, t5_crash_kind_name((t5_crash_kind_t)rec[last].last_kind));
    } else {
        snprintf(why, n, "The last boots didn't finish.");
    }
    free(rec);
    return true;
}

void shell_sys_restart(void)
{
    t5_power_restart();
}
