/* SPDX-License-Identifier: Apache-2.0
 * The standby screen (t5_shell/standby.h): a full-screen layer over the app,
 * in two halves. One holds a large clock and the date. The other holds the
 * app cards as a stack the person scrolls through a card at a time, with a
 * dot per card, and under it the music bar (the active media session, with
 * previous / play-pause / next), pinned. Landscape puts the clock left;
 * portrait puts it on top. From 22:00 to 07:00 everything turns dim red.
 *
 * The stack keeps the cards in a fixed order, by app name, so they don't
 * change places whenever one updates. It opens on the newest card, and when
 * a card changes it scrolls there, unless the person scrolled the stack in
 * the last two minutes. With no cards and no music the clock takes the
 * whole screen.
 *
 * It lives on lv_layer_top() in front of everything, so the app under it
 * keeps running. A tap anywhere but on the music buttons leaves (a drag on
 * the stack scrolls it instead), and so does a dialog or menu opening
 * (check_front). Updated once a second (standby_tick); a card is rebuilt only
 * when what it shows changes. UI task only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "md/md_fonts.h"
#include "shell_priv.h"

#define STANDBY_PERCENT 20          /* backlight cap */
#define STANDBY_NIGHT_PERCENT 5
#define NIGHT_FROM_H 22
#define NIGHT_TO_H 7
#define NIGHT_RGB 0xb71c1c
#define MAX_CARDS 8                 /* as many as the OS keeps (ambient.c) */
#define ROTATE_IDLE_MS 120000       /* no turning to a new card this soon after a scroll */
#define GAP_DP 24
#define DOT_DP 6

typedef struct {
    lv_obj_t *card, *icon, *title, *value, *detail;
    char who[T5_APP_NAME_LEN];
    char sig[200];
} slot_t;

