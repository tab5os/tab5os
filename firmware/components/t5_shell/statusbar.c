/* SPDX-License-Identifier: Apache-2.0
 * Status bar (plan 6.3): 24 dp across the top of every screen. Left: the
 * home button (in apps; long press for recents) and the time. Right, in
 * this order:
 *   activities  what apps posted (t5_shell_activity_*), up to three icons
 *               (one in portrait), then a count of the rest
 *   background  an app other than the one in front at work (a wake lock,
 *               its alarm running headless): a bolt
 *   alarm       an app's background wake is scheduled
 *   transfer    an OS download or update in progress
 *   network     network handles are open (any app's)
 *   audio       muted, or something playing (a note, or headphones when
 *               they're in)
 *   privacy     the microphone and camera, in colour
 *   devices     USB, a keyboard, Bluetooth (connected or on), Wi-Fi, battery
 * Quick settings lists the activities with their text and progress. Tap
 * anywhere else on the bar to open it. Narrow (portrait) bars leave out the
 * alarm and network icons.
 *
 * Updated once a second; labels only change when their text does, so an
 * idle status bar draws nothing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shell_priv.h"
#include "t5_ui/t5_ui.h"

#define ACT_ICONS 3
#define NARROW_DP 480          /* below this width: fewer indicators */

static t5ui_obj_t *s_bar, *s_home, *s_time, *s_mic, *s_cam, *s_usb, *s_kbd, *s_ble, *s_wifi, *s_bat, *s_pct;
static t5ui_obj_t *s_act[ACT_ICONS], *s_more, *s_bg, *s_alarm, *s_xfer, *s_net, *s_audio;
static bool s_in_app;

static void set_shown(t5ui_obj_t *o, bool shown)
{
    if (t5ui_get_int(o, T5UI_P_HIDDEN, 0) == shown) t5ui_set_int(o, T5UI_P_HIDDEN, !shown);
}

static void set_opa(t5ui_obj_t *o, t5ui_opa_t opa)
{
    if (t5ui_get_int(o, T5UI_P_TEXT_OPA, -1) != opa) t5ui_set_int(o, T5UI_P_TEXT_OPA, opa);
}

static void bar_clicked(t5ui_event_t *e)
{
    if (t5ui_event_target(e) == s_bar) qs_open();
}

static void home_clicked(t5ui_event_t *e)
{
    (void)e;
    shell_sys_home();
}

static void home_long(t5ui_event_t *e)
{
    shell_show_recents(t5ui_event_obj(e));
    t5ui_cmd(t5ui_event_obj(e), T5UI_CMD_IGNORE_PRESS, 0, 0, 0, NULL);   /* no click after the long press */
}

static t5ui_obj_t *icon(t5ui_obj_t *parent, const char *glyph)
{
    t5ui_obj_t *l = shell_icon(parent, glyph, 18);
    t5ui_set_int(l, T5UI_P_CLICKABLE, false);
    return l;
}

/* A figure in the bar: the caption without its tracking ("100 %" spaced
 * out reads as separate glyphs at this size). */
static t5ui_obj_t *figure(t5ui_obj_t *parent)
{
    t5ui_obj_t *l = shell_text(parent, "", T5UI_TEXT_CAPTION);
    t5ui_set_int(l, T5UI_P_LETTER_SPACE, 0);
    t5ui_set_int(l, T5UI_P_CLICKABLE, false);
    return l;
}

/* The app-posted activities: an icon each, then "+N" for the rest. */
static void update_activities(int room)
{
    shell_activity_t *a = calloc(SHELL_ACTIVITY_MAX, sizeof *a);
    int k = a ? shell_sys_activities(a, SHELL_ACTIVITY_MAX) : 0;
    int shown = 0, more = 0;
    for (int i = 0; i < k; i++) {
        if (!a[i].app) continue;          /* the system's have their own icons */
        if (shown < room) shell_set_text(s_act[shown++], a[i].icon);
        else more++;
    }
    for (int i = 0; i < ACT_ICONS; i++) set_shown(s_act[i], i < shown);
    set_shown(s_more, more > 0);
    if (more) {
        char buf[8];
        snprintf(buf, sizeof buf, "+%d", more);
        shell_set_text(s_more, buf);
    }
    free(a);
}

