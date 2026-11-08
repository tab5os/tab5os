/* SPDX-License-Identifier: Apache-2.0
 * Quick settings (plan 6.4): a sheet that slides down from the top over a
 * scrim. Tiles for Wi-Fi, Bluetooth, auto-rotate, battery saver and mute;
 * sliders for brightness and volume; the time, the battery and a shortcut
 * to Settings. A long press on a tile opens its Settings page. Close by
 * tapping outside, swiping up, or the back gesture.
 */
#include <stdio.h>
#include <string.h>
#include "shell_priv.h"

#define QS_MAX_W_DP 560

typedef enum { TILE_WIFI, TILE_BLE, TILE_ROTATE, TILE_SAVER, TILE_MUTE, TILE_COUNT } tile_id_t;

static const struct {
    const char *icon_on, *icon_off, *label, *page;
} s_tile_def[TILE_COUNT] = {
    [TILE_WIFI] = { MD_ICON_WIFI, MD_ICON_WIFI_OFF, "Wi-Fi", "wifi" },
    [TILE_BLE] = { MD_ICON_BLUETOOTH, MD_ICON_BLUETOOTH_DISABLED, "Bluetooth", "ble" },
    [TILE_ROTATE] = { MD_ICON_SCREEN_ROTATION, MD_ICON_SCREEN_LOCK_ROTATION, "Auto-rotate", "display" },
    [TILE_SAVER] = { MD_ICON_BATTERY_SAVER, MD_ICON_BATTERY_SAVER, "Battery saver", "power" },
    [TILE_MUTE] = { MD_ICON_VOLUME_OFF, MD_ICON_VOLUME_UP, "Mute", "sound" },
};

static struct {
    lv_obj_t *catcher, *panel;
    lv_obj_t *time, *date, *battery;
    lv_obj_t *tile[TILE_COUNT], *tile_icon[TILE_COUNT], *tile_label[TILE_COUNT];
    lv_obj_t *bright, *volume;
    bool on[TILE_COUNT];
    bool closing;
    lv_timer_t *timer;
} s;

bool qs_is_open(void)
{
    return s.catcher && !s.closing;
}

static void set_text(lv_obj_t *l, const char *t)
{
    if (strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

static void tile_paint(int i, bool on)
{
    const md_scheme_t *sc = md_scheme();
    s.on[i] = on;
    lv_obj_t *c = s.tile_icon[i];
    lv_obj_t *circle = lv_obj_get_parent(c);
    lv_obj_set_style_bg_color(circle, md_color(on ? sc->c.primary : sc->c.on_surface), 0);
    lv_obj_set_style_bg_opa(circle, on ? LV_OPA_COVER : LV_OPA_10 + 2, 0);
    lv_obj_set_style_text_color(circle, md_color(on ? sc->c.on_primary : sc->c.on_surface), 0);
    set_text(c, on ? s_tile_def[i].icon_on : s_tile_def[i].icon_off);
}

static void update(void)
{
    if (!s.catcher) return;
    shell_status_t st;
    shell_sys_status(&st);
    struct tm tm;
    char buf[64];
    static const char *days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *months[] = { "January", "February", "March", "April", "May", "June", "July",
                                    "August", "September", "October", "November", "December" };
    if (shell_sys_time(&tm)) {
        snprintf(buf, sizeof buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
        set_text(s.time, buf);
        snprintf(buf, sizeof buf, "%s, %d %s", days[tm.tm_wday % 7], tm.tm_mday, months[tm.tm_mon % 12]);
        set_text(s.date, buf);
    } else {
        set_text(s.time, "--:--");
        set_text(s.date, "Time not set");
    }
    if (st.battery_pct < 0) {
        snprintf(buf, sizeof buf, "Battery: measuring");
    } else if (st.charging) {
        snprintf(buf, sizeof buf, "%d%% · charging", st.battery_pct);
    } else if (st.usb_power) {
        snprintf(buf, sizeof buf, "%d%% · on USB power", st.battery_pct);
    } else if (st.minutes_left > 0) {
        snprintf(buf, sizeof buf, "%d%% · about %d h %d min left", st.battery_pct, (int)(st.minutes_left / 60),
                 (int)(st.minutes_left % 60));
    } else {
        snprintf(buf, sizeof buf, "%d%%", st.battery_pct);
    }
    set_text(s.battery, buf);
    tile_paint(TILE_WIFI, st.wifi_on);
    set_text(s.tile_label[TILE_WIFI], st.wifi_connected && st.ssid[0] ? st.ssid : "Wi-Fi");
    tile_paint(TILE_BLE, st.ble_on);
    tile_paint(TILE_ROTATE, !st.rotation_locked);
    tile_paint(TILE_SAVER, st.low_power);
    tile_paint(TILE_MUTE, st.muted);
    if (!lv_obj_has_state(s.bright, LV_STATE_PRESSED)) lv_slider_set_value(s.bright, st.brightness, LV_ANIM_OFF);
    if (!lv_obj_has_state(s.volume, LV_STATE_PRESSED)) lv_slider_set_value(s.volume, st.volume, LV_ANIM_OFF);
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    update();
}

static void set_y_anim(void *o, int32_t v)
{
    lv_obj_set_y(o, v);
}

static void set_bg_opa_anim(void *o, int32_t v)
{
    lv_obj_set_style_bg_opa(o, (lv_opa_t)v, 0);
}

static void closed(lv_anim_t *a)
{
    lv_obj_delete(a->var);
}

static void catcher_deleted(lv_event_t *e)
{
    (void)e;
    if (s.timer) lv_timer_delete(s.timer);
    memset(&s, 0, sizeof s);
}

void qs_close(void)
{
    if (!s.catcher || s.closing) return;
    s.closing = true;
    int32_t h = lv_obj_get_height(s.panel);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s.panel);
    lv_anim_set_exec_cb(&a, set_y_anim);
    lv_anim_set_values(&a, lv_obj_get_y(s.panel), -h);
    lv_anim_set_duration(&a, MD_DUR_DRAWER_OUT_MS);
    md_anim_set_curve(&a, MD_CURVE_SHARP);
    lv_anim_start(&a);
    lv_anim_init(&a);
    lv_anim_set_var(&a, s.catcher);
    lv_anim_set_exec_cb(&a, set_bg_opa_anim);
    lv_anim_set_values(&a, lv_obj_get_style_bg_opa(s.catcher, 0), LV_OPA_TRANSP);
    lv_anim_set_duration(&a, MD_DUR_DRAWER_OUT_MS);
    lv_anim_set_completed_cb(&a, closed);
    lv_anim_start(&a);
}

static void outside(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) == s.catcher) qs_close();
}

