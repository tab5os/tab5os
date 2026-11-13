/* SPDX-License-Identifier: Apache-2.0
 * Shell core (plan 6.2, 6.3): the chrome that sits around every app, the
 * recents list, and the thread-safe entry points of t5_shell/shell.h.
 *
 * Layering: the status bar, the app and gesture bars (bars.c), quick
 * settings, menus and the keyboard live on the top layer
 * (t5ui_sys_layer(T5UI_LAYER_TOP)), created in
 * system context so an exiting app never takes them with it. Every screen
 * is padded at the top by the status bar's height, and an app's screen at
 * the bottom by the gesture bar's strip, unless the app asked for full
 * screen. t5_shell_get_insets() reports the padding. There is no app bar:
 * apps draw their own if they want one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shell_priv.h"
#include "t5_shell/standby.h"
#include "t5_ui/t5_ui.h"

static bool s_ready;
static char s_cur[T5_APP_ID_LEN];          /* "" = home */
static bool s_fullscreen;
static t5ui_obj_t *s_app_screen;
/* What the bars take from the app's screen, in dp (any task reads it). */
static volatile int32_t s_inset_top, s_inset_bottom;
static shell_recents_t s_recents;
static t5ui_timer_t *s_tick;

bool shell_in_app(void)
{
    return s_cur[0] != 0;
}

void shell_set_text(t5ui_obj_t *label, const char *text)
{
    const char *now = t5ui_get_str(label, T5UI_P_TEXT, NULL);
    if (!now || strcmp(now, text)) t5ui_set_str(label, T5UI_P_TEXT, text);
}

t5ui_obj_t *shell_box(t5ui_obj_t *parent)
{
    return t5ui_create(parent, T5UI_BOX);
}

t5ui_obj_t *shell_text(t5ui_obj_t *parent, const char *text, t5ui_text_t style)
{
    t5ui_obj_t *l = t5ui_create(parent, T5UI_MD_TEXT);
    t5ui_set_int(l, T5UI_P_TEXT_STYLE, style);
    t5ui_set_str(l, T5UI_P_TEXT, text ? text : "");
    return l;
}

t5ui_obj_t *shell_icon(t5ui_obj_t *parent, const char *glyph, int32_t size_dp)
{
    t5ui_obj_t *i = t5ui_create(parent, T5UI_MD_ICON);
    t5ui_set_int(i, T5UI_P_ICON_SIZE, size_dp);
    t5ui_set_str(i, T5UI_P_ICON, glyph);
    return i;
}

void shell_flex(t5ui_obj_t *o, t5ui_flow_t flow, t5ui_place_t main, t5ui_place_t cross, t5ui_place_t track)
{
    t5ui_set_int(o, T5UI_P_FLOW, flow);
    t5ui_set_int(o, T5UI_P_PLACE_MAIN, main);
    t5ui_set_int(o, T5UI_P_PLACE_CROSS, cross);
    t5ui_set_int(o, T5UI_P_PLACE_TRACK, track);
}

void shell_pad_hor(t5ui_obj_t *o, int32_t px)
{
    t5ui_set_int(o, T5UI_P_PAD_LEFT, px);
    t5ui_set_int(o, T5UI_P_PAD_RIGHT, px);
}

void shell_pad_ver(t5ui_obj_t *o, int32_t px)
{
    t5ui_set_int(o, T5UI_P_PAD_TOP, px);
    t5ui_set_int(o, T5UI_P_PAD_BOTTOM, px);
}

void shell_size(t5ui_obj_t *o, int32_t w, int32_t h)
{
    t5ui_set_int(o, T5UI_P_WIDTH, w);
    t5ui_set_int(o, T5UI_P_HEIGHT, h);
}

void shell_elevate(t5ui_obj_t *o, int32_t dp)
{
    if (!t5ui_theme_dark()) {
        t5ui_set_int(o, T5UI_P_ELEVATION, dp);   /* the surface and its shadow */
        return;
    }
    t5ui_set_color(o, T5UI_P_BG_COLOR, t5ui_theme_surface(dp));
    t5ui_set_int(o, T5UI_P_BG_OPA, T5UI_OPA_COVER);
}

