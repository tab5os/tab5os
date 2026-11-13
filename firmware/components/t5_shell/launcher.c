/* SPDX-License-Identifier: Apache-2.0
 * Launcher (plan 6.3), laid out like a phone launcher:
 *
 *   home    the clock and date at the top left over a soft wallpaper
 *           gradient, and a dock of up to five apps at the bottom
 *   drawer  every app, A to Z, with the recent apps on top; it slides up
 *           over home on a swipe up (gesture.c) or a tap on the chevron
 *           above the dock, and goes away on a pull down past the top of
 *           the list, the chevron at its top, or a swipe up from the bottom
 *           edge
 *
 * Tap an icon to open the app; long press for Open, App info and Remove.
 * The app list is rebuilt only when it changes; the clock updates once a
 * second and only redraws when the minute changes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shell_priv.h"

#define DOCK_N 5
#define CELL_MIN_W_DP 96
#define CELL_H_DP 116            /* badge plus a two-line name */
#define BADGE_DP 56
#define DOCK_BADGE_DP 52
#define PULL_CLOSE_DP 48

/* The apps the dock prefers, in order; the rest of it fills from the list. */
static const char *const DOCK_PREFS[] = { T5_APP_ID_SETTINGS, T5_APP_ID_FILES, T5_APP_ID_CAMERA,
                                          T5_APP_ID_MANAGER, T5_APP_ID_POWER };

static t5ui_obj_t *s_scr, *s_time, *s_date, *s_dock;
static t5ui_obj_t *s_drawer, *s_recent_row, *s_recent_box, *s_grid;
static bool s_drawer_open;
static shell_app_t *s_apps;
static int s_napps;
static char s_shown_recents[SHELL_RECENTS][T5_APP_ID_LEN];
static char s_dock_ids[DOCK_N][T5_APP_ID_LEN];

/* A stable hue per app from its id. Badges are tonal: the accent's soft
 * 200 tone for the glyph on a dark circle tinted with the same hue, so a
 * row of unrelated hues still reads as one set, and each app keeps its
 * colour. Full-strength 700 discs side by side were loud and clashed. */
#define BADGE_TINT_OPA 72        /* 28% of the hue over the surface */

