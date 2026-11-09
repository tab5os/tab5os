/* SPDX-License-Identifier: Apache-2.0
 * Settings (plan 6.5): Wi-Fi, Bluetooth, display, sound, power, time, USB,
 * updates, developer mode and About (with the licences). The launch
 * argument opens a page directly: wifi, ble, display, sound, power, time,
 * usb, updates, developer, about (quick settings uses it).
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_psram.h"
#include "t5/audio.h"
#include "t5/power.h"
#include "t5/shell.h"
#include "t5_shell/accent.h"
#include "t5/storage.h"
#include "t5/sys.h"
#include "t5/system.h"
#include "sysapp_system.h"
#include "t5/usb.h"
#include "t5_dev/dev.h"
#include "t5_net/ble_sys.h"
#include "t5_ota/ota.h"
#include "sysapps_priv.h"


static sa_nav_t s_nav;

/* ---- small builders ------------------------------------------------------------------ */
typedef void (*toggle_fn_t)(bool on);

static void switch_changed(lv_event_t *e)
{
    toggle_fn_t fn = (toggle_fn_t)lv_event_get_user_data(e);
    fn(lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
}

static lv_obj_t *switch_row(lv_obj_t *body, const char *icon, const char *text, const char *sub, bool on,
                            toggle_fn_t fn)
{
    lv_obj_t *r = md_row_create(body, icon, text, sub);
    lv_obj_t *sw = md_row_switch(r, on);
    lv_obj_add_event_cb(sw, switch_changed, LV_EVENT_VALUE_CHANGED, (void *)fn);
    return sw;
}

static void set_switch(lv_obj_t *sw, bool on)
{
    if (!sw || lv_obj_has_state(sw, LV_STATE_PRESSED)) return;
    if (on != lv_obj_has_state(sw, LV_STATE_CHECKED)) {
        if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
        else lv_obj_remove_state(sw, LV_STATE_CHECKED);
    }
}

static lv_obj_t *slider_row(lv_obj_t *body, const char *icon, int32_t min, int32_t max, int32_t v, lv_event_cb_t cb)
{
    lv_obj_t *r = lv_obj_create(body);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), T5_DP(56));
    lv_obj_set_style_pad_hor(r, T5_DP(16), 0);
    lv_obj_set_style_pad_column(r, T5_DP(24), 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(r, false);
    lv_obj_t *i = md_icon_create(r, icon, MD_ICON_DP);
    lv_obj_set_style_text_opa(i, md_scheme()->on_surface.medium, 0);
    lv_obj_t *s = md_slider_create(r, min, max, v);
    lv_obj_set_flex_grow(s, 1);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return s;
}

static lv_obj_t *chips(lv_obj_t *body)
{
    lv_obj_t *c = sa_button_row(body);
    return c;
}

/* ---- Wi-Fi ----------------------------------------------------------------------------- */
typedef struct {
    lv_obj_t *sw, *status, *list, *saved;
    t5_net_ap_t aps[T5_NET_MAX_SCAN];
    int naps;
    uint32_t shown_hash;
    char saved_ssids[8][T5_NET_SSID_LEN];
    int nsaved;
} wifi_page_t;

static void wifi_toggle(bool on)
{
    t5_net_enable(on);
}

typedef struct {
    char ssid[T5_NET_SSID_LEN];
    lv_obj_t *field;
} pass_page_t;

static void pass_connect(lv_event_t *e)
{
    pass_page_t *p = lv_event_get_user_data(e);
    const char *pw = md_textfield_text(p->field);
    char msg[80];
    if (t5_net_connect(p->ssid, pw, true) == T5_OK) snprintf(msg, sizeof msg, "Connecting to %s", p->ssid);
    else snprintf(msg, sizeof msg, "Couldn't start connecting to %s", p->ssid);
    md_snackbar_show(msg, NULL, NULL, NULL);
    t5_shell_keyboard_hide();
    sa_pop(&s_nav);
}

static void pass_show(lv_event_t *e)
{
    pass_page_t *p = lv_event_get_user_data(e);
    lv_textarea_set_password_mode(md_textfield_ta(p->field), !lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
}

static void pass_ready(lv_event_t *e)
{
    pass_connect(e);
}

static void open_password(const char *ssid)
{
    pass_page_t *p = calloc(1, sizeof *p);
    if (!p) return;
    snprintf(p->ssid, sizeof p->ssid, "%s", ssid);
    lv_obj_t *body = sa_push(&s_nav, ssid);
    lv_obj_t *box = lv_obj_create(body);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(box, T5_DP(16), 0);
    lv_obj_set_style_pad_row(box, T5_DP(8), 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    p->field = md_textfield_create(box, "Password", true);
    md_checkbox_create(box, "Show password", false);
    lv_obj_t *cb = lv_obj_get_child(box, 1);
    lv_obj_add_event_cb(cb, pass_show, LV_EVENT_VALUE_CHANGED, p);
    lv_obj_add_event_cb(md_textfield_ta(p->field), pass_ready, LV_EVENT_READY, p);
    lv_obj_t *row = sa_button_row(body);
    lv_obj_t *b = md_button_create(row, "Connect", MD_BUTTON_CONTAINED);
    lv_obj_add_event_cb(b, pass_connect, LV_EVENT_CLICKED, p);
    sa_set_refresh(&s_nav, NULL, p, free);
    lv_obj_t *ta = md_textfield_ta(p->field);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    lv_obj_send_event(ta, LV_EVENT_FOCUSED, NULL);
    t5_shell_keyboard_show(ta);
}

static void ap_clicked(lv_event_t *e)
{
    wifi_page_t *w = s_nav.lv[s_nav.depth - 1].arg;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= w->naps) return;
    t5_net_ap_t *ap = &w->aps[i];
    if (ap->saved || ap->auth == 0) {
        t5_net_connect(ap->ssid, ap->auth == 0 ? "" : NULL, true);
        char msg[80];
        snprintf(msg, sizeof msg, "Connecting to %s", ap->ssid);
        md_snackbar_show(msg, NULL, NULL, NULL);
    } else {
        open_password(ap->ssid);
    }
}

typedef struct {
    char ssid[T5_NET_SSID_LEN];
} forget_t;

static void forget_answer(int b, void *ud)
{
    forget_t *f = ud;
    if (b == 0) {
        t5_net_forget(f->ssid);
        md_snackbar_show("Network forgotten", NULL, NULL, NULL);
    }
    free(f);
}

static void saved_clicked(lv_event_t *e)
{
    wifi_page_t *w = s_nav.lv[s_nav.depth - 1].arg;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= w->nsaved) return;
    forget_t *f = calloc(1, sizeof *f);
    if (!f) return;
    snprintf(f->ssid, sizeof f->ssid, "%s", w->saved_ssids[i]);
    char title[80];
    snprintf(title, sizeof title, "Forget %s?", f->ssid);
    md_dialog_show(title, "Tab5 OS won't join this network on its own any more.", "Forget", "Cancel", forget_answer, f);
}