void shell_show_time(t5ui_obj_t *clock, t5ui_obj_t *date)
{
    struct tm tm;
    char buf[48];
    bool set = shell_sys_time(&tm);
    if (clock) {
        if (set) shell_fmt_clock(&tm, buf, sizeof buf);
        shell_set_text(clock, set ? buf : SHELL_CLOCK_NOT_SET);
    }
    if (date) {
        if (set) shell_fmt_date(&tm, buf, sizeof buf);
        shell_set_text(date, set ? buf : SHELL_DATE_NOT_SET);
    }
}

void shell_note_launch(const char *id)
{
    shell_recents_note(&s_recents, id);
}

int shell_recents(char ids[][T5_APP_ID_LEN], int max)
{
    return shell_recents_list(&s_recents, ids, max);
}

/* The registry's entry for id. */
static bool app_find(const char *id, shell_app_t *out)
{
    shell_app_t *apps = calloc(T5_APPS_MAX, sizeof *apps);
    if (!apps) return false;
    int k = shell_sys_apps(apps, T5_APPS_MAX);
    bool found = false;
    for (int i = 0; i < k && !found; i++) {
        if (!strcmp(apps[i].id, id)) {
            *out = apps[i];
            found = true;
        }
    }
    free(apps);
    return found;
}

typedef struct {
    char ids[SHELL_RECENTS][T5_APP_ID_LEN];
    char names[SHELL_RECENTS][T5_APP_NAME_LEN];
    const char *items[SHELL_RECENTS + 1];
    int n;
} recents_menu_t;

static recents_menu_t *s_rm;

static void recents_picked(int item, void *ud)
{
    recents_menu_t *m = ud;
    if (item >= 0 && item < m->n) shell_sys_launch(m->ids[item], NULL);
    else if (item == m->n) shell_sys_home();
    if (s_rm == m) s_rm = NULL;
    free(m);
}

void shell_show_recents(t5ui_obj_t *anchor)
{
    recents_menu_t *m = calloc(1, sizeof *m);
    if (!m) return;
    char ids[SHELL_RECENTS][T5_APP_ID_LEN];
    int n = shell_recents(ids, SHELL_RECENTS);
    for (int i = 0; i < n; i++) {
        shell_app_t a;
        if (!strcmp(ids[i], s_cur)) continue;          /* the one in front */
        if (!app_find(ids[i], &a)) continue;           /* removed since */
        memcpy(m->ids[m->n], ids[i], T5_APP_ID_LEN);
        snprintf(m->names[m->n], sizeof m->names[m->n], "%s", shell_app_name(&a));
        m->items[m->n] = m->names[m->n];
        m->n++;
    }
    m->items[m->n] = "Home";
    void *saved = shell_sys_enter();
    t5ui_menu(anchor, m->items, m->n + 1, recents_picked, m);
    shell_sys_leave(saved);
}

/* ---- per-app menu (long press in the launcher) ----------------------------------- */
typedef struct {
    char id[T5_APP_ID_LEN];
    char name[T5_APP_NAME_LEN];
    bool removable;
} app_menu_t;

static void remove_answer(int button, void *ud)
{
    app_menu_t *m = ud;
    if (button == 0) {
        shell_sys_remove(m->id);
        char msg[96];
        snprintf(msg, sizeof msg, "%s removed", m->name);
        t5ui_snackbar(msg, NULL, NULL, NULL);
    }
    free(m);
}

