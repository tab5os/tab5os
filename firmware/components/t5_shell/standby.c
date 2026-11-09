/* SPDX-License-Identifier: Apache-2.0
 * The standby screen (t5_shell/standby.h): a full-screen layer over the app
 * with a large clock and up to two cards, the music card (the active media
 * session, with previous / play-pause / next) and the newest app card.
 * Landscape puts the clock left and the cards right; portrait stacks them.
 * From 22:00 to 07:00 everything turns dim red.
 *
 * It lives on lv_layer_top() in front of everything, so the app under it
 * keeps running. A tap anywhere but on the music buttons leaves, and so
 * does a dialog or menu opening (check_front). Updated
 * once a second (standby_tick); a card is rebuilt only when what it shows
 * changes. UI task only.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "shell_priv.h"

#define STANDBY_PERCENT 20          /* backlight cap */
#define STANDBY_NIGHT_PERCENT 5
#define NIGHT_FROM_H 22
#define NIGHT_TO_H 7
#define CLOCK_SCALE 512             /* 2x the 68 px headline */
#define CARD_W_DP 280
#define NIGHT_RGB 0xb71c1c

static struct {
    lv_obj_t *root, *clock, *date, *cards, *music, *app;
    lv_obj_t *m_title, *m_artist, *m_who, *m_bar, *m_play;
    lv_obj_t *a_icon, *a_title, *a_value, *a_detail;
    char app_sig[200];
    bool night, have_music;
    int percent;
} s;

static bool is_night(void)
{
    struct tm tm;
    if (!shell_sys_time(&tm)) return false;
    return tm.tm_hour >= NIGHT_FROM_H || tm.tm_hour < NIGHT_TO_H;
}

static lv_color_t ink(void)
{
    return s.night ? lv_color_hex(NIGHT_RGB) : md_color(md_scheme()->c.on_surface);
}

static void leave(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) == lv_event_get_current_target_obj(e)) standby_hide();
}

/* Something opened in front of standby once its creator has sized it: a
 * dialog's scrim, a menu's or sheet's catcher (anything covering the whole
 * screen) wants an answer, so standby goes. A toast just shows on top. */
static void check_front(void *arg)
{
    (void)arg;
    if (!s.root) return;
    lv_obj_t *top = lv_layer_top();
    lv_obj_update_layout(top);
    int32_t w = lv_obj_get_width(top), h = lv_obj_get_height(top);
    for (int32_t i = lv_obj_get_index(s.root) + 1; i < (int32_t)lv_obj_get_child_count(top); i++) {
        lv_obj_t *o = lv_obj_get_child(top, i);
        if (lv_obj_is_hidden(o)) continue;
        if (lv_obj_get_width(o) >= w && lv_obj_get_height(o) >= h) {
            standby_hide();
            return;
        }
    }
}

static void child_created(lv_event_t *e)
{
    if (s.root && lv_event_get_param(e) != s.root) {
        lv_async_call_cancel(check_front, NULL);
        lv_async_call(check_front, NULL);
    }
}

static void media_clicked(lv_event_t *e)
{
    shell_sys_media_cmd((shell_media_cmd_t)(intptr_t)lv_event_get_user_data(e));
}

static lv_obj_t *box(lv_obj_t *parent, lv_flex_flow_t flow)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, flow);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    return o;
}

static lv_obj_t *card(lv_obj_t *parent)
{
    lv_obj_t *c = box(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(c, T5_DP(CARD_W_DP));
    lv_obj_set_style_pad_all(c, T5_DP(16), 0);
    lv_obj_set_style_pad_row(c, T5_DP(4), 0);
    lv_obj_set_style_radius(c, T5_DP(16), 0);
    /* Opaque, mixed here: a faint translucent fill over black comes out a
     * shade different where the 2D hardware blends it and where software
     * does (the rounded ends), a visible band. */
    lv_obj_set_style_bg_color(c, lv_color_mix(md_color(md_scheme()->c.on_surface), lv_color_black(), LV_OPA_10), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    return c;
}

static lv_obj_t *label(lv_obj_t *parent, md_type_role_t type)
{
    lv_obj_t *l = md_label_create(parent, "", type);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_clickable(l, false);
    return l;
}

static lv_obj_t *media_button(lv_obj_t *parent, const char *icon, shell_media_cmd_t cmd)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, T5_DP(48), T5_DP(48));
    lv_obj_set_ext_click_area(b, T5_DP(8));
    lv_obj_t *i = md_icon_create(b, icon, MD_ICON_DP);
    lv_obj_center(i);
    lv_obj_set_clickable(i, false);
    lv_obj_add_event_cb(b, media_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)cmd);
    return b;
}

