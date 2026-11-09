/* SPDX-License-Identifier: Apache-2.0
 * Shell core (plan 6.2, 6.3): the chrome that sits around every app, the
 * recents list, and the thread-safe entry points of t5_shell/shell.h.
 *
 * Layering: the status bar, the app and gesture bars (bars.c), quick
 * settings, menus and the keyboard live on lv_layer_top(), created in
 * system context so an exiting app never takes them with it. Every screen
 * is padded at the top by the status bar's height; an app's screen also by
 * its app bar's, and at the bottom by the gesture bar's strip, unless the
 * app asked for full screen. t5_shell_get_insets() reports the padding.
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
static bool s_appbar;                      /* the app wants its app bar */
static char s_title[T5_APP_NAME_LEN];      /* the manifest's title or name */
static lv_obj_t *s_app_screen;
/* What the bars take from the app's screen, in dp (any task reads it). */
static volatile int32_t s_inset_top, s_inset_bottom;
static shell_recents_t s_recents;
static lv_timer_t *s_tick;

bool shell_in_app(void)
{
    return s_cur[0] != 0;
}

void shell_set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text)) lv_label_set_text(label, text);
}

void shell_anim_y(void *obj, int32_t y)
{
    lv_obj_set_y(obj, y);
}

void shell_show_time(lv_obj_t *clock, lv_obj_t *date)
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

void shell_show_recents(lv_obj_t *anchor)
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
    md_menu_show(anchor, m->items, m->n + 1, recents_picked, m);
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
        md_snackbar_show(msg, NULL, NULL, NULL);
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
        md_dialog_show(title, text, "Remove", "Cancel", remove_answer, m);
        return;
    }
    free(m);
}

void shell_app_menu(lv_obj_t *anchor, const char *id, const char *name)
{
    app_menu_t *m = calloc(1, sizeof *m);
    if (!m) return;
    snprintf(m->id, sizeof m->id, "%s", id);
    snprintf(m->name, sizeof m->name, "%s", name);
    shell_app_t a;
    m->removable = app_find(id, &a) && shell_app_removable(&a);
    static const char *items[] = { "Open", "App info", "Remove" };
    md_menu_show(anchor, items, m->removable ? 3 : 2, app_menu_picked, m);
}

/* ---- chrome ------------------------------------------------------------------------ */
static void tick(lv_timer_t *t)
{
    (void)t;
    statusbar_update();
    launcher_tick();
    standby_tick();
}

static void app_screen_deleted(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != s_app_screen) return;
    s_app_screen = NULL;
    bars_appbar_delete();    /* with any actions the app added: before its code goes */
}

bool shell_fullscreen(void)
{
    return s_cur[0] && s_fullscreen;
}

static void apply_inset(void)
{
    bool app = s_app_screen != NULL;
    bool bars = app && !s_fullscreen;
    int32_t top = s_fullscreen ? 0 : SHELL_STATUS_H_DP + (bars && s_appbar ? SHELL_APP_BAR_H_DP : 0);
    int32_t bottom = bars ? SHELL_GESTURE_H_DP : 0;
    if (app) {
        lv_obj_set_style_pad_top(s_app_screen, T5_DP(top), 0);
        lv_obj_set_style_pad_bottom(s_app_screen, T5_DP(bottom), 0);
    }
    statusbar_set_hidden(s_fullscreen);
    bars_appbar_show(bars && s_appbar);
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
    s_tick = lv_timer_create(tick, 1000, NULL);
    shell_sys_leave(saved);
    s_ready = true;
    statusbar_update();
}

lv_obj_t *shell_home_screen(void)
{
    lv_obj_t *home = launcher_get();
    shell_chrome_init();
    return home;
}

void shell_prepare(lv_obj_t *scr, const char *id)
{
    (void)id;
    shell_chrome_t c;
    shell_sys_chrome(&c);
    s_app_screen = scr;
    s_fullscreen = c.fullscreen;
    s_appbar = c.app_bar;
    snprintf(s_title, sizeof s_title, "%s", c.title);
    s_inset_top = s_inset_bottom = -1;      /* a new screen: always post its insets */
    lv_obj_add_event_cb(scr, app_screen_deleted, LV_EVENT_DELETE, NULL);
    bars_appbar_create(s_title);
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
        s_appbar = false;
        bars_appbar_delete();
        apply_inset();
        /* The app's screen is deleted straight after this, so there is no
         * cross-fade: fade the launcher in from black instead (150 ms). */
        lv_obj_fade_in(launcher_get(), MD_DUR_DIALOG_IN_MS, 0);
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

static void appbar_async(void *p)
{
    if (!s_cur[0]) return;
    s_appbar = p != NULL;
    apply_inset();
}

t5_err_t t5_shell_set_app_bar(bool shown)
{
    return on_ui(appbar_async, shown ? (void *)1 : NULL);
}

static void title_async(void *p)
{
    if (s_cur[0]) {
        if (p) snprintf(s_title, sizeof s_title, "%s", (const char *)p);
        else {
            shell_chrome_t c;
            shell_sys_chrome(&c);
            snprintf(s_title, sizeof s_title, "%s", c.title);
        }
        bars_appbar_title(s_title);
    }
    free(p);
}

t5_err_t t5_shell_set_title(const char *title)
{
    char *copy = NULL;
    if (title && !(copy = strdup(title))) return T5_ERR_NO_MEM;
    t5_err_t e = on_ui(title_async, copy);
    if (e != T5_OK) free(copy);
    return e;
}

struct _lv_obj_t *t5_shell_app_bar(void)
{
    return t5_ui_on_ui_task() && s_cur[0] ? bars_appbar() : NULL;
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
    md_snackbar_show(p, NULL, NULL, NULL);
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
    keyboard_show(ta);
}

void t5_shell_keyboard_hide(void)
{
    keyboard_hide();
}

void shell_reset(void)
{
    if (s_tick) lv_timer_delete(s_tick);
    s_tick = NULL;
    s_ready = false;
    s_cur[0] = 0;
    s_fullscreen = false;
    s_appbar = false;
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
