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
 * It lives on the top layer in front of everything, so the app under it
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
    t5ui_obj_t *card, *icon, *title, *value, *detail;
    char who[T5_APP_NAME_LEN];
    char sig[200];
} slot_t;

static struct {
    t5ui_obj_t *root, *clock_pane, *clock, *date, *side, *stack_row, *stack, *dots, *music;
    t5ui_obj_t *m_title, *m_artist, *m_bar, *m_play;
    slot_t slot[MAX_CARDS];
    int n;                          /* cards in the stack */
    char order[MAX_CARDS * T5_APP_NAME_LEN];   /* the apps in stack order, to see it change */
    char newest[200];               /* the newest card's app and content */
    char newest_who[T5_APP_NAME_LEN];
    uint32_t touched;               /* t5ui_ticks_ms() of the last press on the stack */
    bool touched_ever, placed, night, have_music, wide;
    int percent, dot_on;
} s;

static bool is_night(void)
{
    struct tm tm;
    if (!shell_sys_time(&tm)) return false;
    return tm.tm_hour >= NIGHT_FROM_H || tm.tm_hour < NIGHT_TO_H;
}

static t5ui_color_t ink(void)
{
    return s.night ? NIGHT_RGB : t5ui_theme_color(T5UI_COLOR_ON_SURFACE);
}

static void size_of(t5ui_obj_t *o, int32_t *w, int32_t *h)
{
    t5ui_rect_t r;
    t5ui_coords(o, &r);
    if (w) *w = r.x2 - r.x1 + 1;
    if (h) *h = r.y2 - r.y1 + 1;
}

static void leave(t5ui_event_t *e)
{
    if (t5ui_event_target(e) == t5ui_event_obj(e)) standby_hide();
}

/* Something opened in front of standby once its creator has sized it: a
 * dialog's scrim, a menu's or sheet's catcher (anything covering the whole
 * screen) wants an answer, so standby goes. A toast just shows on top. */
static void check_front(void *arg)
{
    (void)arg;
    if (!s.root) return;
    t5ui_obj_t *top = t5ui_sys_layer(T5UI_LAYER_TOP);
    int32_t w, h;
    size_of(top, &w, &h);              /* lays it out */
    int32_t n = (int32_t)t5ui_child_count(top), i = 0;
    while (i < n && t5ui_child(top, i) != s.root) i++;
    for (i++; i < n; i++) {
        t5ui_obj_t *o = t5ui_child(top, i);
        if (t5ui_get_int(o, T5UI_P_HIDDEN, 0)) continue;
        int32_t ow, oh;
        size_of(o, &ow, &oh);
        if (ow >= w && oh >= h) {
            standby_hide();
            return;
        }
    }
}

static void child_created(t5ui_event_t *e)
{
    if (s.root && t5ui_event_target(e) != s.root) {
        t5ui_later_cancel(check_front, NULL);
        t5ui_later(check_front, NULL);
    }
}

static void media_clicked(t5ui_event_t *e)
{
    shell_sys_media_cmd((shell_media_cmd_t)(intptr_t)t5ui_event_user(e));
}

static t5ui_obj_t *box(t5ui_obj_t *parent, t5ui_flow_t flow)
{
    t5ui_obj_t *o = shell_box(parent);
    shell_size(o, T5UI_CONTENT, T5UI_CONTENT);
    t5ui_set_int(o, T5UI_P_FLOW, flow);
    t5ui_set_int(o, T5UI_P_SCROLLABLE, false);
    t5ui_set_int(o, T5UI_P_CLICKABLE, false);
    return o;
}