static void build_music(void)
{
    s.music = card(s.cards);
    s.m_who = label(s.music, MD_TYPE_OVERLINE);
    s.m_title = label(s.music, MD_TYPE_HEADLINE6);
    s.m_artist = label(s.music, MD_TYPE_BODY2);
    s.m_bar = lv_bar_create(s.music);
    lv_obj_set_size(s.m_bar, LV_PCT(100), T5_DP(4));
    lv_obj_set_style_bg_opa(s.m_bar, LV_OPA_20, 0);
    lv_obj_set_clickable(s.m_bar, false);
    lv_obj_t *row = box(s.music, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    media_button(row, MD_ICON_SKIP_PREVIOUS, SHELL_MEDIA_PREVIOUS);
    s.m_play = media_button(row, MD_ICON_PLAY_ARROW, SHELL_MEDIA_TOGGLE);
    media_button(row, MD_ICON_SKIP_NEXT, SHELL_MEDIA_NEXT);
}

static void build_app(void)
{
    s.app = card(s.cards);
    lv_obj_t *row = box(s.app, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_style_pad_column(row, T5_DP(8), 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s.a_icon = md_icon_create(row, MD_ICON_INFO, 18);
    lv_obj_set_clickable(s.a_icon, false);
    s.a_title = md_label_create(row, "", MD_TYPE_OVERLINE);
    lv_obj_set_flex_grow(s.a_title, 1);
    lv_label_set_long_mode(s.a_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_clickable(s.a_title, false);
    s.a_value = label(s.app, MD_TYPE_HEADLINE4);
    s.a_detail = label(s.app, MD_TYPE_BODY2);
}

/* The colours that follow night mode, on every label and icon. */
static void paint(lv_obj_t *o)
{
    lv_obj_set_style_text_color(o, ink(), 0);
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) paint(lv_obj_get_child(o, (int32_t)i));
}

static void update_music(void)
{
    shell_media_t m;
    bool on = shell_sys_media(&m) && m.active;
    if (on != s.have_music) {
        s.have_music = on;
        lv_obj_set_hidden(s.music, !on);
    }
    if (!on) return;
    char who[T5_APP_NAME_LEN + 16];
    snprintf(who, sizeof who, "%s", m.who[0] ? m.who : "Now playing");
    shell_set_text(s.m_who, who);
    shell_set_text(s.m_title, m.title[0] ? m.title : "Unknown");
    shell_set_text(s.m_artist, m.artist);
    lv_obj_set_hidden(s.m_bar, !m.duration_ms);
    if (m.duration_ms) {
        int32_t v = (int32_t)((uint64_t)m.position_ms * 1000 / m.duration_ms);
        if (lv_bar_get_max_value(s.m_bar) != 1000) lv_bar_set_range(s.m_bar, 0, 1000);
        if (lv_bar_get_value(s.m_bar) != v) lv_bar_set_value(s.m_bar, v, LV_ANIM_OFF);
    }
    shell_set_text(lv_obj_get_child(s.m_play, 0), m.playing ? MD_ICON_PAUSE : MD_ICON_PLAY_ARROW);
}

static void update_app(void)
{
    shell_card_t c;
    bool on = shell_sys_cards(&c, 1) > 0;
    char sig[sizeof s.app_sig];
    snprintf(sig, sizeof sig, "%d|%s|%s|%s|%s|%s", on, c.icon, c.title, c.value, c.detail, c.who);
    if (!on) memset(&c, 0, sizeof c);
    if (!strcmp(sig, s.app_sig)) return;
    snprintf(s.app_sig, sizeof s.app_sig, "%s", sig);
    lv_obj_set_hidden(s.app, !on);
    if (!on) return;
    shell_set_text(s.a_icon, c.icon[0] ? c.icon : MD_ICON_INFO);
    char title[96];
    bool same = !strcasecmp(c.title, c.who);       /* "Weather" from Weather: once */
    snprintf(title, sizeof title, "%s%s%s", c.title, c.title[0] && c.who[0] && !same ? " · " : "",
             same ? "" : c.who);
    shell_set_text(s.a_title, title);
    shell_set_text(s.a_value, c.value);
    shell_set_text(s.a_detail, c.detail);
}

static void update_clock(void)
{
    shell_show_time(s.clock, s.date);
}

/* Layout for the screen's shape: side by side, or stacked. */
static void layout(void)
{
    int32_t w = lv_obj_get_width(lv_layer_top()), h = lv_obj_get_height(lv_layer_top());
    bool wide = w > h;
    lv_obj_set_flex_flow(s.root, wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s.root, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
}

void standby_show(void)
{
    if (s.root) return;
    keyboard_hide();
    qs_close();
    void *saved = shell_sys_enter();
    s.night = is_night();
    s.root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s.root);
    lv_obj_set_size(s.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s.root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s.root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s.root, T5_DP(24), 0);
    lv_obj_set_scrollable(s.root, false);
    lv_obj_set_clickable(s.root, true);
    lv_obj_add_event_cb(s.root, leave, LV_EVENT_CLICKED, NULL);

    lv_obj_t *left = box(s.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s.clock = md_label_create(left, "", MD_TYPE_HEADLINE4);
    lv_obj_set_clickable(s.clock, false);
    /* The clock at twice the largest type: scaled about its centre, with
     * the room it takes reserved by padding (transforms don't reflow). */
    lv_obj_set_style_transform_scale(s.clock, CLOCK_SCALE, 0);
    lv_obj_set_style_transform_pivot_x(s.clock, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(s.clock, LV_PCT(50), 0);
    lv_obj_set_style_pad_ver(s.clock, T5_DP(34) / 2, 0);
    lv_obj_set_style_pad_hor(s.clock, T5_DP(60), 0);
    s.date = md_label_create(left, "", MD_TYPE_HEADLINE6);
    lv_obj_set_clickable(s.date, false);

    s.cards = box(s.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s.cards, T5_DP(16), 0);
    build_music();
    build_app();
    s.have_music = true;                 /* so the first update sets it */
    s.app_sig[0] = 1;
    layout();
    update_clock();
    update_music();
    update_app();
    paint(s.root);
    lv_obj_move_foreground(s.root);
    lv_obj_add_event_cb(lv_layer_top(), child_created, LV_EVENT_CHILD_CREATED, NULL);
    shell_sys_leave(saved);
    s.percent = s.night ? STANDBY_NIGHT_PERCENT : STANDBY_PERCENT;
    shell_sys_standby_hold(true, s.percent);
}

void standby_hide(void)
{
    if (!s.root) return;
    lv_obj_remove_event_cb(lv_layer_top(), child_created);
    lv_async_call_cancel(check_front, NULL);
    lv_obj_delete(s.root);
    memset(&s, 0, sizeof s);
    shell_sys_standby_hold(false, -1);
}

bool standby_shown(void)
{
    return s.root != NULL;
}

void standby_tick(void)
{
    if (!s.root) return;
    void *saved = shell_sys_enter();
    bool night = is_night();
    if (night != s.night) {
        s.night = night;
        paint(s.root);
    }
    layout();
    update_clock();
    update_music();
    update_app();
    shell_sys_leave(saved);
    /* Again each second: Dim, low-power mode or a wake may have set their own cap. */
    s.percent = s.night ? STANDBY_NIGHT_PERCENT : STANDBY_PERCENT;
    shell_sys_standby_hold(true, s.percent);
}

void standby_reset(void)
{
    if (s.root) lv_obj_remove_event_cb(lv_layer_top(), child_created);
    memset(&s, 0, sizeof s);
}
