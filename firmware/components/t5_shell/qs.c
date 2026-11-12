/* SPDX-License-Identifier: Apache-2.0
 * Quick settings (plan 6.4): a sheet that slides down from the top over a
 * scrim. Tiles for Wi-Fi, Bluetooth, auto-rotate, battery saver and mute;
 * sliders for brightness and volume; the ongoing activities (apps' and the
 * system's: what each is doing, with progress); the time, the battery and
 * a shortcut to Settings. A long press on a tile opens its Settings page. Close by
 * tapping outside, swiping up, or the back gesture.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shell_priv.h"

#define QS_MAX_W_DP 560
#define QS_RADIUS_DP 16          /* the sheet's bottom corners */
#define SCRIM_RGB 0x000000       /* Material's scrim, in both themes */
#define SCRIM_OPA 82

typedef enum { TILE_WIFI, TILE_BLE, TILE_ROTATE, TILE_SAVER, TILE_MUTE, TILE_COUNT } tile_id_t;

static const struct {
    const char *icon_on, *icon_off, *label, *page;
} s_tile_def[TILE_COUNT] = {
    [TILE_WIFI] = { T5UI_ICON_WIFI, T5UI_ICON_WIFI_OFF, "Wi-Fi", T5_SETTINGS_PAGE_WIFI },
    [TILE_BLE] = { T5UI_ICON_BLUETOOTH, T5UI_ICON_BLUETOOTH_DISABLED, "Bluetooth", T5_SETTINGS_PAGE_BLE },
    [TILE_ROTATE] = { T5UI_ICON_SCREEN_ROTATION, T5UI_ICON_SCREEN_LOCK_ROTATION, "Auto-rotate",
                      T5_SETTINGS_PAGE_DISPLAY },
    [TILE_SAVER] = { T5UI_ICON_BATTERY_SAVER, T5UI_ICON_BATTERY_SAVER, "Battery saver", T5_SETTINGS_PAGE_POWER },
    [TILE_MUTE] = { T5UI_ICON_VOLUME_OFF, T5UI_ICON_VOLUME_UP, "Mute", T5_SETTINGS_PAGE_SOUND },
};

static struct {
    t5ui_obj_t *catcher, *panel;
    t5ui_obj_t *time, *date, *battery;
    t5ui_obj_t *tile[TILE_COUNT], *tile_icon[TILE_COUNT], *tile_label[TILE_COUNT];
    t5ui_obj_t *bright, *volume;
    t5ui_obj_t *acts;             /* the activity rows */
    uint32_t acts_sig;            /* what they show, to rebuild only on a change */
    bool on[TILE_COUNT];
    bool closing;
    t5ui_timer_t *timer;
} s;

bool qs_is_open(void)
{
    return s.catcher && !s.closing;
}

static void tile_paint(int i, bool on)
{
    s.on[i] = on;
    t5ui_obj_t *c = s.tile_icon[i];
    t5ui_obj_t *circle = t5ui_parent(c);
    t5ui_set_color(circle, T5UI_P_BG_COLOR, t5ui_theme_color(on ? T5UI_COLOR_PRIMARY : T5UI_COLOR_ON_SURFACE));
    t5ui_set_int(circle, T5UI_P_BG_OPA, on ? T5UI_OPA_COVER : 25 + 2);   /* 10% and a bit */
    t5ui_set_color(circle, T5UI_P_TEXT_COLOR, t5ui_theme_color(on ? T5UI_COLOR_ON_PRIMARY : T5UI_COLOR_ON_SURFACE));
    shell_set_text(c, on ? s_tile_def[i].icon_on : s_tile_def[i].icon_off);
}

static bool pressed(t5ui_obj_t *o)
{
    return t5ui_get_int(o, T5UI_P_STATE, 0) & T5UI_STATE_PRESSED;
}

static void update_activities(void);

