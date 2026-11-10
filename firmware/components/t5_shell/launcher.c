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

static lv_obj_t *s_scr, *s_time, *s_date, *s_dock;
static lv_obj_t *s_drawer, *s_recent_row, *s_recent_box, *s_grid;
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

static const md_accent_def_t *badge_accent(const char *id)
{
    static const md_accent_t pick[] = { MD_ACCENT_BLUE, MD_ACCENT_TEAL, MD_ACCENT_INDIGO, MD_ACCENT_GREEN,
                                        MD_ACCENT_ORANGE, MD_ACCENT_PINK, MD_ACCENT_CYAN, MD_ACCENT_PURPLE,
                                        MD_ACCENT_RED, MD_ACCENT_AMBER };
    uint32_t h = 2166136261u;
    for (const char *p = id; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return md_accent(pick[h % (sizeof pick / sizeof pick[0])]);
}

static const shell_app_t *find(const char *id)
{
    for (int i = 0; i < s_napps; i++) {
        if (!strcmp(s_apps[i].id, id)) return &s_apps[i];
    }
    return NULL;
}

static lv_obj_t *badge(lv_obj_t *parent, const shell_app_t *a, int size_dp, int icon_dp)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, T5_DP(size_dp), T5_DP(size_dp));
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    const md_accent_def_t *ac = badge_accent(a->id);
    md_rgb_t base = md_surface_at(md_scheme(), MD_ELEV_CARD);
    lv_obj_set_style_bg_color(b, md_color(md_mix(ac->dark, base, BADGE_TINT_OPA)), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(b, md_color(ac->dark), 0);
    lv_obj_set_clickable(b, false);
    /* An installed app's own icon covers the disc; the disc still shows
     * through any transparent parts, so a logo on a clear background sits
     * on the same tinted circle as the glyphs. */
    lv_obj_t *i = a->builtin ? NULL : shell_sys_app_icon(b, a->id, T5_DP(size_dp));
    if (!i) i = md_icon_create(b, a->icon[0] ? a->icon : MD_ICON_EXTENSION, icon_dp);
    lv_obj_center(i);
    if (a->blocked) {
        lv_obj_t *w = md_icon_create(b, MD_ICON_WARNING, 18);
        lv_obj_set_style_text_color(w, md_color(md_scheme()->c.error), 0);
        lv_obj_align(w, LV_ALIGN_BOTTOM_RIGHT, T5_DP(4), T5_DP(4));
    }
    return b;
}

/* Every icon carries its app id as user data (pointing into s_apps or
 * s_dock_ids, both stable until the next rebuild). */
static void icon_clicked(lv_event_t *e)
{
    shell_sys_launch((const char *)lv_event_get_user_data(e), NULL);
}

static void icon_long(lv_event_t *e)
{
    const shell_app_t *a = find((const char *)lv_event_get_user_data(e));
    lv_indev_wait_release(lv_indev_active());
    if (a) shell_app_menu(lv_event_get_current_target_obj(e), a->id, shell_app_name(a));
}

static void make_tappable(lv_obj_t *o, const char *id)
{
    lv_obj_set_clickable(o, true);
    md_ripple_attach(o);
    lv_obj_add_event_cb(o, icon_clicked, LV_EVENT_SHORT_CLICKED, (void *)id);
    lv_obj_add_event_cb(o, icon_long, LV_EVENT_LONG_PRESSED, (void *)id);
}

/* An icon with its name under it (the drawer). */
static lv_obj_t *cell(lv_obj_t *parent, const shell_app_t *a, const char *id)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, T5_DP(CELL_MIN_W_DP), T5_DP(CELL_H_DP));
    lv_obj_set_style_radius(c, T5_DP(MD_SHAPE_MEDIUM_DP), 0);
    lv_obj_set_style_pad_top(c, T5_DP(8), 0);
    lv_obj_set_style_pad_row(c, T5_DP(8), 0);
    lv_obj_set_style_pad_hor(c, T5_DP(4), 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(c, false);
    make_tappable(c, id);
    badge(c, a, BADGE_DP, 40);
    lv_obj_t *l = md_label_create(c, shell_app_name(a), MD_TYPE_BODY2);
    lv_obj_set_style_text_letter_space(l, 0, 0);
    lv_obj_set_size(l, LV_PCT(100), T5_DP(40));   /* up to two lines, then dots */
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    return c;
}

/* Spread the drawer's cells over whole columns of the current width, so
 * the grid is even in both orientations. */
