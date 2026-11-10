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

static lv_obj_t *s_bar, *s_home, *s_time, *s_mic, *s_cam, *s_usb, *s_kbd, *s_ble, *s_wifi, *s_bat, *s_pct;
static lv_obj_t *s_act[ACT_ICONS], *s_more, *s_bg, *s_alarm, *s_xfer, *s_net, *s_audio;
static bool s_in_app;

static void set_shown(lv_obj_t *o, bool shown)
{
    if (lv_obj_is_hidden(o) == shown) lv_obj_set_hidden(o, !shown);
}

static void set_opa(lv_obj_t *o, lv_opa_t opa)
{
    if (lv_obj_get_style_text_opa(o, 0) != opa) lv_obj_set_style_text_opa(o, opa, 0);
}

static void bar_clicked(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) == s_bar) qs_open();
}

static void home_clicked(lv_event_t *e)
{
    (void)e;
    shell_sys_home();
}

static void home_long(lv_event_t *e)
{
    shell_show_recents(lv_event_get_current_target_obj(e));
    lv_indev_wait_release(lv_indev_active());   /* no click after the long press */
}

static lv_obj_t *icon(lv_obj_t *parent, const char *glyph)
{
    lv_obj_t *l = md_icon_create(parent, glyph, 18);
    lv_obj_set_clickable(l, false);
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

lv_obj_t *statusbar_create(void)
{
    const md_scheme_t *sc = md_scheme();
    s_bar = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_bar);
    lv_obj_set_size(s_bar, LV_PCT(100), T5_DP(SHELL_STATUS_H_DP));
    /* Square and opaque: a plain fill on the PPA path. */
    lv_obj_set_style_bg_color(s_bar, md_color(sc->c.background), 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_bar, md_color(sc->c.on_surface), 0);
    lv_obj_set_style_pad_hor(s_bar, T5_DP(8), 0);
    lv_obj_set_style_pad_column(s_bar, T5_DP(6), 0);
    lv_obj_set_flex_flow(s_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(s_bar, false);
    lv_obj_set_clickable(s_bar, true);
    lv_obj_add_event_cb(s_bar, bar_clicked, LV_EVENT_CLICKED, NULL);

    s_home = lv_obj_create(s_bar);
    lv_obj_remove_style_all(s_home);
    lv_obj_set_size(s_home, T5_DP(24), T5_DP(24));
    lv_obj_set_clickable(s_home, true);
    lv_obj_set_ext_click_area(s_home, T5_DP(12));
    lv_obj_t *hi = icon(s_home, MD_ICON_HOME);
    lv_obj_center(hi);
    lv_obj_add_event_cb(s_home, home_clicked, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(s_home, home_long, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_set_hidden(s_home, true);

    /* Figures in the bar set without the caption's tracking: "100 %" spaced
     * out reads as separate glyphs at this size. */
    s_time = md_label_create(s_bar, "", MD_TYPE_CAPTION);
    lv_obj_set_style_text_opa(s_time, sc->on_surface.high, 0);
    lv_obj_set_style_text_letter_space(s_time, 0, 0);
    lv_obj_set_clickable(s_time, false);
    lv_obj_t *spacer = lv_obj_create(s_bar);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_height(spacer, 1);    /* px: layout spacer */
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_clickable(spacer, false);

    for (int i = 0; i < ACT_ICONS; i++) s_act[i] = icon(s_bar, MD_ICON_SCHEDULE);
    s_more = md_label_create(s_bar, "", MD_TYPE_CAPTION);
    lv_obj_set_style_text_letter_space(s_more, 0, 0);
    lv_obj_set_clickable(s_more, false);
    s_bg = icon(s_bar, MD_ICON_BOLT);
    s_alarm = icon(s_bar, MD_ICON_ALARM);
    s_xfer = icon(s_bar, MD_ICON_DOWNLOAD);
    s_net = icon(s_bar, MD_ICON_SWAP_HORIZ);
    s_audio = icon(s_bar, MD_ICON_MUSIC_NOTE);
    s_mic = icon(s_bar, MD_ICON_MIC);
    lv_obj_set_style_text_color(s_mic, md_color(md_accent(MD_ACCENT_AMBER)->dark), 0);
    s_cam = icon(s_bar, MD_ICON_VIDEOCAM);
    lv_obj_set_style_text_color(s_cam, md_color(md_accent(MD_ACCENT_GREEN)->dark), 0);
    s_usb = icon(s_bar, MD_ICON_USB);
    s_kbd = icon(s_bar, MD_ICON_KEYBOARD);
    s_ble = icon(s_bar, MD_ICON_BLUETOOTH);
    s_wifi = icon(s_bar, MD_ICON_WIFI);
    s_bat = icon(s_bar, MD_ICON_BATTERY_STD);
    s_pct = md_label_create(s_bar, "", MD_TYPE_CAPTION);
    lv_obj_set_style_text_letter_space(s_pct, 0, 0);
    lv_obj_set_clickable(s_pct, false);
    lv_obj_t *hide[] = { s_mic, s_cam, s_usb, s_kbd, s_ble, s_act[0], s_act[1], s_act[2], s_more, s_bg, s_alarm,
                         s_xfer, s_net, s_audio };
    for (size_t i = 0; i < sizeof hide / sizeof hide[0]; i++) lv_obj_set_hidden(hide[i], true);
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
    const md_scheme_t *sc = md_scheme();
    shell_status_t st;
    shell_sys_status(&st);
    shell_show_time(s_time, NULL);
    bool narrow = lv_obj_get_width(s_bar) < T5_DP(NARROW_DP);
    update_activities(narrow ? 1 : ACT_ICONS);
    set_shown(s_bg, st.background);
    set_shown(s_alarm, st.alarm && !narrow);
    set_shown(s_xfer, st.transfer);
    set_shown(s_net, st.net_handles > 0 && !narrow);
    const char *audio = st.muted ? MD_ICON_VOLUME_OFF
                        : st.audio_playing ? (st.headphones ? MD_ICON_HEADPHONES : MD_ICON_MUSIC_NOTE)
                                           : NULL;
    set_shown(s_audio, audio != NULL);
    if (audio) shell_set_text(s_audio, audio);
    set_shown(s_mic, st.mic_active);
    set_shown(s_cam, st.cam_active);
    set_shown(s_usb, st.usb_devices || st.usb_storage);
    set_shown(s_kbd, t5_ui_hw_keyboard());
    set_shown(s_ble, st.ble_on);
    if (st.ble_on) shell_set_text(s_ble, st.ble_connected ? MD_ICON_BLUETOOTH_CONNECTED : MD_ICON_BLUETOOTH);
    set_shown(s_wifi, st.wifi_on);
    if (st.wifi_on) {
        shell_set_text(s_wifi, st.wifi_connected ? MD_ICON_WIFI : MD_ICON_WIFI_OFF);
        lv_opa_t o = !st.wifi_connected ? sc->on_surface.disabled
                     : st.wifi_rssi > -60  ? sc->on_surface.high
                     : st.wifi_rssi > -72  ? sc->on_surface.medium
                                           : sc->on_surface.disabled;
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