static void panel_gesture(lv_event_t *e)
{
    (void)e;
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_TOP) qs_close();
}

static void tile_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    bool on = !s.on[i];
    switch (i) {
    case TILE_WIFI: shell_sys_set_wifi(on); break;
    case TILE_BLE: shell_sys_set_ble(on); break;
    case TILE_ROTATE: shell_sys_set_rotation_lock(!on); break;
    case TILE_SAVER: shell_sys_set_low_power(on); break;
    case TILE_MUTE: shell_sys_set_muted(on); break;
    default: break;
    }
    tile_paint(i, on);           /* optimistic; the next update corrects it */
    shell_sys_click();
}

static void tile_long(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    lv_indev_wait_release(lv_indev_active());
    qs_close();
    shell_sys_launch(SHELL_APP_SETTINGS, s_tile_def[i].page);
}

static void settings_clicked(lv_event_t *e)
{
    (void)e;
    qs_close();
    shell_sys_launch(SHELL_APP_SETTINGS, NULL);
}

static void bright_changed(lv_event_t *e)
{
    shell_sys_set_brightness(lv_slider_get_value(lv_event_get_target_obj(e)));
}

static void volume_changed(lv_event_t *e)
{
    shell_sys_set_volume(lv_slider_get_value(lv_event_get_target_obj(e)));
}

static void volume_released(lv_event_t *e)
{
    (void)e;
    shell_sys_click();            /* hear the new level */
}

static lv_obj_t *slider_row(lv_obj_t *parent, const char *icon, lv_obj_t **slider, lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), T5_DP(48));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, T5_DP(16), 0);
    lv_obj_set_style_pad_hor(row, T5_DP(8), 0);
    lv_obj_set_scrollable(row, false);
    lv_obj_t *i = md_icon_create(row, icon, MD_ICON_DP);
    lv_obj_set_style_text_opa(i, md_scheme()->on_surface.medium, 0);
    *slider = md_slider_create(row, 0, 100, 50);
    lv_obj_set_flex_grow(*slider, 1);
    lv_obj_add_event_cb(*slider, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return row;
}