static void scan_clicked(lv_event_t *e)
{
    (void)e;
    if (t5_net_scan() == T5_OK) md_snackbar_show("Scanning...", NULL, NULL, NULL);
}

static uint32_t ap_hash(const t5_net_ap_t *a, int n)
{
    uint32_t h = (uint32_t)n * 2654435761u;
    for (int i = 0; i < n; i++) {
        for (const char *p = a[i].ssid; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
        h ^= (uint32_t)(a[i].rssi / 8) + a[i].saved;
    }
    return h;
}

static void wifi_refresh(sa_nav_t *nav, void *arg)
{
    (void)nav;
    wifi_page_t *w = arg;
    t5_net_status_t st;
    t5_net_status(&st);
    set_switch(w->sw, st.enabled);
    char buf[128];
    if (!st.enabled) snprintf(buf, sizeof buf, "Wi-Fi is off");
    else if (st.state == T5_NET_CONNECTED)
        snprintf(buf, sizeof buf, "Connected to %s, %d dBm, %u.%u.%u.%u", st.ssid, st.rssi, st.ip[0], st.ip[1], st.ip[2],
                 st.ip[3]);
    else snprintf(buf, sizeof buf, "%s", t5_net_state_name(st.state));
    sa_set_text(w->status, buf);

    w->naps = t5_net_scan_results(w->aps, T5_NET_MAX_SCAN);
    char ss[8][T5_NET_SSID_LEN];
    int ns = t5_net_saved_list(ss, 8);
    uint32_t h = ap_hash(w->aps, w->naps) ^ ((uint32_t)ns << 24);
    for (int i = 0; i < ns; i++)
        for (const char *p = ss[i]; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    if (h == w->shown_hash) return;
    w->shown_hash = h;
    memcpy(w->saved_ssids, ss, sizeof ss);
    w->nsaved = ns;
    lv_obj_clean(w->list);
    for (int i = 0; i < w->naps; i++) {
        t5_net_ap_t *a = &w->aps[i];
        if (!a->ssid[0]) continue;
        snprintf(buf, sizeof buf, "%d dBm · channel %u%s%s", a->rssi, a->channel, a->auth ? " · secured" : " · open",
                 a->saved ? " · saved" : "");
        lv_obj_t *r = md_row_create(w->list, a->auth ? MD_ICON_WIFI_LOCK : MD_ICON_WIFI, a->ssid, buf);
        md_row_set_clickable(r, ap_clicked, (void *)(intptr_t)i);
        if (st.state == T5_NET_CONNECTED && !strcmp(st.ssid, a->ssid)) md_row_trailing_text(r, "Connected");
    }
    if (!w->naps) sa_note(w->list, "No networks found yet. Tap Scan.");
    lv_obj_clean(w->saved);
    for (int i = 0; i < ns; i++) {
        lv_obj_t *r = md_row_create(w->saved, MD_ICON_WIFI, ss[i], "Tap to forget");
        md_row_set_clickable(r, saved_clicked, (void *)(intptr_t)i);
    }
}

static void page_wifi(void)
{
    wifi_page_t *w = calloc(1, sizeof *w);
    if (!w) return;
    lv_obj_t *body = sa_push(&s_nav, "Wi-Fi");
    md_appbar_add_action(sa_bar(&s_nav), MD_ICON_REFRESH, scan_clicked, NULL);
    t5_net_status_t st;
    t5_net_status(&st);
    w->sw = switch_row(body, MD_ICON_WIFI, "Use Wi-Fi", NULL, st.enabled, wifi_toggle);
    w->status = sa_note(body, "");
    md_section_create(body, "Networks");
    w->list = lv_obj_create(body);
    lv_obj_remove_style_all(w->list);
    lv_obj_set_size(w->list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(w->list, LV_FLEX_FLOW_COLUMN);
    md_section_create(body, "Saved");
    w->saved = lv_obj_create(body);
    lv_obj_remove_style_all(w->saved);
    lv_obj_set_size(w->saved, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(w->saved, LV_FLEX_FLOW_COLUMN);
    if (st.enabled && t5_net_scan_results(w->aps, 1) == 0) t5_net_scan();
    sa_set_refresh(&s_nav, wifi_refresh, w, free);
}

/* ---- Bluetooth --------------------------------------------------------------------------- */
typedef struct {
    lv_obj_t *sw, *status, *list;
    t5_ble_dev_t devs[T5_BLE_MAX_SCAN];
    int n, shown;
} ble_page_t;

static void ble_toggle(bool on)
{
    t5_ble_enable(on);
}

static void ble_scan_clicked(lv_event_t *e)
{
    (void)e;
    if (!t5_ble_ready()) {
        md_snackbar_show("Turn Bluetooth on first", NULL, NULL, NULL);
        return;
    }
    if (t5_ble_sys_scan(5000) == T5_OK) md_snackbar_show("Scanning for 5 s...", NULL, NULL, NULL);
}

static void ble_dev_clicked(lv_event_t *e)
{
    ble_page_t *b = s_nav.lv[s_nav.depth - 1].arg;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= b->n) return;
    if (t5_ble_sys_connected()) {
        t5_ble_sys_disconnect();
        md_snackbar_show("Disconnected", NULL, NULL, NULL);
        return;
    }
    t5_err_t err = t5_ble_sys_connect(b->devs[i].addr, b->devs[i].addr_type);
    md_snackbar_show(err == T5_OK ? "Connecting..." : "Couldn't connect", NULL, NULL, NULL);
}

static void ble_refresh(sa_nav_t *nav, void *arg)
{
    (void)nav;
    ble_page_t *b = arg;
    bool on = t5_ble_ready();
    set_switch(b->sw, on);
    sa_set_text(b->status, !on ? "Bluetooth is off" : t5_ble_sys_connected() ? "Connected to a device" : "On, not connected");
    b->n = t5_ble_sys_scan_results(b->devs, T5_BLE_MAX_SCAN);
    if (b->n == b->shown) return;
    b->shown = b->n;
    lv_obj_clean(b->list);
    char addr[18], sub[48];
    for (int i = 0; i < b->n; i++) {
        t5_ble_format_addr(b->devs[i].addr, addr);
        snprintf(sub, sizeof sub, "%s · %d dBm", addr, b->devs[i].rssi);
        lv_obj_t *r = md_row_create(b->list, MD_ICON_BLUETOOTH, b->devs[i].name[0] ? b->devs[i].name : "(no name)", sub);
        md_row_set_clickable(r, ble_dev_clicked, (void *)(intptr_t)i);
    }
}

static void page_ble(void)
{
    ble_page_t *b = calloc(1, sizeof *b);
    if (!b) return;
    lv_obj_t *body = sa_push(&s_nav, "Bluetooth");
    md_appbar_add_action(sa_bar(&s_nav), MD_ICON_REFRESH, ble_scan_clicked, NULL);
    b->sw = switch_row(body, MD_ICON_BLUETOOTH, "Use Bluetooth", "Bluetooth LE central, through the C6", t5_ble_ready(),
                       ble_toggle);
    b->status = sa_note(body, "");
    md_section_create(body, "Devices");
    b->list = lv_obj_create(body);
    lv_obj_remove_style_all(b->list);
    lv_obj_set_size(b->list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(b->list, LV_FLEX_FLOW_COLUMN);
    b->shown = -1;
    sa_set_refresh(&s_nav, ble_refresh, b, free);
}

/* ---- Display ------------------------------------------------------------------------------ */
static void bright_changed(lv_event_t *e)
{
    t5_display_set_brightness(lv_slider_get_value(lv_event_get_target_obj(e)));
}

static void autorot_toggle(bool on)
{
    sa_report(t5_ui_set_follows_board(on), "change auto-rotate");
}

static void stats_toggle(bool on)
{
    t5_ui_show_stats(on);
}

static void accent_clicked(lv_event_t *e)
{
    t5_shell_accent_set((md_accent_t)(intptr_t)lv_event_get_user_data(e));
}

static void rot_clicked(lv_event_t *e)
{
    t5_ui_set_rotation_lock(true);
    t5_ui_set_rotation((t5_rotation_t)(intptr_t)lv_event_get_user_data(e));
}

static void profile_clicked(lv_event_t *e)
{
    sa_report(t5_power_set_profile((t5_power_profile_t)(intptr_t)lv_event_get_user_data(e)), "change the power profile");
}

static void page_display(void)
{
    lv_obj_t *body = sa_push(&s_nav, "Display");
    md_section_create(body, "Brightness");
    slider_row(body, MD_ICON_BRIGHTNESS_6, T5_DISPLAY_BRIGHTNESS_MIN, 100, t5_display_brightness(), bright_changed);
    md_section_create(body, "Rotation");
    switch_row(body, MD_ICON_SCREEN_ROTATION, "Auto-rotate", "Off keeps the current orientation, across reboots",
               t5_ui_follows_board(), autorot_toggle);
    lv_obj_t *c = chips(body);
    static const char *names[] = { "Portrait", "Landscape", "Portrait (flipped)", "Landscape (flipped)" };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *ch = md_chip_create(c, names[i], NULL, MD_CHIP_CHOICE);
        if ((int)t5_ui_rotation() == i) md_chip_set_selected(ch, true);
        lv_obj_add_event_cb(ch, rot_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    md_section_create(body, "Accent colour");
    c = chips(body);
    for (int i = 0; i < MD_ACCENT_COUNT; i++) {
        lv_obj_t *ch = md_chip_create(c, md_accent((md_accent_t)i)->name, MD_ICON_PALETTE, MD_CHIP_CHOICE);
        if (md_get_accent() == (md_accent_t)i) md_chip_set_selected(ch, true);
        lv_obj_add_event_cb(ch, accent_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    md_section_create(body, "Screen timeout");
    c = chips(body);
    static const char *prof[] = { "Handheld", "Mounted" };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *ch = md_chip_create(c, prof[i], NULL, MD_CHIP_CHOICE);
        if ((int)t5_power_profile() == i) md_chip_set_selected(ch, true);
        lv_obj_add_event_cb(ch, profile_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    t5_power_timeouts_t to;
    t5_power_get_timeouts(&to);
    char buf[128], a[24], b[24];
    sa_fmt_duration(a, sizeof a, to.screen_off_ms / 1000);
    sa_fmt_duration(b, sizeof b, to.soft_off_ms / 1000);
    snprintf(buf, sizeof buf, "Screen off after %s idle; off completely after %s asleep%s.", a, b,
             t5_power_profile() == T5_PROFILE_MOUNTED ? " (mounted: never)" : "");
    sa_note(body, buf);
    md_section_create(body, "Theme");
    sa_note(body, "Dark theme. The light theme is built but not tuned yet, so it stays hidden until after 1.0.");
    switch_row(body, MD_ICON_DASHBOARD, "Frame counters", "Frame rate and render time on screen", false, stats_toggle);
}

/* ---- Sound --------------------------------------------------------------------------------- */
typedef struct {
    lv_obj_t *mute, *status;
} sound_page_t;

static void vol_changed(lv_event_t *e)
{
    t5_audio_set_volume(lv_slider_get_value(lv_event_get_target_obj(e)));
}

static void vol_released(lv_event_t *e)
{
    (void)e;
    t5_audio_play_sound(T5_SOUND_NOTIFY);
}

static void mute_toggle(bool on)
{
    t5_audio_set_muted(on);
}

static void sounds_toggle(bool on)
{
    t5_audio_set_system_sounds(on);
    if (on) t5_audio_play_sound(T5_SOUND_CLICK);
}

static void gain_changed(lv_event_t *e)
{
    t5_audio_in_set_gain_db((float)lv_slider_get_value(lv_event_get_target_obj(e)));
}

static void tone_task(void *arg)
{
    (void)arg;
    t5_audio_tone(440, 800);
}

static void tone_clicked(lv_event_t *e)
{
    (void)e;
    t5_task_create("tone", tone_task, NULL, 4096, 5, T5_CORE_ANY, NULL);
}

static void sound_refresh(sa_nav_t *nav, void *arg)
{
    (void)nav;
    sound_page_t *p = arg;
    t5_audio_stats_t st;
    t5_audio_stats(&st);
    char buf[160];
    if (!st.ready) snprintf(buf, sizeof buf, "The audio codecs didn't answer at boot.");
    else snprintf(buf, sizeof buf, "Output to the %s%s. %u underruns since boot.", st.headphones ? "headphones" : "speaker",
                  st.out_running ? " (playing)" : "", (unsigned)st.underruns);
    sa_set_text(p->status, buf);
    set_switch(p->mute, t5_audio_muted());
}

static void page_sound(void)
{
    sound_page_t *p = calloc(1, sizeof *p);
    if (!p) return;
    lv_obj_t *body = sa_push(&s_nav, "Sound");
    md_section_create(body, "Volume");
    lv_obj_t *v = slider_row(body, MD_ICON_VOLUME_UP, 0, 100, t5_audio_volume(), vol_changed);
    lv_obj_add_event_cb(v, vol_released, LV_EVENT_RELEASED, NULL);
    p->mute = switch_row(body, MD_ICON_VOLUME_OFF, "Mute", NULL, t5_audio_muted(), mute_toggle);
    switch_row(body, MD_ICON_NOTIFICATIONS, "System sounds", "Clicks and notifications", t5_audio_system_sounds(),
               sounds_toggle);
    p->status = sa_note(body, "");
    lv_obj_t *row = sa_button_row(body);
    lv_obj_t *b = md_button_create(row, "Play test tone", MD_BUTTON_OUTLINED);
    lv_obj_add_event_cb(b, tone_clicked, LV_EVENT_CLICKED, NULL);
    md_section_create(body, "Microphones");
    slider_row(body, MD_ICON_MIC, 0, 37, (int32_t)t5_audio_in_gain_db(), gain_changed);
    sa_note(body, "Input gain for both microphones, 0 to 37 dB. Apps ask before they use them, and the status bar "
                  "shows a microphone while one is listening.");
    sa_set_refresh(&s_nav, sound_refresh, p, free);
}

/* ---- Power --------------------------------------------------------------------------------- */
typedef struct {
    lv_obj_t *status, *saver, *limit, *fast;
} power_page_t;

static void saver_toggle(bool on)
{
    t5_power_set_low_power(on);
}

static void limit_toggle(bool on)
{
    t5_battery_set_charge_limit(on);
}

static void fast_toggle(bool on)
{
    t5_battery_set_fast_charge(on);
}

static void open_power(lv_event_t *e)
{
    (void)e;
    sa_report(t5_apps_launch(T5_APP_ID_POWER), "open Power");
}

static void power_refresh(sa_nav_t *nav, void *arg)
{
    (void)nav;
    power_page_t *p = arg;
    t5_battery_status_t b;
    t5_battery_status(&b);
    char buf[128];
    if (!b.ready || b.percent < 0) snprintf(buf, sizeof buf, "Battery: measuring");
    else snprintf(buf, sizeof buf, "%d%% · %u mV · %s", b.percent, b.mv, b.usb ? "USB power" : "on battery");
    sa_set_text(p->status, buf);
    set_switch(p->saver, t5_power_low_power());
    set_switch(p->limit, t5_battery_charge_limit());
    set_switch(p->fast, t5_battery_fast_charge());
}

static void page_power(void)
{
    power_page_t *p = calloc(1, sizeof *p);
    if (!p) return;
    lv_obj_t *body = sa_push(&s_nav, "Power");
    p->status = sa_note(body, "");
    p->saver = switch_row(body, MD_ICON_BATTERY_SAVER, "Battery saver", "30 fps, plain ripples, shorter timers",
                          t5_power_low_power(), saver_toggle);
    p->limit = switch_row(body, MD_ICON_BATTERY_CHARGING_FULL, "Stop charging at 80%",
                          "Better for the pack when the Tab5 lives on USB", t5_battery_charge_limit(), limit_toggle);
    p->fast = switch_row(body, MD_ICON_BOLT, "Fast charging", NULL, t5_battery_fast_charge(), fast_toggle);
    lv_obj_t *r = md_row_create(body, MD_ICON_OPEN_IN_NEW, "Power app", "Live draw, history and per-app use");
    md_row_set_clickable(r, open_power, NULL);
    sa_set_refresh(&s_nav, power_refresh, p, free);
}

/* ---- Time ----------------------------------------------------------------------------------- */
typedef struct {
    lv_obj_t *now, *src, *date, *time, *zone;
    char tz[48];                 /* the zone the fields were filled in */
} time_page_t;

static void fill_fields(time_page_t *p)
{
    struct tm tm;
    if (!t5_time_local(&tm)) return;
    char buf[16];
    strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    md_textfield_set_text(p->date, buf);
    strftime(buf, sizeof buf, "%H:%M", &tm);
    md_textfield_set_text(p->time, buf);
}

static void sync_clicked(lv_event_t *e)
{
    (void)e;
    t5_err_t err = t5_net_sync_time();
    md_snackbar_show(err == T5_OK ? "Asking the network for the time" : err == T5_ERR_STATE ? "Connect to Wi-Fi first"
                                                                                         : "Couldn't ask for the time",
                     NULL, NULL, NULL);
}

static void zone_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    const char *posix;
    if (t5_time_zone(i, NULL, &posix)) t5_time_set_tz(posix);
    sa_pop(&s_nav);
}

static void open_zones(lv_event_t *e)
{
    (void)e;
    lv_obj_t *body = sa_push(&s_nav, "Time zone");
    for (int i = 0; i < t5_time_zone_count(); i++) {
        const char *name, *posix;
        t5_time_zone(i, &name, &posix);
        lv_obj_t *r = md_row_create(body, !strcmp(posix, t5_time_tz()) ? MD_ICON_CHECK : NULL, name, posix);
        md_row_set_clickable(r, zone_clicked, (void *)(intptr_t)i);
    }
}

static void time_set_clicked(lv_event_t *e)
{
    time_page_t *p = lv_event_get_user_data(e);
    int Y, M, D, h, m;
    if (sscanf(md_textfield_text(p->date), "%d-%d-%d", &Y, &M, &D) != 3 ||
        sscanf(md_textfield_text(p->time), "%d:%d", &h, &m) != 2 || M < 1 || M > 12 || D < 1 || D > 31 || h < 0 ||
        h > 23 || m < 0 || m > 59 || Y < 2024) {
        md_textfield_set_helper(p->date, "Use YYYY-MM-DD and HH:MM", true);
        return;
    }
    struct tm tm = { .tm_year = Y - 1900, .tm_mon = M - 1, .tm_mday = D, .tm_hour = h, .tm_min = m, .tm_isdst = -1 };
    time_t utc = mktime(&tm);        /* local, through the zone */
    t5_err_t err = t5_time_set(utc, T5_TIME_SRC_USER);
    md_textfield_set_helper(p->date, NULL, false);
    md_snackbar_show(err == T5_OK ? "Time set" : "Couldn't set the time", NULL, NULL, NULL);
    t5_shell_keyboard_hide();
}

static void time_refresh(sa_nav_t *nav, void *arg)
{
    (void)nav;
    time_page_t *p = arg;
    struct tm tm;
    char buf[96];
    bool ok = t5_time_local(&tm);
    if (ok) strftime(buf, sizeof buf, "%A %d %B %Y, %H:%M:%S", &tm);
    else snprintf(buf, sizeof buf, "Not set");
    sa_set_text(p->now, buf);
    static const char *src[] = { "not set", "the RTC", "set by hand", "the network (NTP)" };
    t5_time_source_t s = t5_time_source();
    snprintf(buf, sizeof buf, "From %s · zone %s", src[s <= T5_TIME_SRC_NTP ? s : 0], t5_time_tz_name());
    sa_set_text(p->src, buf);
    /* The zone changed (its page, or elsewhere): show it, and refill the
     * fields in it unless someone is typing there. */
    if (strcmp(p->tz, t5_time_tz())) {
        snprintf(p->tz, sizeof p->tz, "%s", t5_time_tz());
        md_row_set_secondary(p->zone, t5_time_tz_name());
        if (!lv_obj_has_state(md_textfield_ta(p->date), LV_STATE_FOCUSED) &&
            !lv_obj_has_state(md_textfield_ta(p->time), LV_STATE_FOCUSED))
            fill_fields(p);
    }
}

static void page_time(void)
{
    time_page_t *p = calloc(1, sizeof *p);
    if (!p) return;
    lv_obj_t *body = sa_push(&s_nav, "Date and time");
    p->now = md_label_create(body, "", MD_TYPE_HEADLINE6);
    lv_obj_set_style_pad_all(p->now, T5_DP(16), 0);
    p->src = sa_note(body, "");
    sa_note(body, "The clock sets itself from the network when Wi-Fi is connected, and the RTC keeps it through soft "
                  "off.");
    lv_obj_t *r = md_row_create(body, MD_ICON_LANGUAGE, "Time zone", t5_time_tz_name());
    md_row_set_clickable(r, open_zones, NULL);
    p->zone = r;
    snprintf(p->tz, sizeof p->tz, "%s", t5_time_tz());
    r = md_row_create(body, MD_ICON_REFRESH, "Sync with the network now", "Needs Wi-Fi");
    md_row_set_clickable(r, sync_clicked, NULL);
    md_section_create(body, "Set by hand");
    lv_obj_t *box = lv_obj_create(body);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(box, T5_DP(16), 0);
    lv_obj_set_style_pad_gap(box, T5_DP(16), 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    p->date = md_textfield_create(box, "Date (YYYY-MM-DD)", false);
    lv_obj_set_flex_grow(p->date, 1);
    p->time = md_textfield_create(box, "Time (HH:MM)", false);
    lv_obj_set_flex_grow(p->time, 1);
    fill_fields(p);
    lv_obj_t *row = sa_button_row(body);
    lv_obj_t *b = md_button_create(row, "Set time", MD_BUTTON_CONTAINED);
    lv_obj_add_event_cb(b, time_set_clicked, LV_EVENT_CLICKED, p);
    sa_set_refresh(&s_nav, time_refresh, p, free);
}

/* ---- USB -------------------------------------------------------------------------------------- */
typedef struct {
    lv_obj_t *sw, *list, *mode;
    int shown, mode_shown;
} usb_page_t;

static void usb_toggle(bool on)
{
    t5_usb_host_enable(on);
}

static void msc_answer(int b, void *ud)
{
    (void)ud;
    if (b != 0) return;
    t5_err_t e = t5_usbc_set_mode(T5_USBC_STORAGE, 0);    /* until the computer ejects it */
    if (e != T5_OK) md_snackbar_show("No SD card to share", NULL, NULL, NULL);
}

static void take_back_answer(int b, void *ud)
{
    (void)ud;
    if (b == 0) t5_usbc_set_mode(T5_USBC_CONSOLE, 0);
}

static void msc_clicked(lv_event_t *e)
{
    (void)e;
    if (t5_usbc_mode() == T5_USBC_STORAGE) {
        md_dialog_show("Take the SD card back now?",
                       "Eject the drive on the computer first. Taking it back while the computer is using it can "
                       "lose files it was writing.",
                       "Take it back", "Cancel", take_back_answer, NULL);
        return;
    }
    md_dialog_show("Hand the SD card to a computer?",
                   "A computer on the USB-C port gets the SD card as a drive and keeps it until you eject it "
                   "there. Meanwhile apps can't use /sd, the developer console and sound stop, and /data is "
                   "never shared.",
                   "Hand over", "Cancel", msc_answer, NULL);
}

static void usb_refresh(sa_nav_t *nav, void *arg)
{
    (void)nav;
    usb_page_t *p = arg;
    set_switch(p->sw, t5_usb_host_enabled());
    int m = (int)t5_usbc_mode();
    if (m != p->mode_shown) {
        p->mode_shown = m;
        md_row_set_secondary(p->mode, m == T5_USBC_STORAGE ? "The computer has the SD card: eject it there to take it back"
                                                           : "Developer console (tap to hand the SD card to a computer)");
    }
    t5_usb_dev_t d[T5_USB_MAX_DEVICES];
    int n = t5_usb_devices(d, T5_USB_MAX_DEVICES);
    int h = n;
    for (int i = 0; i < n; i++) h = h * 31 + d[i].addr + d[i].cls * 7;
    if (h == p->shown) return;
    p->shown = h;
    lv_obj_clean(p->list);
    static const char *icons[T5_USB_CLASS_COUNT] = { MD_ICON_USB, MD_ICON_HUB, MD_ICON_KEYBOARD, MD_ICON_MOUSE,
                                                     MD_ICON_STORAGE, MD_ICON_TERMINAL, MD_ICON_PIANO,
                                                     MD_ICON_VIDEOCAM, MD_ICON_HEADPHONES };
    char sub[64];
    for (int i = 0; i < n; i++) {
        snprintf(sub, sizeof sub, "%s · %04x:%04x · %s speed", t5_usb_class_name(d[i].cls), d[i].vid, d[i].pid,
                 d[i].speed == 2 ? "high" : d[i].speed == 1 ? "full" : "low");
        md_row_create(p->list, icons[d[i].cls < T5_USB_CLASS_COUNT ? d[i].cls : 0],
                      d[i].product[0] ? d[i].product : "USB device", sub);
    }
    if (!n) sa_note(p->list, t5_usb_host_enabled() ? "Nothing plugged in." : "USB-A is off.");
}

static void page_usb(void)
{
    usb_page_t *p = calloc(1, sizeof *p);
    if (!p) return;
    p->shown = p->mode_shown = -1;
    lv_obj_t *body = sa_push(&s_nav, "USB");
    md_section_create(body, "USB-A");
    p->sw = switch_row(body, MD_ICON_USB, "USB-A port", "Power and drivers for keyboards, drives, cameras, MIDI",
                       t5_usb_host_enabled(), usb_toggle);
    p->list = lv_obj_create(body);
    lv_obj_remove_style_all(p->list);
    lv_obj_set_size(p->list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(p->list, LV_FLEX_FLOW_COLUMN);
    md_section_create(body, "USB-C");
    p->mode = md_row_create(body, MD_ICON_SD_STORAGE, "SD card to a computer", " ");
    md_row_set_clickable(p->mode, msc_clicked, NULL);
    sa_set_refresh(&s_nav, usb_refresh, p, free);
}

/* ---- Updates (plan 9.1-9.3) ---------------------------------------------------------------------- */
typedef struct {
    lv_obj_t *state, *list;
    int shown;
} upd_page_t;

static char s_upd_path[128];

static void upd_task(void *arg)
{
    (void)arg;
    char err[128] = "";
    t5_err_t e = t5_ota_install_file(s_upd_path, T5_OTA_FROM_SETTINGS, t5_dev_enabled(), err, sizeof err);
    char msg[160];
    if (e == T5_OK) snprintf(msg, sizeof msg, "Installed. Restart to run it.");
    else snprintf(msg, sizeof msg, "Not installed: %s", err);
    t5_shell_toast(msg);
}

static void upd_answer(int b, void *ud)
{
    (void)ud;
    if (b != 0) return;
    if (t5_task_create("t5_update", upd_task, NULL, 8192, 3, T5_CORE_ANY, NULL) != T5_OK) {
        md_snackbar_show("Couldn't start the update", NULL, NULL, NULL);
    }
}

static void upd_file_clicked(lv_event_t *e)
{
    const char *path = lv_event_get_user_data(e);
    snprintf(s_upd_path, sizeof s_upd_path, "%s", path);
    char text[200];
    snprintf(text, sizeof text, "Install %s into the other system slot? The current system keeps running until you "
                                "restart, and comes back by itself if the new one doesn't start properly.", path);
    md_dialog_show("Install this update?", text, "Install", "Cancel", upd_answer, NULL);
}

static void restart_answer(int b, void *ud)
{
    if (b != 0) return;
    /* These return only if the restart didn't happen. */
    sa_report(ud ? t5_ota_reboot_to_recovery() : t5_ota_reboot_safe_mode(), "restart");
}

static void recovery_clicked(lv_event_t *e)
{
    (void)e;
    md_dialog_show("Restart into recovery?",
                   "Recovery can reinstall the system from the SD card, clear settings and save the logs to the card.",
                   "Restart", "Cancel", restart_answer, (void *)1);
}

static void safemode_clicked(lv_event_t *e)
{
    (void)e;
    md_dialog_show("Restart in safe mode?", "Installed apps stay off until the next restart.", "Restart", "Cancel",
                   restart_answer, NULL);
}

static void restart_now(lv_event_t *e)
{
    (void)e;
    t5_power_restart();
}

static void upd_refresh(sa_nav_t *nav, void *arg)
{
    (void)nav;
    upd_page_t *p = arg;
    t5_ota_status_t st;
    t5_ota_status(&st);
    char buf[160];
    switch (st.state) {
    case T5_OTA_RECEIVING:
        snprintf(buf, sizeof buf, "Installing %s: %u%%", st.version, st.total ? (unsigned)(st.done * 100ull / st.total) : 0);
        break;
    case T5_OTA_VERIFYING: snprintf(buf, sizeof buf, "Checking %s", st.version); break;
    case T5_OTA_READY: snprintf(buf, sizeof buf, "%s installed: restart to run it", st.version); break;
    case T5_OTA_FAILED: snprintf(buf, sizeof buf, "Last update failed: %s", st.error); break;
    default:
        if (t5_ota_on_trial()) snprintf(buf, sizeof buf, "Just updated: confirmed after a minute of running");
        else snprintf(buf, sizeof buf, "Up to date with what's installed");
    }
    md_row_set_secondary(p->state, buf);
    if (st.state == T5_OTA_READY && p->shown != -2) {
        p->shown = -2;
        lv_obj_t *row = sa_button_row(p->list);
        lv_obj_t *b = md_button_create(row, "Restart now", MD_BUTTON_CONTAINED);
        lv_obj_add_event_cb(b, restart_now, LV_EVENT_CLICKED, NULL);
        return;
    }
    if (p->shown != -1) return;
    p->shown = 0;
    static char paths[8][128];
    int n = t5_ota_find_on_sd(paths, 8);
    for (int i = 0; i < n; i++) {
        lv_obj_t *r = md_row_create(p->list, MD_ICON_SD_STORAGE, paths[i], "Tap to install");
        md_row_set_clickable(r, upd_file_clicked, paths[i]);
    }
    if (!n) sa_note(p->list, "No update on the SD card. Put a .t5os file in its top folder or in /update.");
}

static void page_updates(void)
{
    lv_obj_t *body = sa_push(&s_nav, "Updates");
    upd_page_t *p = calloc(1, sizeof *p);
    if (!p) return;
    p->shown = -1;
    const esp_app_desc_t *d = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    char buf[160];
    snprintf(buf, sizeof buf, "Tab5 OS %s, %s %s, slot %s", d->version, d->date, d->time, run ? run->label : "?");
    md_row_create(body, MD_ICON_SYSTEM_UPDATE, "Installed", buf);
    p->state = md_row_create(body, MD_ICON_VERIFIED_USER, "Status", " ");
    t5_net_status_t n;
    t5_net_status(&n);
    snprintf(buf, sizeof buf, "Running %s%s%s", n.c6_version[0] ? n.c6_version : "unknown",
             n.c6_image[0] ? ", staged " : "", n.c6_image);
    md_row_create(body, MD_ICON_WIFI, "Radio (ESP32-C6)", buf);
    md_section_create(body, "From the SD card");
    p->list = lv_obj_create(body);
    lv_obj_remove_style_all(p->list);
    lv_obj_set_size(p->list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(p->list, LV_FLEX_FLOW_COLUMN);
    sa_note(body, "Updates are checked before they are used: signed by a trusted key (or pushed in developer mode), "
                  "and built for this chip. A paired computer can also push one with `t5 ota`.");
    md_section_create(body, "Repair");
    lv_obj_t *r = md_row_create(body, MD_ICON_HEALING, "Restart in safe mode", "Installed apps stay off");
    md_row_set_clickable(r, safemode_clicked, NULL);
    r = md_row_create(body, MD_ICON_RESTORE, "Restart into recovery", "Reinstall, clear settings, save logs");
    md_row_set_clickable(r, recovery_clicked, NULL);
    sa_set_refresh(&s_nav, upd_refresh, p, free);
}

/* ---- Developer ---------------------------------------------------------------------------------- */
typedef struct {
    lv_obj_t *sw, *addr, *stats, *clients;
    int shown;
} dev_page_t;

static void devmode_toggle(bool on)
{
    sa_report(t5_dev_set_enabled(on), "change developer mode");
}

static void dev_forget_answer(int b, void *ud)
{
    (void)ud;
    if (b == 0) {
        t5_err_t e = t5_dev_forget_all();
        if (e == T5_OK) md_snackbar_show("Paired computers forgotten", NULL, NULL, NULL);
        sa_report(e, "forget the computers");
    }
}

static void forget_clicked(lv_event_t *e)
{
    (void)e;
    md_dialog_show("Forget paired computers?", "They will need to pair again to push or launch apps.", "Forget",
                   "Cancel", dev_forget_answer, NULL);
}

static void dev_refresh(sa_nav_t *nav, void *arg)
{
    (void)nav;
    dev_page_t *p = arg;
    t5_dev_status_t st;
    t5_dev_status(&st);
    set_switch(p->sw, st.enabled);
    t5_net_status_t n;
    t5_net_status(&n);
    char buf[160];
    if (!st.enabled) snprintf(buf, sizeof buf, "Off: nothing listens on the network");
    else if (n.state != T5_NET_CONNECTED) snprintf(buf, sizeof buf, "Waiting for Wi-Fi (port %u)", st.port);
    else snprintf(buf, sizeof buf, "t5 pair %u.%u.%u.%u", n.ip[0], n.ip[1], n.ip[2], n.ip[3]);
    md_row_set_secondary(p->addr, buf);
    snprintf(buf, sizeof buf, "%u requests, %u refused since boot", (unsigned)st.requests, (unsigned)st.denied);
    md_row_set_secondary(p->stats, buf);
    if (st.clients == p->shown) return;
    p->shown = st.clients;
    lv_obj_clean(p->clients);
    char names[T5_DEV_MAX_CLIENTS][T5_DEV_NAME_LEN];
    int k = t5_dev_clients(names, T5_DEV_MAX_CLIENTS);
    for (int i = 0; i < k; i++) md_row_create(p->clients, MD_ICON_DEVELOPER_BOARD, names[i], NULL);
    if (!k) sa_note(p->clients, "No computer is paired. Run `t5 pair <address>` and type the code shown here.");
}

static void page_developer(void)
{
    lv_obj_t *body = sa_push(&s_nav, "Developer mode");
    dev_page_t *p = calloc(1, sizeof *p);
    if (!p) return;
    p->shown = -1;
    p->sw = switch_row(body, MD_ICON_CODE, "Developer mode", "A paired computer can push, launch and watch apps",
                       t5_dev_enabled(), devmode_toggle);
    p->addr = md_row_create(body, MD_ICON_WIFI, "Address", " ");
    p->stats = md_row_create(body, MD_ICON_SECURITY, "Activity", " ");
    md_section_create(body, "Paired computers");
    p->clients = lv_obj_create(body);
    lv_obj_remove_style_all(p->clients);
    lv_obj_set_size(p->clients, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(p->clients, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *row = sa_button_row(body);
    lv_obj_t *b = md_button_create(row, "Forget all", MD_BUTTON_TEXT);
    lv_obj_add_event_cb(b, forget_clicked, LV_EVENT_CLICKED, NULL);
    char note[224];
    snprintf(note, sizeof note,
             "Developer mode listens on port %d only while it is on, and every request but pairing needs a "
             "token from a computer you paired here. The USB-C console is separate and always on.", T5_DEV_PORT);
    sa_note(body, note);
    switch_row(body, MD_ICON_DASHBOARD, "Frame counters", "Frame rate and render time on screen", false, stats_toggle);
    sa_set_refresh(&s_nav, dev_refresh, p, free);
}

/* ---- About -------------------------------------------------------------------------------------- */
static void page_licences(lv_event_t *e)
{
    (void)e;
    lv_obj_t *body = sa_push(&s_nav, "Licences");
#define STR_(x) #x
#define STR(x) STR_(x)
    static const struct { const char *what, *lic; } l[] = {
        { "Tab5 OS", "Apache License 2.0" },
        { "Roboto (fonts)", "Apache License 2.0, Google" },
        { "Material Icons", "Apache License 2.0, Google" },
        { "LVGL " STR(LVGL_VERSION_MAJOR) "." STR(LVGL_VERSION_MINOR), "MIT" },
        { "ESP-IDF and Espressif components", "Apache License 2.0, Espressif" },
        { "FreeRTOS", "MIT" },
        { "LittleFS", "BSD-3-Clause" },
        { "TinyUSB", "MIT" },
        { "NimBLE (Apache Mynewt)", "Apache License 2.0" },
        { "lwIP", "BSD-3-Clause" },
        { "mbedTLS", "Apache License 2.0" },
    };
    for (size_t i = 0; i < sizeof l / sizeof l[0]; i++) md_row_create(body, NULL, l[i].what, l[i].lic);
    sa_note(body, "The full licence texts are in the source tree (LICENSE and firmware/components/md/assets/LICENSES). "
                  "Material-inspired: Tab5 OS follows the Material Design 2 guidelines and is not a Google product.");
}

static void page_about(void)
{
    lv_obj_t *body = sa_push(&s_nav, "About");
    char buf[160];
    const esp_app_desc_t *d = esp_app_get_description();
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    md_row_create(body, MD_ICON_DEVELOPER_BOARD, "M5Stack Tab5", "ESP32-P4 with an ESP32-C6 radio");
    snprintf(buf, sizeof buf, "%s (API level %d), ESP-IDF %s", d->version, T5_API_LEVEL, d->idf_ver);
    md_row_create(body, MD_ICON_INFO, "Tab5 OS", buf);
    snprintf(buf, sizeof buf, "ESP32-P4 v%d.%d, %d cores · %u MB PSRAM · %u MB flash", ci.revision / 100,
             ci.revision % 100, ci.cores, (unsigned)(esp_psram_get_size() >> 20), (unsigned)(flash >> 20));
    md_row_create(body, MD_ICON_MEMORY, "Chip", buf);
    snprintf(buf, sizeof buf, "%s, %d x %d at %d px per dp", t5_bsp_panel_name(t5_bsp_panel()->panel), T5_PANEL_NATIVE_W,
             T5_PANEL_NATIVE_H, MD_PX_PER_DP);
    md_row_create(body, MD_ICON_IMAGE, "Display", buf);
    lv_obj_t *r = md_row_create(body, MD_ICON_INFO, "Licences", "Open-source components in this image");
    md_row_set_clickable(r, page_licences, NULL);
}

/* ---- root ----------------------------------------------------------------------------------------- */
typedef struct {
    const char *key, *icon, *title, *sub;
    void (*open)(void);
} section_t;

static const section_t s_sections[] = {
    { T5_SETTINGS_PAGE_WIFI, MD_ICON_WIFI, "Wi-Fi", "Networks and connection", page_wifi },
    { T5_SETTINGS_PAGE_BLE, MD_ICON_BLUETOOTH, "Bluetooth", "Bluetooth LE devices", page_ble },
    { T5_SETTINGS_PAGE_DISPLAY, MD_ICON_BRIGHTNESS_6, "Display", "Brightness, rotation, accent, timeout", page_display },
    { T5_SETTINGS_PAGE_SOUND, MD_ICON_VOLUME_UP, "Sound", "Volume, system sounds, microphones", page_sound },
    { T5_SETTINGS_PAGE_POWER, MD_ICON_BATTERY_FULL, "Power", "Battery saver, charging", page_power },
    { T5_SETTINGS_PAGE_TIME, MD_ICON_ACCESS_TIME, "Date and time", "Time zone, network time", page_time },
    { T5_SETTINGS_PAGE_USB, MD_ICON_USB, "USB", "USB-A devices, USB-C mode", page_usb },
    { T5_SETTINGS_PAGE_UPDATES, MD_ICON_SYSTEM_UPDATE, "Updates", "System updates, safe mode, recovery", page_updates },
    { T5_SETTINGS_PAGE_DEVELOPER, MD_ICON_CODE, "Developer mode", "Push and run apps from a computer", page_developer },
    { T5_SETTINGS_PAGE_ABOUT, MD_ICON_INFO, "About", "Device, versions, licences", page_about },
};

static void section_clicked(lv_event_t *e)
{
    const section_t *s = lv_event_get_user_data(e);
    s->open();
}

static void on_create(t5_app_t *app)
{
    sa_nav_init(&s_nav, app);
    lv_obj_t *body = sa_push(&s_nav, "Settings");
    for (size_t i = 0; i < sizeof s_sections / sizeof s_sections[0]; i++) {
        const section_t *s = &s_sections[i];
        lv_obj_t *r = md_row_create(body, s->icon, s->title, s->sub);
        md_row_set_clickable(r, section_clicked, (void *)s);
    }
    const char *arg = t5_app_launch_arg(app);
    for (size_t i = 0; arg[0] && i < sizeof s_sections / sizeof s_sections[0]; i++) {
        if (!strcmp(arg, s_sections[i].key)) s_sections[i].open();
    }
}

static void on_destroy(t5_app_t *app)
{
    (void)app;
    sa_nav_destroy(&s_nav);
}

static const t5_app_desc_t s_desc = { .api = T5_API_LEVEL, .on_create = on_create, .on_destroy = on_destroy };

const t5_builtin_app_t sysapp_settings = {
    .id = T5_APP_ID_SETTINGS,
    .name = "Settings",
    .desc = &s_desc,
    .perms = T5_PERM_NET | T5_PERM_BLE | T5_PERM_USB | T5_PERM_STORAGE | T5_PERM_SYSTEM,
    .orientation = T5_ORIENT_ANY,
    .icon = MD_ICON_SETTINGS,
};