static t5ui_accent_t badge_accent(const char *id)
{
    static const t5ui_accent_t pick[] = { T5UI_ACCENT_BLUE, T5UI_ACCENT_TEAL, T5UI_ACCENT_INDIGO, T5UI_ACCENT_GREEN,
                                          T5UI_ACCENT_ORANGE, T5UI_ACCENT_PINK, T5UI_ACCENT_CYAN, T5UI_ACCENT_PURPLE,
                                          T5UI_ACCENT_RED, T5UI_ACCENT_AMBER };
    uint32_t h = 2166136261u;
    for (const char *p = id; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return pick[h % (sizeof pick / sizeof pick[0])];
}

static const shell_app_t *find(const char *id)
{
    for (int i = 0; i < s_napps; i++) {
        if (!strcmp(s_apps[i].id, id)) return &s_apps[i];
    }
    return NULL;
}

static int32_t width_of(t5ui_obj_t *o)
{
    t5ui_rect_t r;
    t5ui_coords(o, &r);
    return r.x2 - r.x1 + 1;
}

static int32_t height_of(t5ui_obj_t *o)
{
    t5ui_rect_t r;
    t5ui_coords(o, &r);
    return r.y2 - r.y1 + 1;
}

static t5ui_obj_t *badge(t5ui_obj_t *parent, const shell_app_t *a, int size_dp, int icon_dp)
{
    t5ui_obj_t *b = shell_box(parent);
    shell_size(b, t5ui_dp(size_dp), t5ui_dp(size_dp));
    t5ui_set_int(b, T5UI_P_RADIUS, T5UI_RADIUS_CIRCLE);
    t5ui_color_t dark = t5ui_accent_color(badge_accent(a->id), T5UI_TONE_DARK);
    t5ui_color_t base = t5ui_theme_surface(SHELL_ELEV_CARD);
    t5ui_set_color(b, T5UI_P_BG_COLOR, t5ui_mix(dark, base, BADGE_TINT_OPA));
    t5ui_set_int(b, T5UI_P_BG_OPA, T5UI_OPA_COVER);
    t5ui_set_color(b, T5UI_P_TEXT_COLOR, dark);
    t5ui_set_int(b, T5UI_P_CLICKABLE, false);
    /* An installed app's own icon covers the disc; the disc still shows
     * through any transparent parts, so a logo on a clear background sits
     * on the same tinted circle as the glyphs. */
    t5ui_obj_t *i = a->builtin ? NULL : shell_sys_app_icon(b, a->id, t5ui_dp(size_dp));
    if (!i) i = shell_icon(b, a->icon[0] ? a->icon : T5UI_ICON_EXTENSION, icon_dp);
    t5ui_set_int(i, T5UI_P_ALIGN, T5UI_ALIGN_CENTER);
    if (a->blocked) {
        t5ui_obj_t *w = shell_icon(b, T5UI_ICON_WARNING, 18);
        t5ui_set_color(w, T5UI_P_TEXT_COLOR, t5ui_theme_color(T5UI_COLOR_ERROR));
        t5ui_set_int(w, T5UI_P_ALIGN, T5UI_ALIGN_BOTTOM_RIGHT);
        t5ui_set_int(w, T5UI_P_X, t5ui_dp(4));
        t5ui_set_int(w, T5UI_P_Y, t5ui_dp(4));
    }
    return b;
}

/* Every icon carries its app id as user data (pointing into s_apps or
 * s_dock_ids, both stable until the next rebuild). */
static void icon_clicked(t5ui_event_t *e)
{
    shell_sys_launch((const char *)t5ui_event_user(e), NULL);
}

static void icon_long(t5ui_event_t *e)
{
    const shell_app_t *a = find((const char *)t5ui_event_user(e));
    t5ui_cmd(t5ui_event_obj(e), T5UI_CMD_IGNORE_PRESS, 0, 0, 0, NULL);
    if (a) shell_app_menu(t5ui_event_obj(e), a->id, shell_app_name(a));
}

static void make_tappable(t5ui_obj_t *o, const char *id)
{
    t5ui_set_int(o, T5UI_P_CLICKABLE, true);
    t5ui_set_int(o, T5UI_P_RIPPLE, true);
    t5ui_on(o, T5UI_EV_SHORT_CLICK, icon_clicked, (void *)id);
    t5ui_on(o, T5UI_EV_LONG_PRESS, icon_long, (void *)id);
}

/* An icon with its name under it (the drawer). */
static t5ui_obj_t *cell(t5ui_obj_t *parent, const shell_app_t *a, const char *id)
{
    t5ui_obj_t *c = shell_box(parent);
    shell_size(c, t5ui_dp(CELL_MIN_W_DP), t5ui_dp(CELL_H_DP));
    t5ui_set_int(c, T5UI_P_RADIUS, t5ui_dp(SHELL_SHAPE_MEDIUM_DP));
    t5ui_set_int(c, T5UI_P_PAD_TOP, t5ui_dp(8));
    t5ui_set_int(c, T5UI_P_PAD_ROW, t5ui_dp(8));
    shell_pad_hor(c, t5ui_dp(4));
    shell_flex(c, T5UI_COLUMN, T5UI_PLACE_START, T5UI_PLACE_CENTER, T5UI_PLACE_CENTER);
    t5ui_set_int(c, T5UI_P_SCROLLABLE, false);
    make_tappable(c, id);
    badge(c, a, BADGE_DP, 40);
    t5ui_obj_t *l = shell_text(c, shell_app_name(a), T5UI_TEXT_BODY2);
    t5ui_set_int(l, T5UI_P_LETTER_SPACE, 0);
    shell_size(l, T5UI_PCT(100), t5ui_dp(40));   /* up to two lines, then dots */
    t5ui_set_int(l, T5UI_P_TEXT_ALIGN, T5UI_TEXT_CENTER);
    t5ui_set_int(l, T5UI_P_LONG_MODE, T5UI_LONG_DOTS);
    return c;
}

/* Spread the drawer's cells over whole columns of the current width, so
 * the grid is even in both orientations. */
static void size_cells(void)
{
    if (!s_grid) return;
    int32_t w = width_of(s_scr);
    if (w <= 0) t5ui_screen_size(&w, NULL);
    w -= 2 * t5ui_dp(SHELL_MARGIN_DP);
    int32_t cols = w / t5ui_dp(CELL_MIN_W_DP);
    if (cols < 1) cols = 1;
    int32_t cw = w / cols;
    t5ui_obj_t *boxes[] = { s_grid, s_recent_box };
    for (size_t b = 0; b < 2; b++) {
        uint32_t n = t5ui_child_count(boxes[b]);
        for (uint32_t i = 0; i < n; i++) t5ui_set_int(t5ui_child(boxes[b], (int32_t)i), T5UI_P_WIDTH, cw);
    }
}

static void build_grid(void)
{
    t5ui_clean(s_grid);
    for (int i = 0; i < s_napps; i++) cell(s_grid, &s_apps[i], s_apps[i].id);
    size_cells();
}

static void build_recents(void)
{
    char ids[SHELL_RECENTS][T5_APP_ID_LEN];
    memset(ids, 0, sizeof ids);
    int n = shell_recents(ids, SHELL_RECENTS);
    memcpy(s_shown_recents, ids, sizeof s_shown_recents);
    t5ui_clean(s_recent_box);
    int shown = 0;
    for (int i = 0; i < n; i++) {
        const shell_app_t *a = find(ids[i]);
        if (a) {
            cell(s_recent_box, a, a->id);
            shown++;
        }
    }
    t5ui_set_int(s_recent_row, T5UI_P_HIDDEN, shown == 0);
    size_cells();
}

/* ---- dock ------------------------------------------------------------------------- */
static void dock_add(const shell_app_t *a, int *n)
{
    for (int i = 0; i < *n; i++) {
        if (!strcmp(s_dock_ids[i], a->id)) return;
    }
    snprintf(s_dock_ids[*n], sizeof s_dock_ids[*n], "%s", a->id);
    t5ui_obj_t *b = badge(s_dock, a, DOCK_BADGE_DP, 36);
    make_tappable(b, s_dock_ids[*n]);
    (*n)++;
}

static void build_dock(void)
{
    t5ui_clean(s_dock);
    memset(s_dock_ids, 0, sizeof s_dock_ids);
    int n = 0;
    for (size_t i = 0; i < sizeof DOCK_PREFS / sizeof DOCK_PREFS[0] && n < DOCK_N; i++) {
        const shell_app_t *a = find(DOCK_PREFS[i]);
        if (a) dock_add(a, &n);
    }
    for (int i = 0; i < s_napps && n < DOCK_N; i++) dock_add(&s_apps[i], &n);
    t5ui_set_int(s_dock, T5UI_P_HIDDEN, n == 0);
}

/* ---- drawer ----------------------------------------------------------------------- */
/* The slide away finished (one that was stopped doesn't end). */
static void drawer_anim_end(t5ui_event_t *e)
{
    if (t5ui_event_index(e) == T5UI_P_Y && !s_drawer_open) t5ui_set_int(s_drawer, T5UI_P_HIDDEN, true);
}

bool launcher_drawer_is_open(void)
{
    return s_drawer && s_drawer_open;
}

void launcher_drawer_open(void)
{
    if (!s_drawer || s_drawer_open) return;
    s_drawer_open = true;
    t5ui_animate_stop(s_drawer, T5UI_P_Y);
    t5ui_set_int(s_grid, T5UI_P_SCROLL_Y, 0);
    int32_t h = height_of(s_scr);
    t5ui_set_int(s_drawer, T5UI_P_Y, h);
    t5ui_set_int(s_drawer, T5UI_P_HIDDEN, false);
    t5ui_animate(s_drawer, T5UI_P_Y, h, 0, SHELL_DUR_DRAWER_IN_MS, 0, T5UI_EASE_DECELERATE);
}

void launcher_drawer_close(bool animate)
{
    if (!s_drawer || !s_drawer_open) return;
    s_drawer_open = false;
    t5ui_animate_stop(s_drawer, T5UI_P_Y);
    if (!animate) {
        t5ui_set_int(s_drawer, T5UI_P_HIDDEN, true);
        return;
    }
    t5ui_animate(s_drawer, T5UI_P_Y, t5ui_get_int(s_drawer, T5UI_P_Y, 0), height_of(s_scr), SHELL_DUR_DRAWER_OUT_MS,
                 0, SHELL_EASE_SHARP);
}

static void open_clicked(t5ui_event_t *e)
{
    (void)e;
    launcher_drawer_open();
}

static void close_clicked(t5ui_event_t *e)
{
    (void)e;
    launcher_drawer_close(true);
}

/* Pulling the list down past its top (the elastic overscroll) closes it. */
static void grid_released(t5ui_event_t *e)
{
    (void)e;
    if (t5ui_get_int(s_grid, T5UI_P_SCROLL_Y, 0) < -t5ui_dp(PULL_CLOSE_DP)) launcher_drawer_close(true);
}

/* The drawer's handle: a chevron, with a word under it on the home screen
 * so the swipe up is discoverable without being taught. */
static t5ui_obj_t *chevron(t5ui_obj_t *parent, const char *icon, const char *text, t5ui_event_cb_t cb)
{
    t5ui_obj_t *b = shell_box(parent);
    shell_size(b, T5UI_CONTENT, T5UI_CONTENT);
    t5ui_set_int(b, T5UI_P_MIN_WIDTH, t5ui_dp(64));
    t5ui_set_int(b, T5UI_P_MIN_HEIGHT, t5ui_dp(32));
    shell_pad_hor(b, t5ui_dp(16));
    shell_pad_ver(b, text ? t5ui_dp(4) : 0);
    t5ui_set_int(b, T5UI_P_RADIUS, t5ui_dp(16));
    shell_flex(b, T5UI_COLUMN, T5UI_PLACE_CENTER, T5UI_PLACE_CENTER, T5UI_PLACE_CENTER);
    t5ui_set_int(b, T5UI_P_SCROLLABLE, false);
    t5ui_set_int(b, T5UI_P_CLICKABLE, true);
    t5ui_set_int(b, T5UI_P_RIPPLE, true);
    t5ui_obj_t *i = shell_icon(b, icon, 24);
    t5ui_set_int(i, T5UI_P_TEXT_OPA, t5ui_theme_emphasis(T5UI_EMPHASIS_MEDIUM));
    t5ui_set_int(i, T5UI_P_CLICKABLE, false);
    if (text) {
        t5ui_obj_t *l = shell_text(b, text, T5UI_TEXT_CAPTION);
        t5ui_set_int(l, T5UI_P_TEXT_OPA, t5ui_theme_emphasis(T5UI_EMPHASIS_MEDIUM));
        t5ui_set_int(l, T5UI_P_LETTER_SPACE, 0);
        t5ui_set_int(l, T5UI_P_CLICKABLE, false);
    }
    t5ui_on(b, T5UI_EV_CLICK, cb, NULL);
    return b;
}

static t5ui_obj_t *s_headers[2];
static int s_nheaders;

static void drawer_header(t5ui_obj_t *parent, const char *text)
{
    t5ui_obj_t *l = t5ui_create(parent, T5UI_MD_SECTION);
    t5ui_set_str(l, T5UI_P_TEXT, text);
    if (s_nheaders < 2) s_headers[s_nheaders++] = l;
    t5ui_set_int(l, T5UI_P_PAD_LEFT, t5ui_dp(4));
    t5ui_set_int(l, T5UI_P_PAD_TOP, t5ui_dp(8));
}

static void build_drawer(void)
{
    s_drawer = shell_box(s_scr);
    shell_size(s_drawer, T5UI_PCT(100), T5UI_PCT(100));
    t5ui_set_color(s_drawer, T5UI_P_BG_COLOR, t5ui_theme_color(T5UI_COLOR_BACKGROUND));
    t5ui_set_int(s_drawer, T5UI_P_BG_OPA, T5UI_OPA_COVER);
    t5ui_set_int(s_drawer, T5UI_P_PAD_TOP, t5ui_dp(SHELL_STATUS_H_DP));
    shell_flex(s_drawer, T5UI_COLUMN, T5UI_PLACE_START, T5UI_PLACE_CENTER, T5UI_PLACE_CENTER);
    t5ui_set_int(s_drawer, T5UI_P_SCROLLABLE, false);
    t5ui_set_int(s_drawer, T5UI_P_CLICKABLE, true);   /* taps never fall through to home */
    t5ui_set_int(s_drawer, T5UI_P_HIDDEN, true);
    t5ui_on(s_drawer, T5UI_EV_ANIM_END, drawer_anim_end, NULL);

    chevron(s_drawer, T5UI_ICON_EXPAND_MORE, NULL, close_clicked);

    s_recent_row = shell_box(s_drawer);
    shell_size(s_recent_row, T5UI_PCT(100), T5UI_CONTENT);
    shell_pad_hor(s_recent_row, t5ui_dp(SHELL_MARGIN_DP));
    t5ui_set_int(s_recent_row, T5UI_P_FLOW, T5UI_COLUMN);
    t5ui_set_int(s_recent_row, T5UI_P_SCROLLABLE, false);
    /* Named groups, so the recent apps read as shortcuts rather than as
     * the list repeating itself. Both headers go when there are no
     * recents: the grid alone needs no title. */
    drawer_header(s_recent_row, "Recent");
    s_recent_box = shell_box(s_recent_row);
    shell_size(s_recent_box, T5UI_PCT(100), t5ui_dp(CELL_H_DP));
    t5ui_set_int(s_recent_box, T5UI_P_FLOW, T5UI_ROW);
    t5ui_set_int(s_recent_box, T5UI_P_SCROLLABLE, false);   /* one row: the extra ones are clipped */
    t5ui_set_int(s_recent_box, T5UI_P_CLIP_CORNER, false);
    drawer_header(s_recent_row, "All apps");
    t5ui_set_int(s_recent_row, T5UI_P_HIDDEN, true);

    s_grid = shell_box(s_drawer);
    t5ui_set_int(s_grid, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(s_grid, T5UI_P_GROW, 1);
    shell_pad_hor(s_grid, t5ui_dp(SHELL_MARGIN_DP));
    t5ui_set_int(s_grid, T5UI_P_PAD_BOTTOM, t5ui_dp(16));
    shell_flex(s_grid, T5UI_ROW_WRAP, T5UI_PLACE_START, T5UI_PLACE_START, T5UI_PLACE_START);
    t5ui_set_int(s_grid, T5UI_P_SCROLL_DIR, T5UI_DIR_VER);
    t5ui_set_int(s_grid, T5UI_P_SCROLL_ELASTIC, true);
    t5ui_set_int(s_grid, T5UI_P_SCROLLBAR, T5UI_SCROLLBAR_ACTIVE);
    t5ui_on(s_grid, T5UI_EV_RELEASE, grid_released, NULL);
}

/* ---- screen ---------------------------------------------------------------------------- */
void launcher_tick(void)
{
    if (!s_scr) return;
    shell_show_time(s_time, s_date);
    char ids[SHELL_RECENTS][T5_APP_ID_LEN];
    memset(ids, 0, sizeof ids);
    shell_recents(ids, SHELL_RECENTS);
    if (memcmp(ids, s_shown_recents, sizeof ids)) build_recents();
}

void launcher_reload(void)
{
    if (!s_scr) return;
    if (!s_apps) s_apps = calloc(T5_APPS_MAX, sizeof *s_apps);
    s_napps = s_apps ? shell_sys_apps(s_apps, T5_APPS_MAX) : 0;
    qsort(s_apps, (size_t)s_napps, sizeof *s_apps, shell_app_name_cmp);
    build_grid();
    build_recents();
    build_dock();
}

static void restart_clicked(t5ui_event_t *e)
{
    (void)e;
    shell_sys_restart();
}

/* Safe mode (plan 9.3): why, and the way out. */
static void safe_banner(t5ui_obj_t *parent, const char *why)
{
    t5ui_obj_t *c = t5ui_create(parent, T5UI_MD_CARD);
    shell_size(c, T5UI_PCT(100), T5UI_CONTENT);
    shell_flex(c, T5UI_ROW, T5UI_PLACE_START, T5UI_PLACE_CENTER, T5UI_PLACE_CENTER);
    t5ui_set_int(c, T5UI_P_PAD_ALL, t5ui_dp(12));
    t5ui_set_int(c, T5UI_P_PAD_COLUMN, t5ui_dp(12));
    t5ui_set_int(c, T5UI_P_SCROLLABLE, false);
    t5ui_obj_t *ic = shell_icon(c, T5UI_ICON_HEALING, 24);
    t5ui_set_color(ic, T5UI_P_TEXT_COLOR, t5ui_theme_color(T5UI_COLOR_ERROR));
    char text[200];
    snprintf(text, sizeof text, "Safe mode: installed apps are off. %s Remove or update the app in App Manager, then restart.", why);
    t5ui_obj_t *l = shell_text(c, text, T5UI_TEXT_BODY2);
    t5ui_set_int(l, T5UI_P_LONG_MODE, T5UI_LONG_WRAP);
    t5ui_set_int(l, T5UI_P_GROW, 1);
    t5ui_obj_t *b = t5ui_create(c, T5UI_MD_BUTTON_TEXT);
    t5ui_set_str(b, T5UI_P_TEXT, "Restart");
    t5ui_on(b, T5UI_EV_CLICK, restart_clicked, NULL);
}

static void scr_resized(t5ui_event_t *e)
{
    (void)e;
    size_cells();
}

/* The colours taken from the accent. Local styles don't follow an accent
 * change (t5ui_sys_set_accent), so the accent code calls this after it. */
void launcher_restyle(void)
{
    if (!s_scr) return;
    t5ui_color_t bg = t5ui_theme_color(T5UI_COLOR_BACKGROUND);
    t5ui_set_color(s_scr, T5UI_P_BG_COLOR, bg);
    t5ui_set_color(s_scr, T5UI_P_BG_GRAD_COLOR, t5ui_mix(t5ui_theme_color(T5UI_COLOR_PRIMARY_VARIANT), bg, 112));
    for (int i = 0; i < s_nheaders; i++) {
        t5ui_set_color(s_headers[i], T5UI_P_TEXT_COLOR, t5ui_theme_color(T5UI_COLOR_PRIMARY));
    }
}

t5ui_obj_t *launcher_get(void)
{
    if (s_scr) return s_scr;
    s_scr = t5ui_sys_screen_create();
    /* The wallpaper: plain background under the status bar and the clock,
     * warming into the primary colour behind the dock. Starting dark means
     * no hard edge where the opaque status bar meets it, and the colour
     * sits where the hands go. */
    t5ui_set_int(s_scr, T5UI_P_BG_GRAD_DIR, T5UI_GRAD_VER);
    t5ui_set_int(s_scr, T5UI_P_BG_MAIN_STOP, 64);
    t5ui_set_int(s_scr, T5UI_P_SCROLLABLE, false);
    t5ui_on(s_scr, T5UI_EV_RESIZE, scr_resized, NULL);

    t5ui_obj_t *home = shell_box(s_scr);
    shell_size(home, T5UI_PCT(100), T5UI_PCT(100));
    t5ui_set_int(home, T5UI_P_PAD_TOP, t5ui_dp(SHELL_STATUS_H_DP));
    shell_pad_hor(home, t5ui_dp(24));
    t5ui_set_int(home, T5UI_P_PAD_BOTTOM, t5ui_dp(16));
    t5ui_set_int(home, T5UI_P_PAD_ROW, t5ui_dp(8));
    shell_flex(home, T5UI_COLUMN, T5UI_PLACE_START, T5UI_PLACE_CENTER, T5UI_PLACE_CENTER);
    t5ui_set_int(home, T5UI_P_SCROLLABLE, false);

    t5ui_obj_t *glance = shell_box(home);
    shell_size(glance, T5UI_PCT(100), T5UI_CONTENT);
    t5ui_set_int(glance, T5UI_P_PAD_TOP, t5ui_dp(24));
    t5ui_set_int(glance, T5UI_P_FLOW, T5UI_COLUMN);
    t5ui_set_int(glance, T5UI_P_SCROLLABLE, false);
    s_time = shell_text(glance, "", T5UI_TEXT_HEADLINE4);
    t5ui_set_int(s_time, T5UI_P_LETTER_SPACE, -t5ui_dp(1));   /* display figures set tight */
    s_date = shell_text(glance, "", T5UI_TEXT_SUBTITLE1);
    t5ui_set_int(s_date, T5UI_P_TEXT_OPA, t5ui_theme_emphasis(T5UI_EMPHASIS_MEDIUM));

    char why[96];
    if (shell_sys_safe_mode(why, sizeof why)) safe_banner(home, why);

    t5ui_obj_t *space = shell_box(home);
    t5ui_set_int(space, T5UI_P_WIDTH, 1); /* px: flex grow sets the real width */
    t5ui_set_int(space, T5UI_P_GROW, 1);

    chevron(home, T5UI_ICON_EXPAND_LESS, "All apps", open_clicked);

    s_dock = shell_box(home);
    shell_size(s_dock, T5UI_PCT(100), T5UI_CONTENT);
    t5ui_set_int(s_dock, T5UI_P_MAX_WIDTH, t5ui_dp(440));
    t5ui_set_int(s_dock, T5UI_P_RADIUS, T5UI_RADIUS_CIRCLE);
    t5ui_set_color(s_dock, T5UI_P_BG_COLOR, t5ui_theme_color(T5UI_COLOR_ON_SURFACE));
    t5ui_set_int(s_dock, T5UI_P_BG_OPA, 20);   /* 8% */
    t5ui_set_int(s_dock, T5UI_P_PAD_ALL, t5ui_dp(10));
    shell_flex(s_dock, T5UI_ROW, T5UI_PLACE_SPACE_EVENLY, T5UI_PLACE_CENTER, T5UI_PLACE_CENTER);
    t5ui_set_int(s_dock, T5UI_P_SCROLLABLE, false);

    build_drawer();
    launcher_restyle();
    launcher_reload();
    launcher_tick();
    return s_scr;
}

void launcher_reset(void)
{
    s_scr = s_time = s_date = s_dock = NULL;
    s_nheaders = 0;
    s_drawer = s_recent_row = s_recent_box = s_grid = NULL;
    s_drawer_open = false;
    s_napps = 0;
    memset(s_shown_recents, 0, sizeof s_shown_recents);
}