t5ui_obj_t *statusbar_create(void)
{
    s_bar = shell_box(t5ui_sys_layer(T5UI_LAYER_TOP));
    shell_size(s_bar, T5UI_PCT(100), t5ui_dp(SHELL_STATUS_H_DP));
    /* Square and opaque: a plain fill on the PPA path. */
    t5ui_set_color(s_bar, T5UI_P_BG_COLOR, t5ui_theme_color(T5UI_COLOR_BACKGROUND));
    t5ui_set_int(s_bar, T5UI_P_BG_OPA, T5UI_OPA_COVER);
    t5ui_set_color(s_bar, T5UI_P_TEXT_COLOR, t5ui_theme_color(T5UI_COLOR_ON_SURFACE));
    shell_pad_hor(s_bar, t5ui_dp(8));
    t5ui_set_int(s_bar, T5UI_P_PAD_COLUMN, t5ui_dp(6));
    shell_flex(s_bar, T5UI_ROW, T5UI_PLACE_START, T5UI_PLACE_CENTER, T5UI_PLACE_CENTER);
    t5ui_set_int(s_bar, T5UI_P_SCROLLABLE, false);
    t5ui_set_int(s_bar, T5UI_P_CLICKABLE, true);
    t5ui_on(s_bar, T5UI_EV_CLICK, bar_clicked, NULL);

    s_home = shell_box(s_bar);
    shell_size(s_home, t5ui_dp(24), t5ui_dp(24));
    t5ui_set_int(s_home, T5UI_P_CLICKABLE, true);
    t5ui_set_int(s_home, T5UI_P_EXT_CLICK, t5ui_dp(12));
    t5ui_obj_t *hi = icon(s_home, T5UI_ICON_HOME);
    t5ui_set_int(hi, T5UI_P_ALIGN, T5UI_ALIGN_CENTER);
    t5ui_on(s_home, T5UI_EV_SHORT_CLICK, home_clicked, NULL);
    t5ui_on(s_home, T5UI_EV_LONG_PRESS, home_long, NULL);
    t5ui_set_int(s_home, T5UI_P_HIDDEN, true);

    s_time = figure(s_bar);
    t5ui_set_int(s_time, T5UI_P_TEXT_OPA, t5ui_theme_emphasis(T5UI_EMPHASIS_HIGH));
    t5ui_obj_t *spacer = shell_box(s_bar);
    t5ui_set_int(spacer, T5UI_P_HEIGHT, 1);    /* px: layout spacer */
    t5ui_set_int(spacer, T5UI_P_GROW, 1);
    t5ui_set_int(spacer, T5UI_P_CLICKABLE, false);

    for (int i = 0; i < ACT_ICONS; i++) s_act[i] = icon(s_bar, T5UI_ICON_SCHEDULE);
    s_more = figure(s_bar);
    s_bg = icon(s_bar, T5UI_ICON_BOLT);
    s_alarm = icon(s_bar, T5UI_ICON_ALARM);
    s_xfer = icon(s_bar, T5UI_ICON_DOWNLOAD);
    s_net = icon(s_bar, T5UI_ICON_SWAP_HORIZ);
    s_audio = icon(s_bar, T5UI_ICON_MUSIC_NOTE);
    s_mic = icon(s_bar, T5UI_ICON_MIC);
    t5ui_set_color(s_mic, T5UI_P_TEXT_COLOR, t5ui_accent_color(T5UI_ACCENT_AMBER, T5UI_TONE_DARK));
    s_cam = icon(s_bar, T5UI_ICON_VIDEOCAM);
    t5ui_set_color(s_cam, T5UI_P_TEXT_COLOR, t5ui_accent_color(T5UI_ACCENT_GREEN, T5UI_TONE_DARK));
    s_usb = icon(s_bar, T5UI_ICON_USB);
    s_kbd = icon(s_bar, T5UI_ICON_KEYBOARD);
    s_ble = icon(s_bar, T5UI_ICON_BLUETOOTH);
    s_wifi = icon(s_bar, T5UI_ICON_WIFI);
    s_bat = icon(s_bar, T5UI_ICON_BATTERY_STD);
    s_pct = figure(s_bar);
    t5ui_obj_t *hide[] = { s_mic, s_cam, s_usb, s_kbd, s_ble, s_act[0], s_act[1], s_act[2], s_more, s_bg, s_alarm,
                           s_xfer, s_net, s_audio };
    for (size_t i = 0; i < sizeof hide / sizeof hide[0]; i++) t5ui_set_int(hide[i], T5UI_P_HIDDEN, true);
    return s_bar;
}