static void update(void)
{
    if (!s.catcher) return;
    shell_status_t st;
    shell_sys_status(&st);
    shell_show_time(s.time, s.date);
    char buf[64];
    if (st.battery_pct < 0) {
        snprintf(buf, sizeof buf, "Measuring battery");
    } else if (st.charging) {
        snprintf(buf, sizeof buf, "%d%%, charging", st.battery_pct);
    } else if (st.usb_power) {
        snprintf(buf, sizeof buf, "%d%%, on USB power", st.battery_pct);
    } else if (st.minutes_left >= 60) {
        snprintf(buf, sizeof buf, "%d%%, about %d h %d min left", st.battery_pct, (int)(st.minutes_left / 60),
                 (int)(st.minutes_left % 60));
    } else if (st.minutes_left > 0) {
        snprintf(buf, sizeof buf, "%d%%, about %d min left", st.battery_pct, (int)st.minutes_left);
    } else {
        snprintf(buf, sizeof buf, "%d%%", st.battery_pct);
    }
    shell_set_text(s.battery, buf);
    tile_paint(TILE_WIFI, st.wifi_on);
    shell_set_text(s.tile_label[TILE_WIFI], st.wifi_connected && st.ssid[0] ? st.ssid : "Wi-Fi");
    tile_paint(TILE_BLE, st.ble_on);
    tile_paint(TILE_ROTATE, !st.rotation_locked);
    tile_paint(TILE_SAVER, st.low_power);
    tile_paint(TILE_MUTE, st.muted);
    if (!pressed(s.bright)) t5ui_set_int(s.bright, T5UI_P_VALUE, st.brightness);
    if (!pressed(s.volume)) t5ui_set_int(s.volume, T5UI_P_VALUE, st.volume);
    update_activities();
}

static uint32_t act_sig(const shell_activity_t *a, int n)
{
    uint32_t h = 2166136261u;     /* FNV-1a over what the rows show */
    for (int i = 0; i < n; i++) {
        const uint8_t *p = (const uint8_t *)&a[i];
        for (size_t j = 0; j < sizeof a[i]; j++) h = (h ^ p[j]) * 16777619u;
    }
    return h ^ (uint32_t)n;
}

static void stop_clicked(t5ui_event_t *e)
{
    shell_sys_stop((uint8_t)(uintptr_t)t5ui_event_user(e));
}

