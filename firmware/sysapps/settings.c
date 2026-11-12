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
#include "t5_shell/standby.h"
#include "t5/storage.h"
#include "t5/sys.h"
#include "t5/system.h"
#include "sysapp_system.h"
#include "t5/usb.h"
#include "t5_dev/dev.h"
#include "t5_net/ble_sys.h"
#include "t5_ota/ota.h"
#include "t5_ota_net/update.h"
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

/* A column container for rows a refresh rebuilds. */
static lv_obj_t *row_list(lv_obj_t *body)
{
    lv_obj_t *l = lv_obj_create(body);
    lv_obj_remove_style_all(l);
    lv_obj_set_size(l, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
    return l;
}

static void set_hidden(lv_obj_t *o, bool hidden)
{
    lv_obj_set_hidden(o, hidden);
}

/* Received signal strength in words: dBm means nothing to most people,
 * and the bands are what a person acts on (move closer, or not). */
static const char *signal_words(int rssi)
{
    return rssi >= -60 ? "strong signal" : rssi >= -70 ? "good signal" : rssi >= -80 ? "fair signal" : "weak signal";
}

/* ---- Wi-Fi ----------------------------------------------------------------------------- */
typedef struct {
    lv_obj_t *sw, *status, *list, *saved, *saved_hdr;
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
    else snprintf(msg, sizeof msg, "Couldn't connect to %s", p->ssid);
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
    md_dialog_show(title, "The Tab5 won't join this network by itself any more, and you'll need the password to join "
                          "it again.", "Forget", "Cancel", forget_answer, f);
}

static void scan_clicked(lv_event_t *e)
{
    (void)e;
    t5_net_status_t st;
    t5_net_status(&st);
    if (!st.enabled) md_snackbar_show("Turn on Wi-Fi first", NULL, NULL, NULL);
    else if (t5_net_scan() == T5_OK) md_snackbar_show("Looking for networks", NULL, NULL, NULL);
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
    if (!st.enabled) snprintf(buf, sizeof buf, "Off. Turn on Wi-Fi to see networks nearby.");
    else if (st.state == T5_NET_CONNECTED)
        snprintf(buf, sizeof buf, "Connected to %s, %s. IP address %u.%u.%u.%u", st.ssid, signal_words(st.rssi),
                 st.ip[0], st.ip[1], st.ip[2], st.ip[3]);
    else if (st.state == T5_NET_CONNECTING) snprintf(buf, sizeof buf, "Connecting...");
    else if (st.state == T5_NET_FAILED)
        snprintf(buf, sizeof buf, "Couldn't connect. Check the password and try again.");
    else if (st.state == T5_NET_STARTING) snprintf(buf, sizeof buf, "Turning on...");
    else snprintf(buf, sizeof buf, "Not connected. Choose a network below.");
    sa_set_text(w->status, buf);

    w->naps = t5_net_scan_results(w->aps, T5_NET_MAX_SCAN);
    char ss[8][T5_NET_SSID_LEN];
    int ns = t5_net_saved_list(ss, 8);
    uint32_t h = ap_hash(w->aps, w->naps) ^ ((uint32_t)ns << 24) ^ ((uint32_t)st.enabled << 30) ^
                 ((uint32_t)(st.state == T5_NET_CONNECTED) << 31);
    for (const char *p = st.ssid; st.state == T5_NET_CONNECTED && *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
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
        /* One row per network name: a mesh or dual-band router answers from
         * several access points. Results come strongest first. */
        bool dup = false;
        for (int j = 0; j < i && !dup; j++) dup = !strcmp(w->aps[j].ssid, a->ssid);
        if (dup) continue;
        snprintf(buf, sizeof buf, "%s, %s", a->saved ? "Saved" : a->auth ? "Secured" : "Open", signal_words(a->rssi));
        lv_obj_t *r = md_row_create(w->list, a->auth ? MD_ICON_WIFI_LOCK : MD_ICON_WIFI, a->ssid, buf);
        md_row_set_clickable(r, ap_clicked, (void *)(intptr_t)i);
        if (st.state == T5_NET_CONNECTED && !strcmp(st.ssid, a->ssid)) md_row_trailing_text(r, "Connected");
    }
    if (!st.enabled) sa_note(w->list, "Wi-Fi is off.");
    else if (!w->naps) sa_note(w->list, "No networks found yet. Tap the refresh button to look again.");
    lv_obj_clean(w->saved);
    for (int i = 0; i < ns; i++) {
        lv_obj_t *r = md_row_create(w->saved, MD_ICON_WIFI, ss[i], "Tap to forget");
        md_row_set_clickable(r, saved_clicked, (void *)(intptr_t)i);
    }
    set_hidden(w->saved_hdr, ns == 0);
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
    md_section_create(body, "Networks nearby");
    w->list = row_list(body);
    w->saved_hdr = md_section_create(body, "Saved networks");
    w->saved = row_list(body);
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
    if (t5_ble_sys_scan(5000) == T5_OK) md_snackbar_show("Looking for devices for 5 seconds", NULL, NULL, NULL);
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
    md_snackbar_show(err == T5_OK ? "Connecting..." : "Couldn't connect to this device", NULL, NULL, NULL);
}

static void ble_refresh(sa_nav_t *nav, void *arg)
{
    (void)nav;
    ble_page_t *b = arg;
    bool on = t5_ble_ready();
    bool conn = on && t5_ble_sys_connected();
    set_switch(b->sw, on);
    sa_set_text(b->status, !on   ? "Off. Turn on Bluetooth to find devices nearby."
                           : conn ? "Connected. Tap the device to disconnect."
                                  : "Not connected. Tap a device to connect.");
    b->n = t5_ble_sys_scan_results(b->devs, T5_BLE_MAX_SCAN);
    int key = b->n * 2 + on;
    if (key == b->shown) return;
    b->shown = key;
    lv_obj_clean(b->list);
    char addr[18], sub[48];
    for (int i = 0; i < b->n; i++) {
        t5_ble_format_addr(b->devs[i].addr, addr);
        snprintf(sub, sizeof sub, "%s, %s", addr, signal_words(b->devs[i].rssi));
        lv_obj_t *r =
            md_row_create(b->list, MD_ICON_BLUETOOTH, b->devs[i].name[0] ? b->devs[i].name : "Unnamed device", sub);
        md_row_set_clickable(r, ble_dev_clicked, (void *)(intptr_t)i);
    }
    if (!on) sa_note(b->list, "Bluetooth is off.");
    else if (!b->n) sa_note(b->list, "No devices found yet. Tap the refresh button to look for 5 seconds.");
}

static void page_ble(void)
{
    ble_page_t *b = calloc(1, sizeof *b);
    if (!b) return;
    lv_obj_t *body = sa_push(&s_nav, "Bluetooth");
    md_appbar_add_action(sa_bar(&s_nav), MD_ICON_REFRESH, ble_scan_clicked, NULL);
    b->sw = switch_row(body, MD_ICON_BLUETOOTH, "Use Bluetooth", "Bluetooth LE sensors and accessories", t5_ble_ready(),
                       ble_toggle);
    b->status = sa_note(body, "");
    md_section_create(body, "Devices nearby");
    b->list = row_list(body);
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

static void standby_toggle(bool on)
{
    t5_shell_set_standby_enabled(on);
}

static void stats_toggle(bool on)
{
    t5_ui_show_stats(on);
}

/* Accent swatches: a row of colour circles, the chosen one carrying a
 * check. Names on ten chips made the choice a reading exercise; the colour
 * itself is the label, and the section header names the current one. */
static lv_obj_t *s_accent_hdr;   /* valid while the Display page is open */

static void accent_header(void)
{
    char buf[48];
    snprintf(buf, sizeof buf, "Accent colour: %s", md_accent(md_get_accent())->name);
    lv_label_set_text(s_accent_hdr, buf);
}

static void accent_clicked(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_current_target_obj(e);
    t5_shell_accent_set((md_accent_t)(intptr_t)lv_event_get_user_data(e));
    lv_obj_t *row = lv_obj_get_parent(sw);
    for (uint32_t i = 0; i < lv_obj_get_child_count(row); i++) {
        lv_obj_t *c = lv_obj_get_child(row, (int32_t)i);
        set_hidden(lv_obj_get_child(c, 0), c != sw);
    }
    if (s_accent_hdr) {
        lv_obj_set_style_text_color(s_accent_hdr, md_color(md_scheme()->c.primary), 0);
        accent_header();
    }
}

static void accent_hdr_deleted(lv_event_t *e)
{
    (void)e;
    s_accent_hdr = NULL;
}

static void accent_swatches(lv_obj_t *body)
{
    s_accent_hdr = md_section_create(body, "");
    lv_obj_add_event_cb(s_accent_hdr, accent_hdr_deleted, LV_EVENT_DELETE, NULL);
    accent_header();
    lv_obj_t *row = lv_obj_create(body);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(row, T5_DP(12), 0);
    lv_obj_set_style_pad_ver(row, T5_DP(4), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_scrollable(row, false);
    for (int i = 0; i < MD_ACCENT_COUNT; i++) {
        const md_accent_def_t *a = md_accent((md_accent_t)i);
        lv_obj_t *s = lv_obj_create(row);
        lv_obj_remove_style_all(s);
        /* 40 dp of colour in a 48 dp touch target. */
        lv_obj_set_size(s, T5_DP(40), T5_DP(40));
        lv_obj_set_style_margin_all(s, T5_DP(4), 0);
        lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s, md_color(md_is_dark() ? a->dark : a->light), 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        lv_obj_set_style_clip_corner(s, true, 0);
        lv_obj_set_ext_click_area(s, T5_DP(4));
        lv_obj_set_clickable(s, true);
        lv_obj_set_scrollable(s, false);
        md_ripple_attach(s);
        lv_obj_t *chk = md_icon_create(s, MD_ICON_CHECK, MD_ICON_DP);
        lv_obj_set_style_text_color(chk, md_color(md_is_dark() ? a->on_dark : a->on_light), 0);
        lv_obj_center(chk);
        set_hidden(chk, md_get_accent() != (md_accent_t)i);
        lv_obj_add_event_cb(s, accent_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void rot_clicked(lv_event_t *e)
{
    t5_ui_set_rotation_lock(true);
    t5_ui_set_rotation((t5_rotation_t)(intptr_t)lv_event_get_user_data(e));
}

/* The timeout note says what the chosen profile does, so it follows the
 * chips (it used to keep describing the profile the page opened with). */
static lv_obj_t *s_timeout_note;   /* valid while the Display page is open */

static void timeout_note_deleted(lv_event_t *e)
{
    (void)e;
    s_timeout_note = NULL;
}

static void timeout_text(void)
{
    if (!s_timeout_note) return;
    t5_power_timeouts_t to;
    t5_power_get_timeouts(&to);
    char buf[160], a[24], b[24];
    sa_fmt_duration(a, sizeof a, to.screen_off_ms / 1000);
    sa_fmt_duration(b, sizeof b, to.soft_off_ms / 1000);
    if (t5_power_profile() == T5_PROFILE_MOUNTED)
        snprintf(buf, sizeof buf, "The screen turns off after %s without a touch. A mounted Tab5 never powers off by "
                                  "itself.", a);
    else
        snprintf(buf, sizeof buf, "The screen turns off after %s without a touch, and the Tab5 powers off after %s "
                                  "asleep.", a, b);
    sa_set_text(s_timeout_note, buf);
}

static void profile_clicked(lv_event_t *e)
{
    sa_report(t5_power_set_profile((t5_power_profile_t)(intptr_t)lv_event_get_user_data(e)), "change the power profile");
    timeout_text();
}

static void page_display(void)
{
    lv_obj_t *body = sa_push(&s_nav, "Display");
    md_section_create(body, "Brightness");
    slider_row(body, MD_ICON_BRIGHTNESS_6, T5_DISPLAY_BRIGHTNESS_MIN, 100, t5_display_brightness(), bright_changed);
    md_section_create(body, "Rotation");
    switch_row(body, MD_ICON_SCREEN_ROTATION, "Auto-rotate", "When off, the screen keeps its orientation, even after a "
               "restart", t5_ui_follows_board(), autorot_toggle);
    lv_obj_t *c = chips(body);
    static const char *names[] = { "Portrait", "Landscape", "Flipped portrait", "Flipped landscape" };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *ch = md_chip_create(c, names[i], NULL, MD_CHIP_CHOICE);
        if ((int)t5_ui_rotation() == i) md_chip_set_selected(ch, true);
        lv_obj_add_event_cb(ch, rot_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    accent_swatches(body);
    md_section_create(body, "Sleep");
    c = chips(body);
    static const char *prof[] = { "Handheld", "Mounted" };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *ch = md_chip_create(c, prof[i], NULL, MD_CHIP_CHOICE);
        if ((int)t5_power_profile() == i) md_chip_set_selected(ch, true);
        lv_obj_add_event_cb(ch, profile_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    s_timeout_note = sa_note(body, "");
    lv_obj_add_event_cb(s_timeout_note, timeout_note_deleted, LV_EVENT_DELETE, NULL);
    timeout_text();
    switch_row(body, MD_ICON_NIGHTS_STAY, "Standby while charging",
               "Shows a clock, music and app cards instead of turning the screen off", t5_shell_standby_enabled(),
               standby_toggle);
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
    if (!st.ready) snprintf(buf, sizeof buf, "No sound: the audio chips didn't respond at startup. Restart the Tab5 to "
                                             "try again.");
    else snprintf(buf, sizeof buf, "%s through the %s.", st.out_running ? "Playing" : "Sound plays",
                  st.headphones ? "headphones" : "speaker");
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
    sa_note(body, "Input level for both microphones. Apps ask before they listen, and the status bar shows a "
                  "microphone while one is.");
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
    if (!b.ready || b.percent < 0) snprintf(buf, sizeof buf, "Measuring the battery...");
    else snprintf(buf, sizeof buf, "Battery at %d%%, %s.", b.percent,
                  !b.usb                          ? "running on battery"
                  : b.charge == T5_CHG_CHARGING   ? "charging"
                  : b.charge == T5_CHG_FULL        ? "full"
                  : b.charge == T5_CHG_PAUSED_LIMIT ? "charging paused at the 80% limit"
                  : b.charge == T5_CHG_PAUSED_TEMP  ? "charging paused while the battery is warm"
                                                    : "on USB power");
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
    p->saver = switch_row(body, MD_ICON_BATTERY_SAVER, "Battery saver",
                          "Lower frame rate, simpler animations, shorter timeouts",
                          t5_power_low_power(), saver_toggle);
    p->limit = switch_row(body, MD_ICON_BATTERY_CHARGING_FULL, "Stop charging at 80%",
                          "Makes the battery last longer if the Tab5 stays plugged in", t5_battery_charge_limit(),
                          limit_toggle);
    p->fast = switch_row(body, MD_ICON_BOLT, "Fast charging", "Higher charging current", t5_battery_fast_charge(),
                         fast_toggle);
    lv_obj_t *r =
        md_row_create(body, MD_ICON_OPEN_IN_NEW, "Battery details", "Live power draw, charge history, use per app");
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
        /* The POSIX rule string is for machines: the name alone, with a
         * trailing check on the current zone. */
        lv_obj_t *r = md_row_create(body, NULL, name, NULL);
        md_row_set_clickable(r, zone_clicked, (void *)(intptr_t)i);
        if (!strcmp(posix, t5_time_tz())) {
            lv_obj_t *chk = md_row_trailing_text(r, MD_ICON_CHECK);
            lv_obj_set_style_text_font(chk, md_icon_font(MD_ICON_DP), 0);
            lv_obj_set_style_text_color(chk, md_color(md_scheme()->c.primary), 0);
            lv_obj_set_style_text_opa(chk, LV_OPA_COVER, 0);
        }
    }
}

static void time_set_clicked(lv_event_t *e)
{
    time_page_t *p = lv_event_get_user_data(e);
    int Y, M, D, h, m;
    if (sscanf(md_textfield_text(p->date), "%d-%d-%d", &Y, &M, &D) != 3 ||
        sscanf(md_textfield_text(p->time), "%d:%d", &h, &m) != 2 || M < 1 || M > 12 || D < 1 || D > 31 || h < 0 ||
        h > 23 || m < 0 || m > 59 || Y < 2024) {
        md_textfield_set_helper(p->date, "Use the form 2026-01-31 and 14:30", true);
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
    if (ok) {
        char day[16], month[16];
        strftime(day, sizeof day, "%A", &tm);
        strftime(month, sizeof month, "%B", &tm);
        snprintf(buf, sizeof buf, "%02d:%02d, %s %d %s %d", tm.tm_hour, tm.tm_min, day, tm.tm_mday, month,
                 tm.tm_year + 1900);
    }
    else snprintf(buf, sizeof buf, "The clock isn't set");
    sa_set_text(p->now, buf);
    static const char *src[] = { "Connect to Wi-Fi or set the time below.",
                                 "Kept by the Tab5's clock while it was off. It sets itself over Wi-Fi.",
                                 "Set by hand.", "Set over the network. It stays right while the Tab5 is off." };
    t5_time_source_t s = t5_time_source();
    snprintf(buf, sizeof buf, "%s", src[s <= T5_TIME_SRC_NTP ? s : 0]);
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
    lv_obj_set_style_pad_hor(p->now, T5_DP(16), 0);
    lv_obj_set_style_pad_top(p->now, T5_DP(16), 0);
    p->src = sa_note(body, "");
    lv_obj_t *r = md_row_create(body, MD_ICON_LANGUAGE, "Time zone", t5_time_tz_name());
    md_row_set_clickable(r, open_zones, NULL);
    p->zone = r;
    snprintf(p->tz, sizeof p->tz, "%s", t5_time_tz());
    r = md_row_create(body, MD_ICON_REFRESH, "Set from the network now", "Needs Wi-Fi");
    md_row_set_clickable(r, sync_clicked, NULL);
    md_section_create(body, "Set by hand");
    lv_obj_t *box = lv_obj_create(body);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(box, T5_DP(16), 0);
    lv_obj_set_style_pad_gap(box, T5_DP(16), 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    p->date = md_textfield_create(box, "Date (year-month-day)", false);
    lv_obj_set_flex_grow(p->date, 1);
    p->time = md_textfield_create(box, "Time (24-hour)", false);
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
    if (e != T5_OK) md_snackbar_show("Couldn't share the SD card. Check that one is inserted.", NULL, NULL, NULL);
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
        md_dialog_show("Take the SD card back?",
                       "Eject the drive on the computer first. Taking it back while the computer is writing to it "
                       "can lose those files.",
                       "Take it back", "Cancel", take_back_answer, NULL);
        return;
    }
    md_dialog_show("Share the SD card with a computer?",
                   "The computer on the USB-C port opens the card as a drive until you eject it there. While it's "
                   "shared, apps can't use the card, and sound and the USB console stop. Internal storage is never "
                   "shared.",
                   "Share", "Cancel", msc_answer, NULL);
}

static void usb_refresh(sa_nav_t *nav, void *arg)
{
    (void)nav;
    usb_page_t *p = arg;
    set_switch(p->sw, t5_usb_host_enabled());
    int m = (int)t5_usbc_mode();
    if (m != p->mode_shown) {
        p->mode_shown = m;
        md_row_set_secondary(p->mode, m == T5_USBC_STORAGE ? "Shared now. Eject it on the computer, then tap here"
                                                           : "Open the card as a drive on a computer over USB-C");
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
        snprintf(sub, sizeof sub, "%s, %s speed, ID %04x:%04x", t5_usb_class_name(d[i].cls),
                 d[i].speed == 2 ? "high" : d[i].speed == 1 ? "full" : "low", d[i].vid, d[i].pid);
        md_row_create(p->list, icons[d[i].cls < T5_USB_CLASS_COUNT ? d[i].cls : 0],
                      d[i].product[0] ? d[i].product : "USB device", sub);
    }
    if (!n) sa_note(p->list, t5_usb_host_enabled() ? "Nothing plugged in." : "Turn on the USB-A port to use devices.");
}

static void page_usb(void)
{
    usb_page_t *p = calloc(1, sizeof *p);
    if (!p) return;
    p->shown = p->mode_shown = -1;
    lv_obj_t *body = sa_push(&s_nav, "USB");
    md_section_create(body, "USB-A");
    p->sw = switch_row(body, MD_ICON_USB, "USB-A port", "Keyboards, mice, drives, cameras and MIDI",
                       t5_usb_host_enabled(), usb_toggle);
    p->list = row_list(body);
    md_section_create(body, "USB-C");
    p->mode = md_row_create(body, MD_ICON_SD_STORAGE, "Share the SD card", " ");
    md_row_set_clickable(p->mode, msc_clicked, NULL);
    sa_set_refresh(&s_nav, usb_refresh, p, free);
}

/* ---- Updates (plan 9.1-9.3) ---------------------------------------------------------------------- */
typedef struct {
    lv_obj_t *state, *list, *online, *check, *install;
    int shown;
    char online_sig[160];
} upd_page_t;

static char s_upd_path[128];

static void upd_task(void *arg)
{
    (void)arg;
    char err[128] = "";
    t5_err_t e = t5_ota_install_file(s_upd_path, T5_OTA_FROM_SETTINGS, t5_dev_enabled(), err, sizeof err);
    char msg[160];
    if (e == T5_OK) snprintf(msg, sizeof msg, "Update installed. Restart to use it.");
    else snprintf(msg, sizeof msg, "Couldn't install the update: %s", err);
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
    md_dialog_show("Install this update?",
                   "It installs alongside the current system, which keeps running until you restart. If the new "
                   "one doesn't start properly, the current one comes back by itself.",
                   "Install", "Cancel", upd_answer, NULL);
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
                   "Recovery can reinstall the system from the SD card, reset settings and save logs to the card.",
                   "Restart", "Cancel", restart_answer, (void *)1);
}

static void safemode_clicked(lv_event_t *e)
{
    (void)e;
    md_dialog_show("Restart in safe mode?", "Apps you installed stay off until the next restart.", "Restart", "Cancel",
                   restart_answer, NULL);
}

static void restart_now(lv_event_t *e)
{
    (void)e;
    t5_power_restart();
}

static void upd_check_clicked(lv_event_t *e)
{
    (void)e;
    if (t5_update_check_now() != T5_OK) md_snackbar_show("A check or an install is already running", NULL, NULL, NULL);
}

static void upd_online_answer(int b, void *ud)
{
    (void)ud;
    if (b == 0 && t5_update_install() != T5_OK) md_snackbar_show("Couldn't start the update", NULL, NULL, NULL);
}

static void upd_install_clicked(lv_event_t *e)
{
    (void)e;
    t5_update_status_t u;
    t5_update_status(&u);
    char title[64];
    snprintf(title, sizeof title, "Install Tab5 OS %s?", u.version);
    md_dialog_show(title,
                   "It downloads and installs alongside the current system, which keeps running until you restart. "
                   "It goes in only if it's signed by a trusted key. If the new one doesn't start properly, the "
                   "current one comes back by itself.",
                   "Install", "Cancel", upd_online_answer, NULL);
}

/* The Over Wi-Fi section: what the last check found, and its buttons. */
static void upd_online_refresh(upd_page_t *p)
{
    t5_update_status_t u;
    t5_update_status(&u);
    char buf[160];
    int64_t ago_min = u.checked_us ? (t5_time_us() - u.checked_us) / 60000000 : -1;
    char when[40] = "";
    if (ago_min >= 120) snprintf(when, sizeof when, ", checked %d hours ago", (int)(ago_min / 60));
    else if (ago_min >= 1) snprintf(when, sizeof when, ", checked %d min ago", (int)ago_min);
    else if (ago_min == 0) snprintf(when, sizeof when, ", checked just now");
    switch (u.state) {
    case T5_UPDATE_CHECKING: snprintf(buf, sizeof buf, "Checking for updates"); break;
    case T5_UPDATE_CURRENT: snprintf(buf, sizeof buf, "Up to date%s", when); break;
    case T5_UPDATE_AVAILABLE:
        if (u.error[0]) snprintf(buf, sizeof buf, "Tab5 OS %s is available. %s", u.version, u.error);
        else snprintf(buf, sizeof buf, "Tab5 OS %s is available", u.version);
        break;
    case T5_UPDATE_INSTALLING: snprintf(buf, sizeof buf, "Downloading and installing Tab5 OS %s", u.version); break;
    case T5_UPDATE_FAILED: snprintf(buf, sizeof buf, "%s", u.error); break;
    default: snprintf(buf, sizeof buf, "Checks once a day while Wi-Fi is connected"); break;
    }
    if (!strcmp(buf, p->online_sig)) return;
    snprintf(p->online_sig, sizeof p->online_sig, "%s", buf);
    md_row_set_secondary(p->online, buf);
    bool busy = u.state == T5_UPDATE_CHECKING || u.state == T5_UPDATE_INSTALLING;
    lv_obj_set_hidden(p->install, u.state != T5_UPDATE_AVAILABLE);
    lv_obj_set_hidden(p->check, busy);
}

static void upd_refresh(sa_nav_t *nav, void *arg)
{
    (void)nav;
    upd_page_t *p = arg;
    upd_online_refresh(p);
    t5_ota_status_t st;
    t5_ota_status(&st);
    char buf[160];
    switch (st.state) {
    case T5_OTA_RECEIVING:
        snprintf(buf, sizeof buf, "Installing %s: %u%%", st.version, st.total ? (unsigned)(st.done * 100ull / st.total) : 0);
        break;
    case T5_OTA_VERIFYING: snprintf(buf, sizeof buf, "Checking %s", st.version); break;
    case T5_OTA_READY: snprintf(buf, sizeof buf, "%s is installed. Restart to use it", st.version); break;
    case T5_OTA_FAILED: snprintf(buf, sizeof buf, "The last update failed: %s", st.error); break;
    default:
        if (t5_ota_on_trial()) snprintf(buf, sizeof buf, "Updated. It's kept once it has run for a minute");
        else snprintf(buf, sizeof buf, "No update in progress");
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
    if (!n) sa_note(p->list, "No update found. Copy a .t5os file to the card's top folder or its update folder.");
}

static void page_updates(void)
{
    lv_obj_t *body = sa_push(&s_nav, "Updates");
    upd_page_t *p = calloc(1, sizeof *p);
    if (!p) return;
    p->shown = -1;
    const esp_app_desc_t *d = esp_app_get_description();
    char buf[160];
    /* __DATE__ is "Nov  9 2026": shown as "9 Nov 2026". */
    char mon[4] = "", day[4] = "", year[8] = "";
    if (sscanf(d->date, "%3s %3s %7s", mon, day, year) == 3)
        snprintf(buf, sizeof buf, "Tab5 OS %s, built %s %s %s", d->version, day, mon, year);
    else
        snprintf(buf, sizeof buf, "Tab5 OS %s, built %s", d->version, d->date);
    md_row_create(body, MD_ICON_SYSTEM_UPDATE, "System", buf);
    p->state = md_row_create(body, MD_ICON_VERIFIED_USER, "Status", " ");
    t5_net_status_t n;
    t5_net_status(&n);
    snprintf(buf, sizeof buf, "Version %s%s%s", n.c6_version[0] ? n.c6_version : "unknown",
             n.c6_image[0] ? ", update ready: " : "", n.c6_image);
    md_row_create(body, MD_ICON_WIFI, "Wi-Fi and Bluetooth radio", buf);
    md_section_create(body, "Over Wi-Fi");
    p->online = md_row_create(body, MD_ICON_CLOUD, "Tab5 OS releases", " ");
    lv_obj_t *brow = sa_button_row(body);
    p->check = md_button_create(brow, "Check now", MD_BUTTON_TEXT);
    lv_obj_add_event_cb(p->check, upd_check_clicked, LV_EVENT_CLICKED, NULL);
    p->install = md_button_create(brow, "Install", MD_BUTTON_CONTAINED);
    lv_obj_add_event_cb(p->install, upd_install_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_set_hidden(p->install, true);
    md_section_create(body, "From the SD card");
    p->list = row_list(body);
    sa_note(body, "An update installs only if it's built for this Tab5, signed by a trusted key and not older than "
                  "this system, or in developer mode.");
    md_section_create(body, "Repair");
    lv_obj_t *r = md_row_create(body, MD_ICON_HEALING, "Restart in safe mode", "Apps you installed stay off");
    md_row_set_clickable(r, safemode_clicked, NULL);
    r = md_row_create(body, MD_ICON_RESTORE, "Restart into recovery",
                      "Reinstall the system, reset settings, save logs");
    md_row_set_clickable(r, recovery_clicked, NULL);
    sa_set_refresh(&s_nav, upd_refresh, p, free);
}

/* ---- Developer ---------------------------------------------------------------------------------- */
typedef struct {
    lv_obj_t *sw, *addr, *stats, *clients, *forget;
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
    md_dialog_show("Forget all paired computers?", "They'll need to pair again to send or launch apps.", "Forget",
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
    if (!st.enabled) snprintf(buf, sizeof buf, "Turn on developer mode first");
    else if (n.state != T5_NET_CONNECTED) snprintf(buf, sizeof buf, "Connect to Wi-Fi first");
    else snprintf(buf, sizeof buf, "On the computer, run:\nt5 pair %u.%u.%u.%u", n.ip[0], n.ip[1], n.ip[2], n.ip[3]);
    md_row_set_secondary(p->addr, buf);
    snprintf(buf, sizeof buf, "%u requests since startup, %u refused", (unsigned)st.requests, (unsigned)st.denied);
    md_row_set_secondary(p->stats, buf);
    if (st.clients == p->shown) return;
    p->shown = st.clients;
    lv_obj_clean(p->clients);
    char names[T5_DEV_MAX_CLIENTS][T5_DEV_NAME_LEN];
    int k = t5_dev_clients(names, T5_DEV_MAX_CLIENTS);
    for (int i = 0; i < k; i++) md_row_create(p->clients, MD_ICON_DEVELOPER_BOARD, names[i], NULL);
    if (!k) sa_note(p->clients, "None yet.");
    set_hidden(p->forget, k == 0);
}

static void page_developer(void)
{
    lv_obj_t *body = sa_push(&s_nav, "Developer mode");
    dev_page_t *p = calloc(1, sizeof *p);
    if (!p) return;
    p->shown = -1;
    p->sw = switch_row(body, MD_ICON_CODE, "Developer mode", "A paired computer can push, launch and watch apps",
                       t5_dev_enabled(), devmode_toggle);
    p->addr = md_row_create(body, MD_ICON_WIFI, "Pair a computer", " ");
    p->stats = md_row_create(body, MD_ICON_SECURITY, "Activity", " ");
    md_section_create(body, "Paired computers");
    p->clients = row_list(body);
    p->forget = sa_button_row(body);
    lv_obj_t *b = md_button_create(p->forget, "Forget all", MD_BUTTON_TEXT);
    lv_obj_add_event_cb(b, forget_clicked, LV_EVENT_CLICKED, NULL);
    md_section_create(body, "Tools");
    switch_row(body, MD_ICON_DASHBOARD, "Frame counters", "Frame rate and render time on screen", false, stats_toggle);
    char note[224];
    snprintf(note, sizeof note,
             "Developer mode listens on port %d only while it's on. Apart from pairing, every request needs a "
             "computer paired here. The USB-C console is separate and always on.", T5_DEV_PORT);
    sa_note(body, note);
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
    sa_note(body, "The full licence texts are in the source code, in LICENSE and firmware/components/md/assets/"
                  "LICENSES. Tab5 OS follows the Material Design 2 guidelines and is not a Google product.");
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
    snprintf(buf, sizeof buf, "Version %s, app API level %d, ESP-IDF %s", d->version, T5_API_LEVEL, d->idf_ver);
    md_row_create(body, MD_ICON_INFO, "Tab5 OS", buf);
    snprintf(buf, sizeof buf, "ESP32-P4 v%d.%d, %d cores, %u MB PSRAM, %u MB flash", ci.revision / 100,
             ci.revision % 100, ci.cores, (unsigned)(esp_psram_get_size() >> 20), (unsigned)(flash >> 20));
    md_row_create(body, MD_ICON_MEMORY, "Chip", buf);
    snprintf(buf, sizeof buf, "%s, %d by %d pixels", t5_bsp_panel_name(t5_bsp_panel()->panel), T5_PANEL_NATIVE_W,
             T5_PANEL_NATIVE_H);
    md_row_create(body, MD_ICON_IMAGE, "Display", buf);
    lv_obj_t *r = md_row_create(body, MD_ICON_CODE, "Licences", "Open-source software in Tab5 OS");
    md_row_set_clickable(r, page_licences, NULL);
}

/* ---- root ----------------------------------------------------------------------------------------- */
typedef struct {
    const char *key, *icon, *title, *sub;
    void (*open)(void);
} section_t;

static const section_t s_sections[] = {
    { T5_SETTINGS_PAGE_WIFI, MD_ICON_WIFI, "Wi-Fi", "Networks and connection", page_wifi },
    { T5_SETTINGS_PAGE_BLE, MD_ICON_BLUETOOTH, "Bluetooth", "Bluetooth LE devices nearby", page_ble },
    { T5_SETTINGS_PAGE_DISPLAY, MD_ICON_BRIGHTNESS_6, "Display", "Brightness, rotation, colour, sleep", page_display },
    { T5_SETTINGS_PAGE_SOUND, MD_ICON_VOLUME_UP, "Sound", "Volume, system sounds, microphones", page_sound },
    { T5_SETTINGS_PAGE_POWER, MD_ICON_BATTERY_FULL, "Power", "Battery saver, charging", page_power },
    { T5_SETTINGS_PAGE_TIME, MD_ICON_ACCESS_TIME, "Date and time", "Time zone, setting the clock", page_time },
    { T5_SETTINGS_PAGE_USB, MD_ICON_USB, "USB", "USB-A devices, sharing the SD card", page_usb },
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