static struct {
    lv_obj_t *root, *clock_pane, *clock, *date, *side, *stack_row, *stack, *dots, *music;
    lv_obj_t *m_title, *m_artist, *m_bar, *m_play;
    slot_t slot[MAX_CARDS];
    int n;                          /* cards in the stack */
    char order[MAX_CARDS * T5_APP_NAME_LEN];   /* the apps in stack order, to see it change */
    char newest[200];               /* the newest card's app and content */
    char newest_who[T5_APP_NAME_LEN];
    uint32_t touched;               /* lv_tick_get() of the last press on the stack */
    bool touched_ever, placed, night, have_music, wide;
    int percent, dot_on;
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
    lv_obj_set_width(c, LV_PCT(100));
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

/* One line, cut short with "..." when it doesn't fit. */
static lv_obj_t *label(lv_obj_t *parent, md_type_role_t type)
{
    lv_obj_t *l = md_label_create(parent, "", type);
    lv_obj_set_size(l, LV_PCT(100), T5_DP(md_type(type)->line_dp));
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

/* The music bar: title and artist beside the buttons, progress under them. */
static void build_music(void)
{
    s.music = card(s.side);
    lv_obj_set_style_pad_ver(s.music, T5_DP(10), 0);
    lv_obj_set_style_pad_row(s.music, T5_DP(6), 0);
    lv_obj_t *row = box(s.music, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *text = box(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(text, 1);
    s.m_title = label(text, MD_TYPE_SUBTITLE1);
    s.m_artist = label(text, MD_TYPE_BODY2);
    lv_obj_set_style_text_opa(s.m_artist, LV_OPA_70, 0);
    media_button(row, MD_ICON_SKIP_PREVIOUS, SHELL_MEDIA_PREVIOUS);
    s.m_play = media_button(row, MD_ICON_PLAY_ARROW, SHELL_MEDIA_TOGGLE);
    media_button(row, MD_ICON_SKIP_NEXT, SHELL_MEDIA_NEXT);
    s.m_bar = lv_bar_create(s.music);
    lv_obj_set_size(s.m_bar, LV_PCT(100), T5_DP(4));
    lv_obj_set_style_bg_opa(s.m_bar, LV_OPA_20, 0);
    lv_obj_set_clickable(s.m_bar, false);
}

/* A card in the stack: a page of its own, the app and title on top, the
 * value large at the bottom with the detail under it. */
static void build_slot(slot_t *sl)
{
    sl->card = card(s.stack);
    lv_obj_set_height(sl->card, LV_PCT(100));
    lv_obj_set_flex_align(sl->card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_snappable(sl->card, true);
    lv_obj_t *row = box(sl->card, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_style_pad_column(row, T5_DP(8), 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    sl->icon = md_icon_create(row, MD_ICON_INFO, 18);
    lv_obj_set_clickable(sl->icon, false);
    sl->title = md_label_create(row, "", MD_TYPE_OVERLINE);
    lv_obj_set_flex_grow(sl->title, 1);
    lv_label_set_long_mode(sl->title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_clickable(sl->title, false);
    lv_obj_t *foot = box(sl->card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(foot, LV_PCT(100));
    lv_obj_set_style_pad_row(foot, T5_DP(4), 0);
    sl->value = label(foot, MD_TYPE_HEADLINE4);
    sl->detail = md_label_create(foot, "", MD_TYPE_BODY1);   /* up to 63 bytes: wraps */
    lv_obj_set_width(sl->detail, LV_PCT(100));
    lv_obj_set_clickable(sl->detail, false);
    lv_obj_set_hidden(sl->card, true);
}

/* A press on the stack: hold off turning to a new card. */
static void stack_pressed(lv_event_t *e)
{
    (void)e;
    s.touched = lv_tick_get();
    s.touched_ever = true;
}

static int32_t page_h(void)
{
    return lv_obj_get_content_height(s.stack);
}

static int current_page(void)
{
    int32_t h = page_h();
    if (h <= 0 || !s.n) return 0;
    int i = (int)((lv_obj_get_scroll_y(s.stack) + h / 2) / h);
    return i < 0 ? 0 : i >= s.n ? s.n - 1 : i;
}

static void paint_dots(void)
{
    int on = current_page();
    for (uint32_t i = 0; i < lv_obj_get_child_count(s.dots); i++) {
        lv_obj_t *d = lv_obj_get_child(s.dots, (int32_t)i);
        lv_obj_set_hidden(d, (int)i >= s.n || s.n < 2);
        lv_obj_set_style_bg_color(d, ink(), 0);
        lv_obj_set_style_bg_opa(d, (int)i == on ? LV_OPA_COVER : LV_OPA_30, 0);
    }
    s.dot_on = on;
}

static void stack_scrolled(lv_event_t *e)
{
    (void)e;
    if (current_page() != s.dot_on) paint_dots();
}

static void build_stack(void)
{
    s.stack_row = box(s.side, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(s.stack_row, LV_PCT(100));
    lv_obj_set_flex_grow(s.stack_row, 1);
    lv_obj_set_style_pad_column(s.stack_row, T5_DP(8), 0);
    lv_obj_set_flex_align(s.stack_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s.stack = lv_obj_create(s.stack_row);
    lv_obj_remove_style_all(s.stack);
    lv_obj_set_height(s.stack, LV_PCT(100));
    lv_obj_set_flex_grow(s.stack, 1);
    lv_obj_set_flex_flow(s.stack, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s.stack, 0, 0);
    lv_obj_set_scroll_dir(s.stack, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(s.stack, LV_SCROLL_SNAP_START);
    lv_obj_set_scrollbar_mode(s.stack, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_one(s.stack, true);
    lv_obj_set_scroll_chain_ver(s.stack, false);
    lv_obj_set_clickable(s.stack, true);
    /* A tap leaves, as anywhere else; LVGL sends no click after a drag. */
    lv_obj_add_event_cb(s.stack, leave, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s.stack, stack_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s.stack, stack_scrolled, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(s.stack, stack_scrolled, LV_EVENT_SCROLL_END, NULL);
    for (int i = 0; i < MAX_CARDS; i++) build_slot(&s.slot[i]);

    s.dots = box(s.stack_row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s.dots, T5_DP(8), 0);
    for (int i = 0; i < MAX_CARDS; i++) {
        lv_obj_t *d = lv_obj_create(s.dots);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, T5_DP(DOT_DP), T5_DP(DOT_DP));
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_clickable(d, false);
    }
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
    shell_set_text(s.m_title, m.title[0] ? m.title : "Unknown");
    shell_set_text(s.m_artist, m.artist[0] ? m.artist : m.who[0] ? m.who : "Now playing");
    lv_obj_set_hidden(s.m_bar, !m.duration_ms);
    if (m.duration_ms) {
        int32_t v = (int32_t)((uint64_t)m.position_ms * 1000 / m.duration_ms);
        if (lv_bar_get_max_value(s.m_bar) != 1000) lv_bar_set_range(s.m_bar, 0, 1000);
        if (lv_bar_get_value(s.m_bar) != v) lv_bar_set_value(s.m_bar, v, LV_ANIM_OFF);
    }
    shell_set_text(lv_obj_get_child(s.m_play, 0), m.playing ? MD_ICON_PAUSE : MD_ICON_PLAY_ARROW);
}

static void fill_slot(slot_t *sl, const shell_card_t *c)
{
    char sig[sizeof sl->sig];
    snprintf(sig, sizeof sig, "%s|%s|%s|%s|%s", c->icon, c->title, c->value, c->detail, c->who);
    if (!strcmp(sig, sl->sig)) return;
    snprintf(sl->sig, sizeof sl->sig, "%s", sig);
    snprintf(sl->who, sizeof sl->who, "%s", c->who);
    shell_set_text(sl->icon, c->icon[0] ? c->icon : MD_ICON_INFO);
    char title[96];
    bool same = !strcasecmp(c->title, c->who);      /* "Weather" from Weather: once */
    /* "Logger: Greenhouse", or just "Weather" when the card is titled
     * after its app. */
    if (!same && c->who[0] && c->title[0]) snprintf(title, sizeof title, "%s: %s", c->who, c->title);
    else snprintf(title, sizeof title, "%s", c->title[0] ? c->title : c->who);
    shell_set_text(sl->title, title);
    shell_set_text(sl->value, c->value);
    shell_set_text(sl->detail, c->detail);
}

static int by_name(const void *a, const void *b)
{
    const shell_card_t *x = a, *y = b;
    int d = strcasecmp(x->who, y->who);
    return d ? d : strcasecmp(x->title, y->title);
}

static int slot_of(const char *who)
{
    for (int i = 0; i < s.n; i++) {
        if (!strcmp(s.slot[i].who, who)) return i;
    }
    return -1;
}

static void show_page(int i, bool anim)
{
    if (i < 0 || i >= s.n) return;
    lv_obj_update_layout(s.root);
    lv_obj_scroll_to_y(s.stack, (int32_t)i * page_h(), anim ? LV_ANIM_ON : LV_ANIM_OFF);
    if (!anim) paint_dots();
}

static void update_cards(void)
{
    shell_card_t c[MAX_CARDS];
    int n = shell_sys_cards(c, MAX_CARDS);
    char newest[sizeof s.newest] = "";
    if (n > 0) snprintf(newest, sizeof newest, "%s|%s|%s|%s", c[0].who, c[0].title, c[0].value, c[0].detail);
    char newest_who[T5_APP_NAME_LEN] = "";
    if (n > 0) snprintf(newest_who, sizeof newest_who, "%s", c[0].who);
    qsort(c, (size_t)n, sizeof c[0], by_name);

    /* Which card is showing, so it stays if others come or go. */
    char was[T5_APP_NAME_LEN] = "";
    if (s.n) snprintf(was, sizeof was, "%s", s.slot[current_page()].who);
    char order[sizeof s.order] = "";
    for (int i = 0; i < n; i++) {
        strncat(order, c[i].who, sizeof order - strlen(order) - 2);
        strcat(order, "\n");
    }
    bool reordered = strcmp(order, s.order) != 0;
    snprintf(s.order, sizeof s.order, "%s", order);

    for (int i = 0; i < MAX_CARDS; i++) {
        slot_t *sl = &s.slot[i];
        if (i < n) fill_slot(sl, &c[i]);
        else sl->sig[0] = sl->who[0] = 0;
        lv_obj_set_hidden(sl->card, i >= n);
    }
    if (lv_obj_is_hidden(s.stack_row) != !n) {
        lv_obj_set_hidden(s.stack_row, !n);
        /* Music alone sits in the middle of its half. */
        lv_obj_set_flex_align(s.side, n ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
    }
    s.n = n;
    snprintf(s.newest_who, sizeof s.newest_who, "%s", newest_who);

    bool changed = strcmp(newest, s.newest) != 0;
    snprintf(s.newest, sizeof s.newest, "%s", newest);
    bool idle = !s.touched_ever || lv_tick_elaps(s.touched) >= ROTATE_IDLE_MS;
    if (!s.placed) return;               /* standby_show places it once laid out */
    if (n && changed && idle) show_page(slot_of(newest_who), true);
    else if (reordered && was[0] && slot_of(was) >= 0) show_page(slot_of(was), false);
    else if (reordered) show_page(0, false);
    else paint_dots();
}

static void update_clock(void)
{
    shell_show_time(s.clock, s.date);
}

/* The halves for the screen's shape: side by side, or one over the other.
 * With nothing to show beside it, the clock takes the whole screen. */
static void layout(void)
{
    int32_t w = lv_obj_get_width(lv_layer_top()), h = lv_obj_get_height(lv_layer_top());
    bool wide = w > h;
    bool side = s.n || s.have_music;
    if (wide != s.wide) {
        s.wide = wide;
        lv_obj_set_flex_flow(s.root, wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    }
    lv_obj_set_hidden(s.side, !side);
    lv_obj_set_size(s.clock_pane, wide ? LV_SIZE_CONTENT : LV_PCT(100), wide ? LV_PCT(100) : LV_SIZE_CONTENT);
    lv_obj_set_size(s.side, wide ? LV_SIZE_CONTENT : LV_PCT(100), wide ? LV_PCT(100) : LV_SIZE_CONTENT);
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
    lv_obj_set_style_pad_all(s.root, T5_DP(GAP_DP), 0);
    lv_obj_set_style_pad_gap(s.root, T5_DP(GAP_DP), 0);
    lv_obj_set_scrollable(s.root, false);
    lv_obj_set_clickable(s.root, true);
    lv_obj_add_event_cb(s.root, leave, LV_EVENT_CLICKED, NULL);

    /* Both halves grow from nothing, so they split the screen evenly. */
    s.clock_pane = box(s.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(s.clock_pane, 1);
    lv_obj_set_flex_align(s.clock_pane, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s.clock = md_label_create(s.clock_pane, "", MD_TYPE_HEADLINE4);
    lv_obj_set_style_text_font(s.clock, &md_font_roboto_clock_200, 0);
    lv_obj_set_style_text_letter_space(s.clock, 0, 0);
    lv_obj_set_style_text_line_space(s.clock, 0, 0);
    lv_obj_set_clickable(s.clock, false);
    s.date = md_label_create(s.clock_pane, "", MD_TYPE_HEADLINE5);
    lv_obj_set_style_pad_top(s.date, T5_DP(8), 0);
    lv_obj_set_clickable(s.date, false);

    s.side = box(s.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(s.side, 1);
    lv_obj_set_style_pad_row(s.side, T5_DP(12), 0);
    build_stack();
    build_music();
    s.have_music = true;                 /* so the first update sets it */
    s.wide = !(lv_obj_get_width(lv_layer_top()) > lv_obj_get_height(lv_layer_top()));
    update_music();
    update_cards();
    layout();
    update_clock();
    paint(s.root);
    lv_obj_move_foreground(s.root);
    s.placed = true;
    if (s.n) show_page(slot_of(s.newest_who), false);   /* open on the newest card */
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
        paint_dots();
    }
    bool was_wide = s.wide;
    int32_t was_h = page_h();
    update_music();
    update_cards();
    layout();
    lv_obj_update_layout(s.root);
    /* Rotated, or the music bar came or went: the pages changed size. */
    if (s.n && (was_wide != s.wide || page_h() != was_h)) show_page(s.dot_on, false);
    update_clock();
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