static void app_menu_picked(int item, void *ud)
{
    app_menu_t *m = ud;
    if (item == 0) {
        shell_sys_launch(m->id, NULL);
    } else if (item == 1) {
        shell_sys_launch(T5_APP_ID_MANAGER, m->id);
    } else if (item == 2 && m->removable) {
        char title[96], dir[T5_APP_PATH_MAX], text[T5_APP_PATH_MAX + 48];
        snprintf(title, sizeof title, "Remove %s?", m->name);
        shell_sys_data_dir(m->id, dir, sizeof dir);
        snprintf(text, sizeof text, "The app is removed. Its data in %s is kept.", dir);
        t5ui_dialog(title, text, "Remove", "Cancel", remove_answer, m);
        return;
    }
    free(m);
}

void shell_app_menu(t5ui_obj_t *anchor, const char *id, const char *name)
{
    app_menu_t *m = calloc(1, sizeof *m);
    if (!m) return;
    snprintf(m->id, sizeof m->id, "%s", id);
    snprintf(m->name, sizeof m->name, "%s", name);
    shell_app_t a;
    m->removable = app_find(id, &a) && shell_app_removable(&a);
    static const char *items[] = { "Open", "App info", "Remove" };
    t5ui_menu(anchor, items, m->removable ? 3 : 2, app_menu_picked, m);
}

/* ---- chrome ------------------------------------------------------------------------ */
static void tick(t5ui_timer_t *t, void *user)
{
    (void)t;
    (void)user;
    statusbar_update();
    launcher_tick();
    standby_tick();
}

static void app_screen_deleted(t5ui_event_t *e)
{
    if (t5ui_event_target(e) != s_app_screen) return;
    s_app_screen = NULL;
}

bool shell_fullscreen(void)
{
    return s_cur[0] && s_fullscreen;
}

static void apply_inset(void)
{
    bool app = s_app_screen != NULL;
    bool bars = app && !s_fullscreen;
    int32_t top = s_fullscreen ? 0 : SHELL_STATUS_H_DP;
    int32_t bottom = bars ? SHELL_GESTURE_H_DP : 0;
    if (app) {
        t5ui_set_int(s_app_screen, T5UI_P_PAD_TOP, t5ui_dp(top));
        t5ui_set_int(s_app_screen, T5UI_P_PAD_BOTTOM, t5ui_dp(bottom));
    }
    statusbar_set_hidden(s_fullscreen);
    bars_handle_show(bars);
    bool changed = app && (top != s_inset_top || bottom != s_inset_bottom);
    s_inset_top = app ? top : 0;
    s_inset_bottom = app ? bottom : 0;
    if (changed) shell_sys_insets_changed();
}

void shell_chrome_init(void)
{
    if (s_ready) return;
    shell_sys_apply_prefs();
    void *saved = shell_sys_enter();
    statusbar_create();
    gesture_attach_all();
    s_tick = t5ui_timer(1000, tick, NULL);
    shell_sys_leave(saved);
    s_ready = true;
    statusbar_update();
}

t5ui_obj_t *shell_home_screen(void)
{
    t5ui_obj_t *home = launcher_get();
    shell_chrome_init();
    return home;
}

void shell_prepare(t5ui_obj_t *scr, const char *id)
{
    (void)id;
    shell_chrome_t c;
    shell_sys_chrome(&c);
    s_app_screen = scr;
    s_fullscreen = c.fullscreen;
    s_inset_top = s_inset_bottom = -1;      /* a new screen: always post its insets */
    t5ui_on(scr, T5UI_EV_DELETE, app_screen_deleted, NULL);
    apply_inset();
    if (s_fullscreen) bars_handle_peek();
}

void shell_changed(const char *id)
{
    keyboard_hide();
    qs_close();
    launcher_drawer_close(false);
    snprintf(s_cur, sizeof s_cur, "%s", id ? id : "");
    if (s_cur[0]) {
        shell_note_launch(s_cur);
    } else {
        s_app_screen = NULL;
        s_fullscreen = false;
        apply_inset();
        /* The app's screen is deleted straight after this, so there is no
         * cross-fade: fade the launcher in from black instead (150 ms). */
        t5ui_animate(launcher_get(), T5UI_P_OPA, T5UI_OPA_TRANSP, T5UI_OPA_COVER, SHELL_DUR_DIALOG_IN_MS, 0,
                     T5UI_EASE_LINEAR);
    }
    statusbar_set_in_app(s_cur[0] != 0);
    statusbar_update();
    launcher_tick();
}

