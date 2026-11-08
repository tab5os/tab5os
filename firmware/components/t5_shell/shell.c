/* SPDX-License-Identifier: Apache-2.0
 * Shell core (plan 6.2, 6.3): the chrome that sits around every app, the
 * recents list, and the thread-safe entry points of t5_shell/shell.h.
 *
 * Layering: the status bar, quick settings, menus and the keyboard live on
 * lv_layer_top(), created in system context so an exiting app never takes
 * them with it. Every screen (home and apps) is padded at the top by the
 * status bar's height unless the app asked for full screen.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shell_priv.h"
#include "t5_ui/t5_ui.h"

static bool s_ready;
static char s_cur[SHELL_ID_LEN];          /* "" = home */
static bool s_fullscreen;
static lv_obj_t *s_app_screen;
static char s_recent[SHELL_RECENTS][SHELL_ID_LEN];
static lv_timer_t *s_tick;

bool t5_shell_ready(void)
{
    return s_ready;
}

bool shell_in_app(void)
{
    return s_cur[0] != 0;
}

const char *shell_current_id(void)
{
    return s_cur;
}

/* ---- recents ------------------------------------------------------------------ */
void shell_note_launch(const char *id)
{
    int at = SHELL_RECENTS - 1;
    for (int i = 0; i < SHELL_RECENTS; i++) {
        if (!strcmp(s_recent[i], id)) {
            at = i;
            break;
        }
    }
    memmove(&s_recent[1], &s_recent[0], (size_t)at * SHELL_ID_LEN);
    strncpy(s_recent[0], id, SHELL_ID_LEN - 1);
    s_recent[0][SHELL_ID_LEN - 1] = 0;
}

int shell_recents(char ids[][SHELL_ID_LEN], int max)
{
    int n = 0;
    for (int i = 0; i < SHELL_RECENTS && n < max; i++) {
        if (s_recent[i][0]) memcpy(ids[n++], s_recent[i], SHELL_ID_LEN);
    }
    return n;
}

static bool app_name(const char *id, char *name, size_t n)
{
    shell_app_t *apps = calloc(48, sizeof *apps);
    if (!apps) return false;
    int k = shell_sys_apps(apps, 48);
    bool found = false;
    for (int i = 0; i < k; i++) {
        if (!strcmp(apps[i].id, id)) {
            strncpy(name, apps[i].name[0] ? apps[i].name : apps[i].id, n - 1);
            name[n - 1] = 0;
            found = true;
            break;
        }
    }
    free(apps);
    return found;
}

typedef struct {
    char ids[SHELL_RECENTS][SHELL_ID_LEN];
    char names[SHELL_RECENTS][SHELL_NAME_LEN];
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
    char ids[SHELL_RECENTS][SHELL_ID_LEN];
    int n = shell_recents(ids, SHELL_RECENTS);
    for (int i = 0; i < n; i++) {
        if (!strcmp(ids[i], s_cur)) continue;          /* the one in front */
        memcpy(m->ids[m->n], ids[i], SHELL_ID_LEN);
        if (!app_name(ids[i], m->names[m->n], SHELL_NAME_LEN)) continue;   /* removed since */
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
    char id[SHELL_ID_LEN];
    char name[SHELL_NAME_LEN];
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
        shell_sys_launch(SHELL_APP_MANAGER, m->id);
    } else if (item == 2 && m->removable) {
        char title[96];
        snprintf(title, sizeof title, "Remove %s?", m->name);
        md_dialog_show(title, "The app is removed. Its data in /data/appdata is kept.", "Remove", "Cancel",
                       remove_answer, m);
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
    m->removable = shell_sys_can_remove(id);
    static const char *items[] = { "Open", "App info", "Remove" };
    md_menu_show(anchor, items, m->removable ? 3 : 2, app_menu_picked, m);
}

/* ---- chrome --------------------------------------------------------------------- */
static void tick(lv_timer_t *t)
{
    (void)t;
    statusbar_update();
    launcher_tick();
}

static void app_screen_deleted(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) == s_app_screen) s_app_screen = NULL;
}

static void apply_inset(void)
{
    if (s_app_screen) lv_obj_set_style_pad_top(s_app_screen, s_fullscreen ? 0 : T5_DP(SHELL_STATUS_H_DP), 0);
    statusbar_set_hidden(s_fullscreen);
}

void shell_chrome_init(void)
{
    if (s_ready) return;
    shell_sys_apply_prefs();
    void *saved = shell_sys_enter();
    statusbar_create();
    keyboard_init();
    gesture_init();
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
    s_app_screen = scr;
    s_fullscreen = false;
    lv_obj_add_event_cb(scr, app_screen_deleted, LV_EVENT_DELETE, NULL);
    apply_inset();
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
        lv_obj_fade_in(launcher_get(), MD_DUR_DIALOG_IN_MS, 0);
    }
    statusbar_set_in_app(s_cur[0] != 0);
    statusbar_update();
    launcher_tick();
}

/* ---- thread-safe API -------------------------------------------------------------------- */
static void fullscreen_async(void *p)
{
    s_fullscreen = p != NULL;
    if (!s_cur[0]) s_fullscreen = false;     /* never on the home screen */
    apply_inset();
}

t5_err_t t5_shell_set_fullscreen(bool on)
{
    if (!s_ready) return T5_ERR_STATE;
    if (t5_ui_on_ui_task()) {
        fullscreen_async(on ? (void *)1 : NULL);
        return T5_OK;
    }
    return t5_ui_async(fullscreen_async, on ? (void *)1 : NULL);
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

t5_err_t t5_shell_home(void)
{
    shell_sys_home();
    return T5_OK;
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
    s_app_screen = NULL;
    memset(s_recent, 0, sizeof s_recent);
    launcher_reset();
    statusbar_reset();
    qs_reset();
    keyboard_reset();
    gesture_reset();
}

void shell_sim_reset(void)
{
    shell_reset();
}