static t5ui_obj_t *card(t5ui_obj_t *parent)
{
    t5ui_obj_t *c = box(parent, T5UI_COLUMN);
    t5ui_set_int(c, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(c, T5UI_P_PAD_ALL, t5ui_dp(16));
    t5ui_set_int(c, T5UI_P_PAD_ROW, t5ui_dp(4));
    t5ui_set_int(c, T5UI_P_RADIUS, t5ui_dp(16));
    /* Opaque, mixed here: a faint translucent fill over black comes out a
     * shade different where the 2D hardware blends it and where software
     * does (the rounded ends), a visible band. */
    t5ui_set_color(c, T5UI_P_BG_COLOR, t5ui_mix(t5ui_theme_color(T5UI_COLOR_ON_SURFACE), 0x000000, 25));   /* 10% */
    t5ui_set_int(c, T5UI_P_BG_OPA, T5UI_OPA_COVER);
    return c;
}

/* A text style's line height (Material's baseline to baseline). */
static int32_t line_dp(t5ui_text_t type)
{
    switch (type) {
    case T5UI_TEXT_HEADLINE4: return 40;
    case T5UI_TEXT_SUBTITLE1: return 28;
    case T5UI_TEXT_BODY2: return 20;
    default: return 24;
    }
}

/* One line, cut short with "..." when it doesn't fit. */
static t5ui_obj_t *label(t5ui_obj_t *parent, t5ui_text_t type)
{
    t5ui_obj_t *l = shell_text(parent, "", type);
    shell_size(l, T5UI_PCT(100), t5ui_dp(line_dp(type)));
    t5ui_set_int(l, T5UI_P_LONG_MODE, T5UI_LONG_DOTS);
    t5ui_set_int(l, T5UI_P_CLICKABLE, false);
    return l;
}

static t5ui_obj_t *media_button(t5ui_obj_t *parent, const char *icon, shell_media_cmd_t cmd)
{
    t5ui_obj_t *b = shell_box(parent);
    shell_size(b, t5ui_dp(48), t5ui_dp(48));
    t5ui_set_int(b, T5UI_P_EXT_CLICK, t5ui_dp(8));
    t5ui_obj_t *i = shell_icon(b, icon, SHELL_ICON_DP);
    t5ui_set_int(i, T5UI_P_ALIGN, T5UI_ALIGN_CENTER);
    t5ui_set_int(i, T5UI_P_CLICKABLE, false);
    t5ui_on(b, T5UI_EV_CLICK, media_clicked, (void *)(intptr_t)cmd);
    return b;
}

/* The music bar: title and artist beside the buttons, progress under them. */
static void build_music(void)
{
    s.music = card(s.side);
    shell_pad_ver(s.music, t5ui_dp(10));
    t5ui_set_int(s.music, T5UI_P_PAD_ROW, t5ui_dp(6));
    t5ui_obj_t *row = box(s.music, T5UI_ROW);
    t5ui_set_int(row, T5UI_P_WIDTH, T5UI_PCT(100));
    shell_flex(row, T5UI_ROW, T5UI_PLACE_START, T5UI_PLACE_CENTER, T5UI_PLACE_CENTER);
    t5ui_obj_t *text = box(row, T5UI_COLUMN);
    t5ui_set_int(text, T5UI_P_GROW, 1);
    s.m_title = label(text, T5UI_TEXT_SUBTITLE1);
    s.m_artist = label(text, T5UI_TEXT_BODY2);
    t5ui_set_int(s.m_artist, T5UI_P_TEXT_OPA, 178);   /* 70% */
    media_button(row, T5UI_ICON_SKIP_PREVIOUS, SHELL_MEDIA_PREVIOUS);
    s.m_play = media_button(row, T5UI_ICON_PLAY_ARROW, SHELL_MEDIA_TOGGLE);
    media_button(row, T5UI_ICON_SKIP_NEXT, SHELL_MEDIA_NEXT);
    s.m_bar = t5ui_create(s.music, T5UI_BAR);
    shell_size(s.m_bar, T5UI_PCT(100), t5ui_dp(4));
    t5ui_set_int(s.m_bar, T5UI_P_BG_OPA, 51);   /* 20% */
    t5ui_set_int(s.m_bar, T5UI_P_CLICKABLE, false);
}

/* A card in the stack: a page of its own, the app and title on top, the
 * value large at the bottom with the detail under it. */
static void build_slot(slot_t *sl)
{
    sl->card = card(s.stack);
    t5ui_set_int(sl->card, T5UI_P_HEIGHT, T5UI_PCT(100));
    shell_flex(sl->card, T5UI_COLUMN, T5UI_PLACE_SPACE_BETWEEN, T5UI_PLACE_START, T5UI_PLACE_START);
    t5ui_set_int(sl->card, T5UI_P_SNAPPABLE, true);
    t5ui_obj_t *row = box(sl->card, T5UI_ROW);
    t5ui_set_int(row, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(row, T5UI_P_PAD_COLUMN, t5ui_dp(8));
    shell_flex(row, T5UI_ROW, T5UI_PLACE_START, T5UI_PLACE_CENTER, T5UI_PLACE_CENTER);
    sl->icon = shell_icon(row, T5UI_ICON_INFO, 18);
    t5ui_set_int(sl->icon, T5UI_P_CLICKABLE, false);
    sl->title = shell_text(row, "", T5UI_TEXT_OVERLINE);
    t5ui_set_int(sl->title, T5UI_P_GROW, 1);
    t5ui_set_int(sl->title, T5UI_P_LONG_MODE, T5UI_LONG_DOTS);
    t5ui_set_int(sl->title, T5UI_P_CLICKABLE, false);
    t5ui_obj_t *foot = box(sl->card, T5UI_COLUMN);
    t5ui_set_int(foot, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(foot, T5UI_P_PAD_ROW, t5ui_dp(4));
    sl->value = label(foot, T5UI_TEXT_HEADLINE4);
    sl->detail = shell_text(foot, "", T5UI_TEXT_BODY1);   /* up to 63 bytes: wraps */
    t5ui_set_int(sl->detail, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(sl->detail, T5UI_P_CLICKABLE, false);
    t5ui_set_int(sl->card, T5UI_P_HIDDEN, true);
}

/* A press on the stack: hold off turning to a new card. */
static void stack_pressed(t5ui_event_t *e)
{
    (void)e;
    s.touched = t5ui_ticks_ms();
    s.touched_ever = true;
}

static int32_t page_h(void)
{
    t5ui_rect_t r;
    t5ui_content_coords(s.stack, &r);
    return r.y2 - r.y1 + 1;
}

static int current_page(void)
{
    int32_t h = page_h();
    if (h <= 0 || !s.n) return 0;
    int i = (int)((t5ui_get_int(s.stack, T5UI_P_SCROLL_Y, 0) + h / 2) / h);
    return i < 0 ? 0 : i >= s.n ? s.n - 1 : i;
}

static void paint_dots(void)
{
    int on = current_page();
    for (uint32_t i = 0; i < t5ui_child_count(s.dots); i++) {
        t5ui_obj_t *d = t5ui_child(s.dots, (int32_t)i);
        t5ui_set_int(d, T5UI_P_HIDDEN, (int)i >= s.n || s.n < 2);
        t5ui_set_color(d, T5UI_P_BG_COLOR, ink());
        t5ui_set_int(d, T5UI_P_BG_OPA, (int)i == on ? T5UI_OPA_COVER : 76);   /* 30% */
    }
    s.dot_on = on;
}

static void stack_scrolled(t5ui_event_t *e)
{
    (void)e;
    if (current_page() != s.dot_on) paint_dots();
}

static void build_stack(void)
{
    s.stack_row = box(s.side, T5UI_ROW);
    t5ui_set_int(s.stack_row, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(s.stack_row, T5UI_P_GROW, 1);
    t5ui_set_int(s.stack_row, T5UI_P_PAD_COLUMN, t5ui_dp(8));
    shell_flex(s.stack_row, T5UI_ROW, T5UI_PLACE_START, T5UI_PLACE_CENTER, T5UI_PLACE_CENTER);

    s.stack = shell_box(s.stack_row);
    t5ui_set_int(s.stack, T5UI_P_HEIGHT, T5UI_PCT(100));
    t5ui_set_int(s.stack, T5UI_P_GROW, 1);
    t5ui_set_int(s.stack, T5UI_P_FLOW, T5UI_COLUMN);
    t5ui_set_int(s.stack, T5UI_P_PAD_ROW, 0);
    t5ui_set_int(s.stack, T5UI_P_SCROLL_DIR, T5UI_DIR_VER);
    t5ui_set_int(s.stack, T5UI_P_SCROLL_SNAP_Y, T5UI_SNAP_START);
    t5ui_set_int(s.stack, T5UI_P_SCROLLBAR, T5UI_SCROLLBAR_OFF);
    t5ui_set_int(s.stack, T5UI_P_SCROLL_ONE, true);
    t5ui_set_int(s.stack, T5UI_P_SCROLL_CHAIN, false);   /* it only scrolls up and down */
    t5ui_set_int(s.stack, T5UI_P_CLICKABLE, true);
    /* A tap leaves, as anywhere else; no click comes after a drag. */
    t5ui_on(s.stack, T5UI_EV_CLICK, leave, NULL);
    t5ui_on(s.stack, T5UI_EV_PRESS, stack_pressed, NULL);
    t5ui_on(s.stack, T5UI_EV_SCROLL, stack_scrolled, NULL);
    t5ui_on(s.stack, T5UI_EV_SCROLL_END, stack_scrolled, NULL);
    for (int i = 0; i < MAX_CARDS; i++) build_slot(&s.slot[i]);

    s.dots = box(s.stack_row, T5UI_COLUMN);
    t5ui_set_int(s.dots, T5UI_P_PAD_ROW, t5ui_dp(8));
    for (int i = 0; i < MAX_CARDS; i++) {
        t5ui_obj_t *d = shell_box(s.dots);
        shell_size(d, t5ui_dp(DOT_DP), t5ui_dp(DOT_DP));
        t5ui_set_int(d, T5UI_P_RADIUS, T5UI_RADIUS_CIRCLE);
        t5ui_set_int(d, T5UI_P_CLICKABLE, false);
    }
}

/* The colours that follow night mode, on every label and icon. */
static void paint(t5ui_obj_t *o)
{
    t5ui_set_color(o, T5UI_P_TEXT_COLOR, ink());
    for (uint32_t i = 0; i < t5ui_child_count(o); i++) paint(t5ui_child(o, (int32_t)i));
}

static void update_music(void)
{
    shell_media_t m;
    bool on = shell_sys_media(&m) && m.active;
    if (on != s.have_music) {
        s.have_music = on;
        t5ui_set_int(s.music, T5UI_P_HIDDEN, !on);
    }
    if (!on) return;
    shell_set_text(s.m_title, m.title[0] ? m.title : "Unknown");
    shell_set_text(s.m_artist, m.artist[0] ? m.artist : m.who[0] ? m.who : "Now playing");
    t5ui_set_int(s.m_bar, T5UI_P_HIDDEN, !m.duration_ms);
    if (m.duration_ms) {
        int32_t v = (int32_t)((uint64_t)m.position_ms * 1000 / m.duration_ms);
        if (t5ui_get_int(s.m_bar, T5UI_P_MAX, 0) != 1000) {
            t5ui_set_int(s.m_bar, T5UI_P_MIN, 0);
            t5ui_set_int(s.m_bar, T5UI_P_MAX, 1000);
        }
        if (t5ui_get_int(s.m_bar, T5UI_P_VALUE, -1) != v) t5ui_set_int(s.m_bar, T5UI_P_VALUE, v);
    }
    shell_set_text(t5ui_child(s.m_play, 0), m.playing ? T5UI_ICON_PAUSE : T5UI_ICON_PLAY_ARROW);
}

static void fill_slot(slot_t *sl, const shell_card_t *c)
{
    char sig[sizeof sl->sig];
    snprintf(sig, sizeof sig, "%s|%s|%s|%s|%s", c->icon, c->title, c->value, c->detail, c->who);
    if (!strcmp(sig, sl->sig)) return;
    snprintf(sl->sig, sizeof sl->sig, "%s", sig);
    snprintf(sl->who, sizeof sl->who, "%s", c->who);
    shell_set_text(sl->icon, c->icon[0] ? c->icon : T5UI_ICON_INFO);
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
    t5ui_layout_now(s.root);
    t5ui_cmd(s.stack, T5UI_CMD_SCROLL_TO, t5ui_get_int(s.stack, T5UI_P_SCROLL_X, 0), (int32_t)i * page_h(), anim,
             NULL);
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
        t5ui_set_int(sl->card, T5UI_P_HIDDEN, i >= n);
    }
    if (t5ui_get_int(s.stack_row, T5UI_P_HIDDEN, 0) != !n) {
        t5ui_set_int(s.stack_row, T5UI_P_HIDDEN, !n);
        /* Music alone sits in the middle of its half. */
        shell_flex(s.side, T5UI_COLUMN, n ? T5UI_PLACE_START : T5UI_PLACE_CENTER, T5UI_PLACE_CENTER,
                   T5UI_PLACE_CENTER);
    }
    s.n = n;
    snprintf(s.newest_who, sizeof s.newest_who, "%s", newest_who);

    bool changed = strcmp(newest, s.newest) != 0;
    snprintf(s.newest, sizeof s.newest, "%s", newest);
    bool idle = !s.touched_ever || t5ui_ticks_ms() - s.touched >= ROTATE_IDLE_MS;
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
    int32_t w, h;
    size_of(t5ui_sys_layer(T5UI_LAYER_TOP), &w, &h);
    bool wide = w > h;
    bool side = s.n || s.have_music;
    if (wide != s.wide) {
        s.wide = wide;
        t5ui_set_int(s.root, T5UI_P_FLOW, wide ? T5UI_ROW : T5UI_COLUMN);
    }
    t5ui_set_int(s.side, T5UI_P_HIDDEN, !side);
    shell_size(s.clock_pane, wide ? T5UI_CONTENT : T5UI_PCT(100), wide ? T5UI_PCT(100) : T5UI_CONTENT);
    shell_size(s.side, wide ? T5UI_CONTENT : T5UI_PCT(100), wide ? T5UI_PCT(100) : T5UI_CONTENT);
}

void standby_show(void)
{
    if (s.root) return;
    keyboard_hide();
    qs_close();
    t5ui_obj_t *top = t5ui_sys_layer(T5UI_LAYER_TOP);
    void *saved = shell_sys_enter();
    s.night = is_night();
    s.root = shell_box(top);
    shell_size(s.root, T5UI_PCT(100), T5UI_PCT(100));
    t5ui_set_color(s.root, T5UI_P_BG_COLOR, 0x000000);
    t5ui_set_int(s.root, T5UI_P_BG_OPA, T5UI_OPA_COVER);
    t5ui_set_int(s.root, T5UI_P_PAD_ALL, t5ui_dp(GAP_DP));
    t5ui_set_int(s.root, T5UI_P_PAD_ROW, t5ui_dp(GAP_DP));
    t5ui_set_int(s.root, T5UI_P_PAD_COLUMN, t5ui_dp(GAP_DP));
    t5ui_set_int(s.root, T5UI_P_SCROLLABLE, false);
    t5ui_set_int(s.root, T5UI_P_CLICKABLE, true);
    t5ui_on(s.root, T5UI_EV_CLICK, leave, NULL);

    /* Both halves grow from nothing, so they split the screen evenly. */
    s.clock_pane = box(s.root, T5UI_COLUMN);
    t5ui_set_int(s.clock_pane, T5UI_P_GROW, 1);
    shell_flex(s.clock_pane, T5UI_COLUMN, T5UI_PLACE_CENTER, T5UI_PLACE_CENTER, T5UI_PLACE_CENTER);
    s.clock = shell_text(s.clock_pane, "", T5UI_TEXT_HEADLINE4);
    t5ui_set_int(s.clock, T5UI_P_TEXT_STYLE, T5UI_TEXT_CLOCK);
    t5ui_set_int(s.clock, T5UI_P_LETTER_SPACE, 0);
    t5ui_set_int(s.clock, T5UI_P_LINE_SPACE, 0);
    t5ui_set_int(s.clock, T5UI_P_CLICKABLE, false);
    s.date = shell_text(s.clock_pane, "", T5UI_TEXT_HEADLINE5);
    t5ui_set_int(s.date, T5UI_P_PAD_TOP, t5ui_dp(8));
    t5ui_set_int(s.date, T5UI_P_CLICKABLE, false);

    s.side = box(s.root, T5UI_COLUMN);
    t5ui_set_int(s.side, T5UI_P_GROW, 1);
    t5ui_set_int(s.side, T5UI_P_PAD_ROW, t5ui_dp(12));
    build_stack();
    build_music();
    s.have_music = true;                 /* so the first update sets it */
    int32_t w, h;
    size_of(top, &w, &h);
    s.wide = !(w > h);
    update_music();
    update_cards();
    layout();
    update_clock();
    paint(s.root);
    t5ui_move_to(s.root, -1);
    s.placed = true;
    if (s.n) show_page(slot_of(s.newest_who), false);   /* open on the newest card */
    t5ui_on(top, T5UI_EV_CHILD_CREATED, child_created, NULL);
    shell_sys_leave(saved);
    s.percent = s.night ? STANDBY_NIGHT_PERCENT : STANDBY_PERCENT;
    shell_sys_standby_hold(true, s.percent);
}

void standby_hide(void)
{
    if (!s.root) return;
    t5ui_off(t5ui_sys_layer(T5UI_LAYER_TOP), T5UI_EV_CHILD_CREATED, child_created, NULL);
    t5ui_later_cancel(check_front, NULL);
    t5ui_delete(s.root);
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
    t5ui_layout_now(s.root);
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
    if (s.root) t5ui_off(t5ui_sys_layer(T5UI_LAYER_TOP), T5UI_EV_CHILD_CREATED, child_created, NULL);
    memset(&s, 0, sizeof s);
}