static void act_row(t5ui_obj_t *parent, const shell_activity_t *a)
{
    t5ui_obj_t *row = shell_box(parent);
    shell_size(row, T5UI_PCT(100), T5UI_CONTENT);
    shell_flex(row, T5UI_ROW, T5UI_PLACE_START, T5UI_PLACE_CENTER, T5UI_PLACE_CENTER);
    t5ui_set_int(row, T5UI_P_PAD_COLUMN, t5ui_dp(12));
    t5ui_set_int(row, T5UI_P_CLICKABLE, false);
    t5ui_obj_t *ic = shell_icon(row, a->icon, SHELL_ICON_DP);
    t5ui_set_int(ic, T5UI_P_TEXT_OPA, t5ui_theme_emphasis(T5UI_EMPHASIS_MEDIUM));
    t5ui_obj_t *col = shell_box(row);
    t5ui_set_int(col, T5UI_P_HEIGHT, T5UI_CONTENT);
    t5ui_set_int(col, T5UI_P_GROW, 1);
    t5ui_set_int(col, T5UI_P_FLOW, T5UI_COLUMN);
    t5ui_set_int(col, T5UI_P_PAD_ROW, t5ui_dp(2));
    t5ui_set_int(col, T5UI_P_CLICKABLE, false);
    /* Who on the first line, what on the second; the system's own
     * activities have no who and take one line. */
    if (a->who[0]) {
        t5ui_obj_t *w = shell_text(col, a->who, T5UI_TEXT_BODY2);
        t5ui_set_int(w, T5UI_P_WIDTH, T5UI_PCT(100));
        t5ui_set_int(w, T5UI_P_LONG_MODE, T5UI_LONG_DOTS);
    }
    t5ui_obj_t *l = shell_text(col, a->text, a->who[0] ? T5UI_TEXT_CAPTION : T5UI_TEXT_BODY2);
    if (a->who[0]) t5ui_set_int(l, T5UI_P_TEXT_OPA, t5ui_theme_emphasis(T5UI_EMPHASIS_MEDIUM));
    t5ui_set_int(l, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(l, T5UI_P_LONG_MODE, T5UI_LONG_DOTS);
    if (a->percent >= 0) {
        t5ui_obj_t *bar = t5ui_create(col, T5UI_BAR);
        shell_size(bar, T5UI_PCT(100), t5ui_dp(4));
        t5ui_set_int(bar, T5UI_P_VALUE, a->percent);
        t5ui_set_color(bar, T5UI_P_BG_COLOR, t5ui_theme_color(T5UI_COLOR_ON_SURFACE));
        t5ui_set_int(bar, T5UI_P_BG_OPA, 51);   /* 20% */
        t5ui_set_color_at(bar, T5UI_P_BG_COLOR, T5UI_AT(T5UI_PART_INDICATOR, T5UI_STATE_DEFAULT),
                          t5ui_theme_color(T5UI_COLOR_PRIMARY));
    }
    /* A port or radio an app holds: the person's way to take it back. */
    if (a->stop) {
        t5ui_obj_t *b = t5ui_create(row, T5UI_MD_BUTTON_TEXT);
        t5ui_set_str(b, T5UI_P_TEXT, "Stop");
        t5ui_on(b, T5UI_EV_CLICK, stop_clicked, (void *)(uintptr_t)a->stop);
    }
}

static void update_activities(void)
{
    shell_activity_t *a = calloc(SHELL_ACTIVITY_MAX, sizeof *a);
    int n = a ? shell_sys_activities(a, SHELL_ACTIVITY_MAX) : 0;
    uint32_t sig = act_sig(a, n);
    if (sig != s.acts_sig) {
        s.acts_sig = sig;
        void *saved = shell_sys_enter();
        t5ui_clean(s.acts);
        for (int i = 0; i < n; i++) act_row(s.acts, &a[i]);
        shell_sys_leave(saved);
        t5ui_set_int(s.acts, T5UI_P_HIDDEN, n == 0);
    }
    free(a);
}

static void timer_cb(t5ui_timer_t *t, void *user)
{
    (void)t;
    (void)user;
    update();
}

/* The scrim's fade out ended: the sheet is gone. */
static void catcher_anim_end(t5ui_event_t *e)
{
    if (s.closing && t5ui_event_index(e) == T5UI_P_BG_OPA) {
        t5ui_cmd(t5ui_event_obj(e), T5UI_CMD_DELETE_LATER, 0, 0, 0, NULL);
    }
}

static void catcher_deleted(t5ui_event_t *e)
{
    (void)e;
    if (s.timer) t5ui_timer_delete(s.timer);
    memset(&s, 0, sizeof s);
}

void qs_close(void)
{
    if (!s.catcher || s.closing) return;
    s.closing = true;
    t5ui_rect_t r;
    t5ui_coords(s.panel, &r);
    int32_t h = r.y2 - r.y1 + 1;
    t5ui_animate(s.panel, T5UI_P_Y, t5ui_get_int(s.panel, T5UI_P_Y, 0), -h, SHELL_DUR_DRAWER_OUT_MS, 0,
                 SHELL_EASE_SHARP);
    t5ui_animate(s.catcher, T5UI_P_BG_OPA, t5ui_get_int(s.catcher, T5UI_P_BG_OPA, 0), T5UI_OPA_TRANSP,
                 SHELL_DUR_DRAWER_OUT_MS, 0, T5UI_EASE_LINEAR);
}

static void outside(t5ui_event_t *e)
{
    if (t5ui_event_target(e) == s.catcher) qs_close();
}

static void panel_gesture(t5ui_event_t *e)
{
    if (t5ui_event_dir(e) == T5UI_SWIPE_UP) qs_close();
}

static void tile_clicked(t5ui_event_t *e)
{
    int i = (int)(intptr_t)t5ui_event_user(e);
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

static void tile_long(t5ui_event_t *e)
{
    int i = (int)(intptr_t)t5ui_event_user(e);
    t5ui_cmd(t5ui_event_obj(e), T5UI_CMD_IGNORE_PRESS, 0, 0, 0, NULL);
    qs_close();
    shell_sys_launch(T5_APP_ID_SETTINGS, s_tile_def[i].page);
}

static void settings_clicked(t5ui_event_t *e)
{
    (void)e;
    qs_close();
    shell_sys_launch(T5_APP_ID_SETTINGS, NULL);
}

static void bright_changed(t5ui_event_t *e)
{
    shell_sys_set_brightness(t5ui_get_int(t5ui_event_obj(e), T5UI_P_VALUE, 0));
}

static void volume_changed(t5ui_event_t *e)
{
    shell_sys_set_volume(t5ui_get_int(t5ui_event_obj(e), T5UI_P_VALUE, 0));
}

static void volume_released(t5ui_event_t *e)
{
    (void)e;
    shell_sys_click();            /* hear the new level */
}

/* A round 48 dp icon button, as an app bar's actions are. */
static t5ui_obj_t *icon_button(t5ui_obj_t *parent, const char *glyph, t5ui_event_cb_t cb)
{
    t5ui_obj_t *b = t5ui_create(parent, T5UI_MD_BUTTON_TEXT);
    shell_size(b, t5ui_dp(48), t5ui_dp(48));
    t5ui_set_int(b, T5UI_P_EXT_CLICK, 0);
    t5ui_set_int(b, T5UI_P_RADIUS, T5UI_RADIUS_CIRCLE);
    t5ui_set_int(b, T5UI_P_PAD_ALL, 0);
    t5ui_set_int(b, T5UI_P_MIN_WIDTH, 0);
    t5ui_set_color(b, T5UI_P_TEXT_COLOR, t5ui_theme_color(T5UI_COLOR_ON_SURFACE));
    t5ui_obj_t *text = t5ui_child(b, 0);   /* the button's own (empty) label */
    if (text) t5ui_set_int(text, T5UI_P_HIDDEN, true);
    t5ui_obj_t *i = shell_icon(b, glyph, SHELL_ICON_DP);
    t5ui_set_int(i, T5UI_P_ALIGN, T5UI_ALIGN_CENTER);
    t5ui_on(b, T5UI_EV_CLICK, cb, NULL);
    return b;
}

static void slider_row(t5ui_obj_t *parent, const char *icon, t5ui_obj_t **slider, t5ui_event_cb_t cb)
{
    t5ui_obj_t *row = shell_box(parent);
    shell_size(row, T5UI_PCT(100), t5ui_dp(48));
    shell_flex(row, T5UI_ROW, T5UI_PLACE_START, T5UI_PLACE_CENTER, T5UI_PLACE_CENTER);
    t5ui_set_int(row, T5UI_P_PAD_COLUMN, t5ui_dp(16));
    shell_pad_hor(row, t5ui_dp(8));
    t5ui_set_int(row, T5UI_P_SCROLLABLE, false);
    t5ui_obj_t *i = shell_icon(row, icon, SHELL_ICON_DP);
    t5ui_set_int(i, T5UI_P_TEXT_OPA, t5ui_theme_emphasis(T5UI_EMPHASIS_MEDIUM));
    *slider = t5ui_create(row, T5UI_MD_SLIDER);
    t5ui_set_int(*slider, T5UI_P_VALUE, 50);
    t5ui_set_int(*slider, T5UI_P_GROW, 1);
    t5ui_on(*slider, T5UI_EV_VALUE, cb, NULL);
}

void qs_open(void)
{
    if (s.catcher) return;
    keyboard_hide();
    void *saved = shell_sys_enter();
    s.catcher = shell_box(t5ui_sys_layer(T5UI_LAYER_TOP));
    shell_size(s.catcher, T5UI_PCT(100), T5UI_PCT(100));
    t5ui_set_color(s.catcher, T5UI_P_BG_COLOR, SCRIM_RGB);
    t5ui_set_int(s.catcher, T5UI_P_CLICKABLE, true);
    t5ui_on(s.catcher, T5UI_EV_CLICK, outside, NULL);
    t5ui_on(s.catcher, T5UI_EV_ANIM_END, catcher_anim_end, NULL);
    t5ui_on(s.catcher, T5UI_EV_DELETE, catcher_deleted, NULL);

    t5ui_obj_t *p = t5ui_create(s.catcher, T5UI_PANEL);
    s.panel = p;
    shell_elevate(p, SHELL_ELEV_MENU);
    /* Rounded at the bottom only: the sheet rests QS_RADIUS_DP above the
     * screen's top edge, with that much more padding, so its top corners
     * stay off screen. */
    t5ui_set_int(p, T5UI_P_RADIUS, t5ui_dp(QS_RADIUS_DP));
    t5ui_set_int(p, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(p, T5UI_P_MAX_WIDTH, t5ui_dp(QS_MAX_W_DP));
    t5ui_set_int(p, T5UI_P_HEIGHT, T5UI_CONTENT);
    shell_pad_hor(p, t5ui_dp(16));
    t5ui_set_int(p, T5UI_P_PAD_TOP, t5ui_dp(12 + QS_RADIUS_DP));
    t5ui_set_int(p, T5UI_P_PAD_BOTTOM, t5ui_dp(8));
    t5ui_set_int(p, T5UI_P_PAD_ROW, t5ui_dp(8));
    t5ui_set_int(p, T5UI_P_FLOW, T5UI_COLUMN);
    t5ui_set_int(p, T5UI_P_SCROLLABLE, false);
    t5ui_set_int(p, T5UI_P_ALIGN, T5UI_ALIGN_TOP_MID);
    t5ui_set_int(p, T5UI_P_X, 0);
    t5ui_set_int(p, T5UI_P_Y, -t5ui_dp(QS_RADIUS_DP));
    t5ui_on(p, T5UI_EV_GESTURE, panel_gesture, NULL);

    /* Header: time and date, battery, Settings. */
    t5ui_obj_t *hdr = shell_box(p);
    shell_size(hdr, T5UI_PCT(100), T5UI_CONTENT);
    shell_flex(hdr, T5UI_ROW, T5UI_PLACE_START, T5UI_PLACE_CENTER, T5UI_PLACE_CENTER);
    t5ui_set_int(hdr, T5UI_P_PAD_COLUMN, t5ui_dp(12));
    t5ui_set_int(hdr, T5UI_P_SCROLLABLE, false);
    t5ui_set_int(hdr, T5UI_P_CLICKABLE, false);
    s.time = shell_text(hdr, "", T5UI_TEXT_HEADLINE5);
    t5ui_obj_t *col = shell_box(hdr);
    t5ui_set_int(col, T5UI_P_HEIGHT, T5UI_CONTENT);
    t5ui_set_int(col, T5UI_P_GROW, 1);
    t5ui_set_int(col, T5UI_P_FLOW, T5UI_COLUMN);
    t5ui_set_int(col, T5UI_P_CLICKABLE, false);
    s.date = shell_text(col, "", T5UI_TEXT_BODY2);
    s.battery = shell_text(col, "", T5UI_TEXT_BODY2);
    t5ui_set_int(s.battery, T5UI_P_TEXT_OPA, t5ui_theme_emphasis(T5UI_EMPHASIS_MEDIUM));
    icon_button(hdr, T5UI_ICON_SETTINGS, settings_clicked);

    /* Tiles. */
    t5ui_obj_t *tiles = shell_box(p);
    shell_size(tiles, T5UI_PCT(100), T5UI_CONTENT);
    shell_flex(tiles, T5UI_ROW_WRAP, T5UI_PLACE_SPACE_EVENLY, T5UI_PLACE_START, T5UI_PLACE_START);
    t5ui_set_int(tiles, T5UI_P_SCROLLABLE, false);
    t5ui_set_int(tiles, T5UI_P_CLICKABLE, false);
    for (int i = 0; i < TILE_COUNT; i++) {
        t5ui_obj_t *t = shell_box(tiles);
        s.tile[i] = t;
        shell_size(t, t5ui_dp(96), t5ui_dp(80));
        shell_flex(t, T5UI_COLUMN, T5UI_PLACE_CENTER, T5UI_PLACE_CENTER, T5UI_PLACE_CENTER);
        t5ui_set_int(t, T5UI_P_PAD_ROW, t5ui_dp(6));
        t5ui_set_int(t, T5UI_P_CLICKABLE, true);
        t5ui_set_int(t, T5UI_P_SCROLLABLE, false);
        t5ui_obj_t *circle = shell_box(t);
        shell_size(circle, t5ui_dp(48), t5ui_dp(48));
        t5ui_set_int(circle, T5UI_P_RADIUS, T5UI_RADIUS_CIRCLE);
        t5ui_set_int(circle, T5UI_P_CLICKABLE, false);
        s.tile_icon[i] = shell_icon(circle, s_tile_def[i].icon_on, SHELL_ICON_DP);
        t5ui_set_int(s.tile_icon[i], T5UI_P_ALIGN, T5UI_ALIGN_CENTER);
        s.tile_label[i] = shell_text(t, s_tile_def[i].label, T5UI_TEXT_CAPTION);
        t5ui_set_int(s.tile_label[i], T5UI_P_LETTER_SPACE, 0);
        t5ui_set_int(s.tile_label[i], T5UI_P_WIDTH, T5UI_PCT(100));
        t5ui_set_int(s.tile_label[i], T5UI_P_TEXT_ALIGN, T5UI_TEXT_CENTER);
        t5ui_set_int(s.tile_label[i], T5UI_P_LONG_MODE, T5UI_LONG_DOTS);
        t5ui_set_int(circle, T5UI_P_RIPPLE, true);
        t5ui_on(t, T5UI_EV_SHORT_CLICK, tile_clicked, (void *)(intptr_t)i);
        t5ui_on(t, T5UI_EV_LONG_PRESS, tile_long, (void *)(intptr_t)i);
    }

    slider_row(p, T5UI_ICON_BRIGHTNESS_6, &s.bright, bright_changed);
    slider_row(p, T5UI_ICON_VOLUME_UP, &s.volume, volume_changed);
    t5ui_on(s.volume, T5UI_EV_RELEASE, volume_released, NULL);

    s.acts = shell_box(p);
    shell_size(s.acts, T5UI_PCT(100), T5UI_CONTENT);
    t5ui_set_int(s.acts, T5UI_P_FLOW, T5UI_COLUMN);
    t5ui_set_int(s.acts, T5UI_P_PAD_ROW, t5ui_dp(8));
    shell_pad_ver(s.acts, t5ui_dp(4));
    t5ui_set_int(s.acts, T5UI_P_SCROLLABLE, false);
    t5ui_set_int(s.acts, T5UI_P_CLICKABLE, false);
    s.acts_sig = 1;               /* not any real list's: the first update builds it */

    /* A drag handle hint at the bottom. */
    t5ui_obj_t *hrow = shell_box(p);
    shell_size(hrow, T5UI_PCT(100), T5UI_CONTENT);
    shell_flex(hrow, T5UI_ROW, T5UI_PLACE_CENTER, T5UI_PLACE_CENTER, T5UI_PLACE_CENTER);
    t5ui_set_int(hrow, T5UI_P_CLICKABLE, false);
    t5ui_obj_t *handle = shell_box(hrow);
    shell_size(handle, t5ui_dp(32), t5ui_dp(4));
    t5ui_set_int(handle, T5UI_P_RADIUS, t5ui_dp(2));
    t5ui_set_color(handle, T5UI_P_BG_COLOR, t5ui_theme_color(T5UI_COLOR_ON_SURFACE));
    t5ui_set_int(handle, T5UI_P_BG_OPA, t5ui_theme_emphasis(T5UI_EMPHASIS_DISABLED));
    t5ui_set_int(handle, T5UI_P_CLICKABLE, false);

    update();
    s.timer = t5ui_timer(1000, timer_cb, NULL);
    shell_sys_leave(saved);

    t5ui_rect_t r;
    t5ui_coords(p, &r);              /* lays it out */
    int32_t h = r.y2 - r.y1 + 1;
    t5ui_animate(p, T5UI_P_Y, -h, -t5ui_dp(QS_RADIUS_DP), SHELL_DUR_DRAWER_IN_MS, 0, T5UI_EASE_DECELERATE);
    t5ui_animate(s.catcher, T5UI_P_BG_OPA, T5UI_OPA_TRANSP, SCRIM_OPA, SHELL_DUR_DRAWER_IN_MS, 0, T5UI_EASE_LINEAR);
}

void qs_reset(void)
{
    if (s.timer) t5ui_timer_delete(s.timer);
    memset(&s, 0, sizeof s);
}