static void size_cells(void)
{
    if (!s_grid) return;
    int32_t w = lv_obj_get_width(s_scr);
    if (w <= 0) w = lv_display_get_horizontal_resolution(lv_obj_get_display(s_scr));
    w -= 2 * T5_DP(MD_MARGIN_DP);
    int32_t cols = w / T5_DP(CELL_MIN_W_DP);
    if (cols < 1) cols = 1;
    int32_t cw = w / cols;
    lv_obj_t *boxes[] = { s_grid, s_recent_box };
    for (size_t b = 0; b < 2; b++) {
        uint32_t n = lv_obj_get_child_count(boxes[b]);
        for (uint32_t i = 0; i < n; i++) lv_obj_set_width(lv_obj_get_child(boxes[b], (int32_t)i), cw);
    }
}

static void build_grid(void)
{
    lv_obj_clean(s_grid);
    for (int i = 0; i < s_napps; i++) cell(s_grid, &s_apps[i], s_apps[i].id);
    size_cells();
}

static void build_recents(void)
{
    char ids[SHELL_RECENTS][T5_APP_ID_LEN];
    memset(ids, 0, sizeof ids);
    int n = shell_recents(ids, SHELL_RECENTS);
    memcpy(s_shown_recents, ids, sizeof s_shown_recents);
    lv_obj_clean(s_recent_box);
    int shown = 0;
    for (int i = 0; i < n; i++) {
        const shell_app_t *a = find(ids[i]);
        if (a) {
            cell(s_recent_box, a, a->id);
            shown++;
        }
    }
    lv_obj_set_hidden(s_recent_row, shown == 0);
    size_cells();
}

/* ---- dock ------------------------------------------------------------------------- */
static void dock_add(const shell_app_t *a, int *n)
{
    for (int i = 0; i < *n; i++) {
        if (!strcmp(s_dock_ids[i], a->id)) return;
    }
    snprintf(s_dock_ids[*n], sizeof s_dock_ids[*n], "%s", a->id);
    lv_obj_t *b = badge(s_dock, a, DOCK_BADGE_DP, 36);
    make_tappable(b, s_dock_ids[*n]);
    (*n)++;
}

static void build_dock(void)
{
    lv_obj_clean(s_dock);
    memset(s_dock_ids, 0, sizeof s_dock_ids);
    int n = 0;
    for (size_t i = 0; i < sizeof DOCK_PREFS / sizeof DOCK_PREFS[0] && n < DOCK_N; i++) {
        const shell_app_t *a = find(DOCK_PREFS[i]);
        if (a) dock_add(a, &n);
    }
    for (int i = 0; i < s_napps && n < DOCK_N; i++) dock_add(&s_apps[i], &n);
    lv_obj_set_hidden(s_dock, n == 0);
}

/* ---- drawer ----------------------------------------------------------------------- */
static void drawer_closed(lv_anim_t *a)
{
    lv_obj_set_hidden(a->var, true);
}

bool launcher_drawer_is_open(void)
{
    return s_drawer && s_drawer_open;
}

void launcher_drawer_open(void)
{
    if (!s_drawer || s_drawer_open) return;
    s_drawer_open = true;
    lv_anim_delete(s_drawer, shell_anim_y);
    lv_obj_scroll_to_y(s_grid, 0, LV_ANIM_OFF);
    int32_t h = lv_obj_get_height(s_scr);
    lv_obj_set_y(s_drawer, h);
    lv_obj_set_hidden(s_drawer, false);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_drawer);
    lv_anim_set_exec_cb(&a, shell_anim_y);
    lv_anim_set_values(&a, h, 0);
    lv_anim_set_duration(&a, MD_DUR_DRAWER_IN_MS);
    md_anim_set_curve(&a, MD_CURVE_DECELERATION);
    lv_anim_start(&a);
}

void launcher_drawer_close(bool animate)
{
    if (!s_drawer || !s_drawer_open) return;
    s_drawer_open = false;
    lv_anim_delete(s_drawer, shell_anim_y);
    if (!animate) {
        lv_obj_set_hidden(s_drawer, true);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_drawer);
    lv_anim_set_exec_cb(&a, shell_anim_y);
    lv_anim_set_values(&a, lv_obj_get_y(s_drawer), lv_obj_get_height(s_scr));
    lv_anim_set_duration(&a, MD_DUR_DRAWER_OUT_MS);
    md_anim_set_curve(&a, MD_CURVE_SHARP);
    lv_anim_set_completed_cb(&a, drawer_closed);
    lv_anim_start(&a);
}