/* ---- thread-safe API -------------------------------------------------------------------- */
static void fullscreen_async(void *p)
{
    bool was = s_fullscreen;
    s_fullscreen = p != NULL;
    if (!s_cur[0]) s_fullscreen = false;     /* never on the home screen */
    apply_inset();
    if (s_fullscreen && !was) bars_handle_peek();   /* show the way out once */
}

/* On the UI task now, or queued for it. */
static t5_err_t on_ui(t5_ui_fn_t fn, void *arg)
{
    if (!s_ready) return T5_ERR_STATE;
    if (t5_ui_on_ui_task()) {
        fn(arg);
        return T5_OK;
    }
    return t5_ui_async(fn, arg);
}

t5_err_t t5_shell_set_fullscreen(bool on)
{
    return on_ui(fullscreen_async, on ? (void *)1 : NULL);
}

bool t5_shell_fullscreen(void)
{
    return shell_fullscreen();
}

t5_err_t t5_shell_get_insets(t5_shell_insets_t *out)
{
    if (!out) return T5_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    if (!s_cur[0]) return T5_ERR_STATE;
    out->top = s_inset_top < 0 ? 0 : s_inset_top;
    out->bottom = s_inset_bottom < 0 ? 0 : s_inset_bottom;
    return T5_OK;
}

static void toast_async(void *p)
{
    void *saved = shell_sys_enter();
    t5ui_snackbar(p, NULL, NULL, NULL);
    shell_sys_leave(saved);
    free(p);
}

t5_err_t t5_shell_toast(const char *text)
{
    if (!s_ready || !text) return T5_ERR_STATE;
    char *copy = strdup(text);
    if (!copy) return T5_ERR_NO_MEM;
    if (t5_ui_on_ui_task()) {
        toast_async(copy);
        return T5_OK;
    }
    t5_err_t e = t5_ui_async(toast_async, copy);
    if (e != T5_OK) free(copy);
    return e;
}

static void qs_async(void *p)
{
    if (p) qs_open();
    else qs_close();
}

t5_err_t t5_shell_quick_settings(bool open)
{
    if (!s_ready) return T5_ERR_STATE;
    return t5_ui_async(qs_async, open ? (void *)1 : NULL);
}

static void home_async(void *p)
{
    (void)p;
    shell_sys_home();
}

t5_err_t t5_shell_home(void)
{
    if (t5_ui_on_ui_task()) {
        shell_sys_home();
        return T5_OK;
    }
    return t5_ui_async(home_async, NULL);
}

static void standby_async(void *p)
{
    if (p) standby_show();
    else standby_hide();
}

t5_err_t t5_shell_standby(bool on)
{
    return on_ui(standby_async, on ? (void *)1 : NULL);
}

bool t5_shell_standby_shown(void)
{
    return standby_shown();
}

void shell_input_added(void)
{
    if (!s_ready) return;            /* the chrome attaches every device when it is built */
    gesture_attach_all();
    if (t5_ui_hw_keyboard()) keyboard_hide();
    statusbar_update();
}

void t5_shell_keyboard_show(void *ta)
{
    keyboard_show(t5ui_sys_obj(ta));   /* the caller's text area, t5ui's or a native one */
}

void t5_shell_keyboard_hide(void)
{
    keyboard_hide();
}

void shell_reset(void)
{
    if (s_tick) t5ui_timer_delete(s_tick);
    s_tick = NULL;
    s_ready = false;
    s_cur[0] = 0;
    s_fullscreen = false;
    s_app_screen = NULL;
    s_inset_top = s_inset_bottom = 0;
    memset(&s_recents, 0, sizeof s_recents);
    bars_reset();
    standby_reset();
    launcher_reset();
    statusbar_reset();
    qs_reset();
    keyboard_reset();
    gesture_reset();
}