void qs_open(void)
{
    if (s.catcher) return;
    keyboard_hide();
    void *saved = shell_sys_enter();
    const md_scheme_t *sc = md_scheme();
    s.catcher = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s.catcher);
    lv_obj_set_size(s.catcher, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s.catcher, md_color(sc->c.scrim), 0);
    lv_obj_set_clickable(s.catcher, true);
    lv_obj_add_event_cb(s.catcher, outside, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s.catcher, catcher_deleted, LV_EVENT_DELETE, NULL);

    lv_obj_t *p = lv_obj_create(s.catcher);
    s.panel = p;
    md_set_elevation(p, MD_ELEV_MENU, false);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_width(p, LV_PCT(100));
    lv_obj_set_style_max_width(p, T5_DP(QS_MAX_W_DP), 0);
    lv_obj_set_height(p, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(p, T5_DP(16), 0);
    lv_obj_set_style_pad_top(p, T5_DP(12), 0);
    lv_obj_set_style_pad_bottom(p, T5_DP(8), 0);
    lv_obj_set_style_pad_row(p, T5_DP(8), 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(p, false);
    lv_obj_align(p, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_add_event_cb(p, panel_gesture, LV_EVENT_GESTURE, NULL);

    /* Header: time and date, battery, Settings. */
    lv_obj_t *hdr = lv_obj_create(p);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, T5_DP(12), 0);
    lv_obj_set_scrollable(hdr, false);
    lv_obj_set_clickable(hdr, false);
    s.time = md_label_create(hdr, "", MD_TYPE_HEADLINE5);
    lv_obj_t *col = lv_obj_create(hdr);
    lv_obj_remove_style_all(col);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_clickable(col, false);
    s.date = md_label_create(col, "", MD_TYPE_BODY2);
    s.battery = md_label_create(col, "", MD_TYPE_CAPTION);
    lv_obj_set_style_text_opa(s.battery, sc->on_surface.medium, 0);
    lv_obj_t *gear = md_appbar_add_action(hdr, MD_ICON_SETTINGS, settings_clicked, NULL);
    (void)gear;

    /* Tiles. */
    lv_obj_t *tiles = lv_obj_create(p);
    lv_obj_remove_style_all(tiles);
    lv_obj_set_size(tiles, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tiles, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(tiles, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollable(tiles, false);
    lv_obj_set_clickable(tiles, false);
    for (int i = 0; i < TILE_COUNT; i++) {
        lv_obj_t *t = lv_obj_create(tiles);
        s.tile[i] = t;
        lv_obj_remove_style_all(t);
        lv_obj_set_size(t, T5_DP(96), T5_DP(80));
        lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(t, T5_DP(6), 0);
        lv_obj_set_clickable(t, true);
        lv_obj_set_scrollable(t, false);
        lv_obj_t *circle = lv_obj_create(t);
        lv_obj_remove_style_all(circle);
        lv_obj_set_size(circle, T5_DP(48), T5_DP(48));
        lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_clickable(circle, false);
        s.tile_icon[i] = md_icon_create(circle, s_tile_def[i].icon_on, MD_ICON_DP);
        lv_obj_center(s.tile_icon[i]);
        s.tile_label[i] = md_label_create(t, s_tile_def[i].label, MD_TYPE_CAPTION);
        lv_obj_set_width(s.tile_label[i], LV_PCT(100));
        lv_obj_set_style_text_align(s.tile_label[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(s.tile_label[i], LV_LABEL_LONG_MODE_DOTS);
        md_ripple_attach(circle);
        lv_obj_add_event_cb(t, tile_clicked, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(t, tile_long, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
    }

    slider_row(p, MD_ICON_BRIGHTNESS_6, &s.bright, bright_changed);
    lv_obj_t *vrow = slider_row(p, MD_ICON_VOLUME_UP, &s.volume, volume_changed);
    (void)vrow;
    lv_obj_add_event_cb(s.volume, volume_released, LV_EVENT_RELEASED, NULL);

    /* A drag handle hint at the bottom. */
    lv_obj_t *hrow = lv_obj_create(p);
    lv_obj_remove_style_all(hrow);
    lv_obj_set_size(hrow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hrow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_clickable(hrow, false);
    lv_obj_t *handle = lv_obj_create(hrow);
    lv_obj_remove_style_all(handle);
    lv_obj_set_size(handle, T5_DP(32), T5_DP(4));
    lv_obj_set_style_radius(handle, T5_DP(2), 0);
    lv_obj_set_style_bg_color(handle, md_color(sc->c.on_surface), 0);
    lv_obj_set_style_bg_opa(handle, sc->on_surface.disabled, 0);
    lv_obj_set_clickable(handle, false);

    update();
    s.timer = lv_timer_create(timer_cb, 1000, NULL);
    shell_sys_leave(saved);

    lv_obj_update_layout(s.catcher);
    int32_t h = lv_obj_get_height(p);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, p);
    lv_anim_set_exec_cb(&a, set_y_anim);
    lv_anim_set_values(&a, -h, 0);
    lv_anim_set_duration(&a, MD_DUR_DRAWER_IN_MS);
    md_anim_set_curve(&a, MD_CURVE_DECELERATION);
    lv_anim_start(&a);
    lv_anim_init(&a);
    lv_anim_set_var(&a, s.catcher);
    lv_anim_set_exec_cb(&a, set_bg_opa_anim);
    lv_anim_set_values(&a, LV_OPA_TRANSP, 82);
    lv_anim_set_duration(&a, MD_DUR_DRAWER_IN_MS);
    lv_anim_start(&a);
}

void qs_reset(void)
{
    if (s.timer) lv_timer_delete(s.timer);
    memset(&s, 0, sizeof s);
}