static void open_clicked(lv_event_t *e)
{
    (void)e;
    launcher_drawer_open();
}

static void close_clicked(lv_event_t *e)
{
    (void)e;
    launcher_drawer_close(true);
}

/* Pulling the list down past its top (the elastic overscroll) closes it. */
static void grid_released(lv_event_t *e)
{
    (void)e;
    if (lv_obj_get_scroll_y(s_grid) < -T5_DP(PULL_CLOSE_DP)) launcher_drawer_close(true);
}

/* The drawer's handle: a chevron, with a word under it on the home screen
 * so the swipe up is discoverable without being taught. */
static lv_obj_t *chevron(lv_obj_t *parent, const char *icon, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_min_width(b, T5_DP(64), 0);
    lv_obj_set_style_min_height(b, T5_DP(32), 0);
    lv_obj_set_style_pad_hor(b, T5_DP(16), 0);
    lv_obj_set_style_pad_ver(b, text ? T5_DP(4) : 0, 0);
    lv_obj_set_style_radius(b, T5_DP(16), 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(b, false);
    lv_obj_set_clickable(b, true);
    md_ripple_attach(b);
    lv_obj_t *i = md_icon_create(b, icon, 24);
    lv_obj_set_style_text_opa(i, md_scheme()->on_surface.medium, 0);
    lv_obj_set_clickable(i, false);
    if (text) {
        lv_obj_t *l = md_label_create(b, text, MD_TYPE_CAPTION);
        lv_obj_set_style_text_opa(l, md_scheme()->on_surface.medium, 0);
        lv_obj_set_style_text_letter_space(l, 0, 0);
        lv_obj_set_clickable(l, false);
    }
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static lv_obj_t *s_headers[2];
static int s_nheaders;

static void drawer_header(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = md_section_create(parent, text);
    if (s_nheaders < 2) s_headers[s_nheaders++] = l;
    lv_obj_set_style_pad_left(l, T5_DP(4), 0);
    lv_obj_set_style_pad_top(l, T5_DP(8), 0);
}

static void build_drawer(void)
{
    const md_scheme_t *sc = md_scheme();
    s_drawer = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_drawer);
    lv_obj_set_size(s_drawer, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_drawer, md_color(sc->c.background), 0);
    lv_obj_set_style_bg_opa(s_drawer, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_top(s_drawer, T5_DP(SHELL_STATUS_H_DP), 0);
    lv_obj_set_flex_flow(s_drawer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_drawer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(s_drawer, false);
    lv_obj_set_clickable(s_drawer, true);   /* taps never fall through to home */
    lv_obj_set_hidden(s_drawer, true);

    chevron(s_drawer, MD_ICON_EXPAND_MORE, NULL, close_clicked);

    s_recent_row = lv_obj_create(s_drawer);
    lv_obj_remove_style_all(s_recent_row);
    lv_obj_set_size(s_recent_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(s_recent_row, T5_DP(MD_MARGIN_DP), 0);
    lv_obj_set_flex_flow(s_recent_row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(s_recent_row, false);
    /* Named groups, so the recent apps read as shortcuts rather than as
     * the list repeating itself. Both headers go when there are no
     * recents: the grid alone needs no title. */
    drawer_header(s_recent_row, "Recent");
    s_recent_box = lv_obj_create(s_recent_row);
    lv_obj_remove_style_all(s_recent_box);
    lv_obj_set_size(s_recent_box, LV_PCT(100), T5_DP(CELL_H_DP));
    lv_obj_set_flex_flow(s_recent_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_scrollable(s_recent_box, false);   /* one row: the extra ones are clipped */
    lv_obj_set_style_clip_corner(s_recent_box, false, 0);
    drawer_header(s_recent_row, "All apps");
    lv_obj_set_hidden(s_recent_row, true);

    s_grid = lv_obj_create(s_drawer);
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_width(s_grid, LV_PCT(100));
    lv_obj_set_flex_grow(s_grid, 1);
    lv_obj_set_style_pad_hor(s_grid, T5_DP(MD_MARGIN_DP), 0);
    lv_obj_set_style_pad_bottom(s_grid, T5_DP(16), 0);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_grid, LV_DIR_VER);
    lv_obj_set_scroll_elastic(s_grid, true);
    lv_obj_set_scrollbar_mode(s_grid, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_add_event_cb(s_grid, grid_released, LV_EVENT_RELEASED, NULL);
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

static void restart_clicked(lv_event_t *e)
{
    (void)e;
    shell_sys_restart();
}

/* Safe mode (plan 9.3): why, and the way out. */
static void safe_banner(lv_obj_t *parent, const char *why)
{
    lv_obj_t *c = md_card_create(parent);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(c, T5_DP(12), 0);
    lv_obj_set_style_pad_column(c, T5_DP(12), 0);
    lv_obj_set_scrollable(c, false);
    lv_obj_t *ic = md_icon_create(c, MD_ICON_HEALING, 24);
    lv_obj_set_style_text_color(ic, md_color(md_scheme()->c.error), 0);
    char text[200];
    snprintf(text, sizeof text, "Safe mode: installed apps are off. %s Remove or update the app in App Manager, then restart.", why);
    lv_obj_t *l = md_label_create(c, text, MD_TYPE_BODY2);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_flex_grow(l, 1);
    lv_obj_t *b = md_button_create(c, "Restart", MD_BUTTON_TEXT);
    lv_obj_add_event_cb(b, restart_clicked, LV_EVENT_CLICKED, NULL);
}

static void scr_resized(lv_event_t *e)
{
    (void)e;
    size_cells();
}

/* The colours taken from the accent. Local styles don't follow
 * md_set_accent(), so the accent code calls this after changing it. */
void launcher_restyle(void)
{
    if (!s_scr) return;
    const md_scheme_t *sc = md_scheme();
    lv_obj_set_style_bg_color(s_scr, md_color(sc->c.background), 0);
    lv_obj_set_style_bg_grad_color(s_scr, md_color(md_mix(sc->c.primary_variant, sc->c.background, 112)), 0);
    for (int i = 0; i < s_nheaders; i++) lv_obj_set_style_text_color(s_headers[i], md_color(sc->c.primary), 0);
}

lv_obj_t *launcher_get(void)
{
    if (s_scr) return s_scr;
    const md_scheme_t *sc = md_scheme();
    s_scr = lv_obj_create(NULL);
    /* The wallpaper: plain background under the status bar and the clock,
     * warming into the primary colour behind the dock. Starting dark means
     * no hard edge where the opaque status bar meets it, and the colour
     * sits where the hands go. */
    lv_obj_set_style_bg_grad_dir(s_scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(s_scr, 64, 0);
    lv_obj_set_scrollable(s_scr, false);
    lv_obj_add_event_cb(s_scr, scr_resized, LV_EVENT_SIZE_CHANGED, NULL);

    lv_obj_t *home = lv_obj_create(s_scr);
    lv_obj_remove_style_all(home);
    lv_obj_set_size(home, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_top(home, T5_DP(SHELL_STATUS_H_DP), 0);
    lv_obj_set_style_pad_hor(home, T5_DP(24), 0);
    lv_obj_set_style_pad_bottom(home, T5_DP(16), 0);
    lv_obj_set_style_pad_row(home, T5_DP(8), 0);
    lv_obj_set_flex_flow(home, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(home, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(home, false);

    lv_obj_t *glance = lv_obj_create(home);
    lv_obj_remove_style_all(glance);
    lv_obj_set_size(glance, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(glance, T5_DP(24), 0);
    lv_obj_set_flex_flow(glance, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(glance, false);
    s_time = md_label_create(glance, "", MD_TYPE_HEADLINE4);
    lv_obj_set_style_text_letter_space(s_time, -T5_DP(1), 0);   /* display figures set tight */
    s_date = md_label_create(glance, "", MD_TYPE_SUBTITLE1);
    lv_obj_set_style_text_opa(s_date, sc->on_surface.medium, 0);

    char why[96];
    if (shell_sys_safe_mode(why, sizeof why)) safe_banner(home, why);

    lv_obj_t *space = lv_obj_create(home);
    lv_obj_remove_style_all(space);
    lv_obj_set_width(space, 1); /* px: flex grow sets the real width */
    lv_obj_set_flex_grow(space, 1);

    chevron(home, MD_ICON_EXPAND_LESS, "All apps", open_clicked);

    s_dock = lv_obj_create(home);
    lv_obj_remove_style_all(s_dock);
    lv_obj_set_size(s_dock, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(s_dock, T5_DP(440), 0);
    lv_obj_set_style_radius(s_dock, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_dock, md_color(sc->c.on_surface), 0);
    lv_obj_set_style_bg_opa(s_dock, 20, 0);   /* 8% */
    lv_obj_set_style_pad_all(s_dock, T5_DP(10), 0);
    lv_obj_set_flex_flow(s_dock, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_dock, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(s_dock, false);

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