void statusbar_set_hidden(bool hidden)
{
    if (s_bar) set_shown(s_bar, !hidden);
}

void statusbar_set_in_app(bool in_app)
{
    s_in_app = in_app;
    if (s_home) set_shown(s_home, in_app);
}

void statusbar_update(void)
{
    if (!s_bar) return;
    shell_status_t st;
    shell_sys_status(&st);
    shell_show_time(s_time, NULL);
    t5ui_rect_t r;
    t5ui_coords(s_bar, &r);
    bool narrow = r.x2 - r.x1 + 1 < t5ui_dp(NARROW_DP);
    update_activities(narrow ? 1 : ACT_ICONS);
    set_shown(s_bg, st.background);
    set_shown(s_alarm, st.alarm && !narrow);
    set_shown(s_xfer, st.transfer);
    set_shown(s_net, st.net_handles > 0 && !narrow);
    const char *audio = st.muted ? T5UI_ICON_VOLUME_OFF
                        : st.audio_playing ? (st.headphones ? T5UI_ICON_HEADPHONES : T5UI_ICON_MUSIC_NOTE)
                                           : NULL;
    set_shown(s_audio, audio != NULL);
    if (audio) shell_set_text(s_audio, audio);
    set_shown(s_mic, st.mic_active);
    set_shown(s_cam, st.cam_active);
    set_shown(s_usb, st.usb_devices || st.usb_storage);
    set_shown(s_kbd, t5_ui_hw_keyboard());
    set_shown(s_ble, st.ble_on);
    if (st.ble_on) shell_set_text(s_ble, st.ble_connected ? T5UI_ICON_BLUETOOTH_CONNECTED : T5UI_ICON_BLUETOOTH);
    set_shown(s_wifi, st.wifi_on);
    if (st.wifi_on) {
        shell_set_text(s_wifi, st.wifi_connected ? T5UI_ICON_WIFI : T5UI_ICON_WIFI_OFF);
        t5ui_opa_t o = !st.wifi_connected ? t5ui_theme_emphasis(T5UI_EMPHASIS_DISABLED)
                       : st.wifi_rssi > -60  ? t5ui_theme_emphasis(T5UI_EMPHASIS_HIGH)
                       : st.wifi_rssi > -72  ? t5ui_theme_emphasis(T5UI_EMPHASIS_MEDIUM)
                                             : t5ui_theme_emphasis(T5UI_EMPHASIS_DISABLED);
        set_opa(s_wifi, o);
    }
    shell_set_text(s_bat, shell_battery_icon(&st));
    set_shown(s_pct, st.battery_pct >= 0);
    if (st.battery_pct >= 0) {
        char buf[8];
        snprintf(buf, sizeof buf, "%d%%", st.battery_pct);
        shell_set_text(s_pct, buf);
    }
}

void statusbar_reset(void)
{
    s_bar = s_home = s_time = s_mic = s_cam = s_usb = s_kbd = s_ble = s_wifi = s_bat = s_pct = NULL;
    for (int i = 0; i < ACT_ICONS; i++) s_act[i] = NULL;
    s_more = s_bg = s_alarm = s_xfer = s_net = s_audio = NULL;
    s_in_app = false;
}
