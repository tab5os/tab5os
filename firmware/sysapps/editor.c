/* SPDX-License-Identifier: Apache-2.0
 * Editor (plan 8.4): write and run Python on the device.
 *
 * Scripts live in T5_USER_DIR/scripts (and /sd/scripts with a card in).
 * The editor page has the code view, a line and column readout and, once a
 * script has run, its output. Run saves first, then starts the script with
 * the Editor's permissions (ports, net, usb, storage) and its folder as the
 * working directory. A script that builds a UI with t5.ui.screen() covers
 * the Editor until it finishes or its back arrow is pressed; the output pane
 * keeps what it printed. Leaving the page saves.
 */
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "t5/shell.h"
#include "t5/storage.h"
#include "t5_gui/t5ui.h"
#include "t5_gui/t5ui_icons.h"
#include "t5_gui/t5ui_sys.h"   /* t5ui_sys_obj / _native: the Python console */
#include "t5_py/py.h"
#include "codeview.h"
#include "sysapps_priv.h"

#define SCRIPTS T5_USER_DIR "/scripts"
#define SD_SCRIPTS T5_SD_PATH "/scripts"
#define FILE_MAX (128 * 1024)

typedef struct {
    t5_app_t *app;
    char path[160];
    t5ui_obj_t *view, *out, *status, *run;
    cv_t *cv;
    t5_py_t *vm;
    volatile bool run_done;
    t5ui_timer_t *timer;
} ed_t;

static sa_nav_t s_nav;
static ed_t *s_ed;          /* the open file's state (one at a time) */
static t5ui_obj_t *s_newname;
static t5ui_obj_t *s_list;  /* the script list's body, rebuilt when it comes back to the front */
static bool s_list_stale;

static const char s_hello[] =
    "# Press the play button to run, the stop button to stop.\n"
    "# print() output appears below the code.\n"
    "import t5\n"
    "\n"
    "print(\"Hello from Python on the Tab5\")\n"
    "print(\"battery:\", t5.power.battery()[\"percent\"], \"%\")\n"
    "\n"
    "# Scan the Grove port (needs a sensor plugged in to find anything).\n"
    "p = t5.port.open(\"grove\", mode=\"i2c\", hz=100_000)\n"
    "print(\"I2C devices on Grove:\", [hex(a) for a in p.scan()])\n"
    "p.close()\n";

static const char s_ui_demo[] =
    "# A small UI: back (top left) ends the script.\n"
    "import t5\n"
    "\n"
    "s = t5.ui.screen(\"UI demo\")\n"
    "label = s.label(\"Tapped 0 times\", role=\"h5\")\n"
    "count = 0\n"
    "\n"
    "def tapped(button):\n"
    "    global count\n"
    "    count += 1\n"
    "    label.set_text(\"Tapped %d times\" % count)\n"
    "\n"
    "s.button(\"Tap me\", fn=tapped)\n"
    "chart = s.chart(points=60, min=0, max=100)\n"
    "\n"
    "while t5.app.running():\n"
    "    chart.push(t5.power.battery()[\"percent\"])\n"
    "    t5.sleep_ms(1000)\n";

static bool write_file(const char *path, const char *text)
{
    t5_err_t e = t5_storage_write_atomic(path, text, strlen(text));
    sa_report(e, "save the script");
    return e == T5_OK;
}

static void ensure_examples(void)
{
    mkdir(SCRIPTS, 0755);
    DIR *d = opendir(SCRIPTS);
    bool empty = true;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] != '.') empty = false;
        }
        closedir(d);
    }
    if (empty && write_file(SCRIPTS "/hello.py", s_hello)) write_file(SCRIPTS "/ui_demo.py", s_ui_demo);
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

#define FAB_DP 56                /* MD2's FAB */

/* A list row, as md made them: grows when its second line wraps. */
static t5ui_obj_t *row(t5ui_obj_t *parent, const char *icon, const char *primary, const char *secondary)
{
    t5ui_obj_t *r = t5ui_create(parent, T5UI_MD_ROW);
    if (icon) t5ui_set_str(r, T5UI_P_ICON, icon);
    t5ui_set_str(r, T5UI_P_TEXT, primary ? primary : "");
    if (secondary) {
        t5ui_set_str(r, T5UI_P_SECONDARY, secondary);
        t5ui_set_int(r, T5UI_P_HEIGHT, T5UI_CONTENT);
        t5ui_set_int(r, T5UI_P_MIN_HEIGHT, t5ui_dp(64));
    }
    return r;
}

/* An app bar action: an icon button. */
static t5ui_obj_t *action(t5ui_obj_t *bar, const char *icon, t5ui_event_cb_t cb, void *user)
{
    t5ui_obj_t *b = t5ui_create(bar, T5UI_MD_APPBAR_ACTION);
    t5ui_set_str(b, T5UI_P_ICON, icon);
    t5ui_on(b, T5UI_EV_CLICK, cb, user);
    return b;
}

static void update_title(ed_t *ed)
{
    char t[96];
    snprintf(t, sizeof t, "%s%s", base_name(ed->path), cv_dirty(ed->cv) ? " \xe2\x80\xa2" : "");
    t5ui_set_str(sa_bar(&s_nav), T5UI_P_TITLE, t);
}

static void changed(cv_t *cv, void *arg)
{
    ed_t *ed = arg;
    int line, col;
    cv_cursor(cv, &line, &col);
    char buf[48];
    snprintf(buf, sizeof buf, "Line %d, column %d, %d lines in all", line, col, cv_line_count(cv));
    sa_set_text(ed->status, buf);
    update_title(ed);
}

static bool save(ed_t *ed)
{
    if (!cv_dirty(ed->cv)) return true;
    size_t len;
    char *text = cv_text(ed->cv, &len);
    if (!text) return false;
    t5_err_t e = t5_storage_write_atomic(ed->path, text, len);
    free(text);
    if (e != T5_OK) {
        char msg[96];
        snprintf(msg, sizeof msg, "Couldn't save: %s", t5_err_str(e));
        t5ui_snackbar(msg, NULL, NULL, NULL);
        return false;
    }
    cv_set_clean(ed->cv);
    return true;
}

static void run_finished(t5_py_t *vm, t5_py_state_t how, void *arg)
{
    (void)vm;
    ed_t *ed = arg;
    const char *msg = how == T5_PY_EXCEPTION ? "\n[Stopped by an error]\n"
                    : how == T5_PY_STOPPED   ? "\n[Stopped]\n"
                                             : "\n[Finished]\n";
    t5_py_console_write(t5ui_sys_native(ed->out), msg, strlen(msg));
    ed->run_done = true;     /* the timer frees the VM on the UI task */
}

static void run_out(const char *s, size_t n, void *arg)
{
    t5_py_console_write(t5ui_sys_native(((ed_t *)arg)->out), s, n);
}

static void set_running(ed_t *ed, bool on)
{
    t5ui_set_str(ed->run, T5UI_P_ICON, on ? T5UI_ICON_STOP : T5UI_ICON_PLAY_ARROW);
}

static void stop_run(ed_t *ed)
{
    if (ed->vm) t5_py_stop(ed->vm, 1000);
    ed->vm = NULL;
    ed->run_done = false;
    set_running(ed, false);
}

static void run_clicked(t5ui_event_t *e)
{
    ed_t *ed = t5ui_event_user(e);
    if (ed->vm) {
        stop_run(ed);
        return;
    }
    if (!save(ed)) return;
    t5_shell_keyboard_hide();
    t5ui_set_int(ed->out, T5UI_P_HIDDEN, false);
    t5_py_console_clear(t5ui_sys_native(ed->out));
    char dir[160];
    snprintf(dir, sizeof dir, "%s", ed->path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    t5_py_cfg_t cfg = {
        .path = ed->path,
        .cwd = dir,
        .ui_parent = t5_app_screen(ed->app),
        .out = run_out,
        .done = run_finished,
        .arg = ed,
    };
    t5_err_t err = t5_py_start(&cfg, &ed->vm);
    if (err != T5_OK) {
        char m[96];
        snprintf(m, sizeof m, "Couldn't run the script (%s)", t5_err_str(err));
        t5ui_snackbar(m, NULL, NULL, NULL);
        return;
    }
    set_running(ed, true);
}

static void save_clicked(t5ui_event_t *e)
{
    ed_t *ed = t5ui_event_user(e);
    if (save(ed)) t5ui_snackbar("Saved", NULL, NULL, NULL);
}

static void keyboard_clicked(t5ui_event_t *e)
{
    ed_t *ed = t5ui_event_user(e);
    cv_focus(ed->cv);
}

static void tick(t5ui_timer_t *t, void *user)
{
    (void)t;
    ed_t *ed = user;
    if (ed->run_done) stop_run(ed);
}

static void ed_free(void *arg)
{
    ed_t *ed = arg;
    stop_run(ed);
    save(ed);
    if (ed->timer) t5ui_timer_delete(ed->timer);
    t5_shell_keyboard_hide();
    if (s_ed == ed) s_ed = NULL;
    free(ed);
}

static void open_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *text = NULL;
    size_t len = 0;
    if (f) {
        text = malloc(FILE_MAX + 1);
        if (text) len = fread(text, 1, FILE_MAX, f);
        fclose(f);
    }
    ed_t *ed = calloc(1, sizeof *ed);
    if (!ed) {
        free(text);
        return;
    }
    ed->app = s_nav.app;
    s_list_stale = true;     /* sizes change once it's saved */
    snprintf(ed->path, sizeof ed->path, "%s", path);
    t5ui_obj_t *body = sa_push(&s_nav, base_name(path));
    t5ui_set_int(body, T5UI_P_PAD_ALL, 0);
    t5ui_set_int(body, T5UI_P_PAD_ROW, 0);
    t5ui_set_int(body, T5UI_P_PAD_COLUMN, 0);
    t5ui_set_int(body, T5UI_P_SCROLLABLE, false);
    t5ui_set_int(body, T5UI_P_FLOW, T5UI_COLUMN);
    t5ui_obj_t *bar = sa_bar(&s_nav);
    ed->run = action(bar, T5UI_ICON_PLAY_ARROW, run_clicked, ed);
    action(bar, T5UI_ICON_SAVE, save_clicked, ed);
    action(bar, T5UI_ICON_KEYBOARD, keyboard_clicked, ed);
    ed->view = cv_create(body);
    t5ui_set_int(ed->view, T5UI_P_HEIGHT, 1);   /* px: flex-grow sets it */
    t5ui_set_int(ed->view, T5UI_P_GROW, 2);
    ed->cv = cv_get(ed->view);
    ed->status = t5ui_create(body, T5UI_MD_TEXT);
    t5ui_set_int(ed->status, T5UI_P_TEXT_STYLE, T5UI_TEXT_CAPTION);
    t5ui_set_int(ed->status, T5UI_P_PAD_LEFT, t5ui_dp(16));
    t5ui_set_int(ed->status, T5UI_P_PAD_RIGHT, t5ui_dp(16));
    t5ui_set_int(ed->status, T5UI_P_PAD_TOP, t5ui_dp(4));
    t5ui_set_int(ed->status, T5UI_P_PAD_BOTTOM, t5ui_dp(4));
    t5ui_set_int(ed->status, T5UI_P_TEXT_OPA, t5ui_theme_emphasis(T5UI_EMPHASIS_MEDIUM));
    ed->out = t5ui_sys_obj(t5_py_console_create(t5ui_sys_native(body)));
    t5ui_set_int(ed->out, T5UI_P_BORDER_SIDES, T5UI_SIDE_TOP);
    t5ui_set_int(ed->out, T5UI_P_BORDER_WIDTH, t5ui_dp(1));
    t5ui_set_color(ed->out, T5UI_P_BORDER_COLOR, t5ui_theme_color(T5UI_COLOR_ON_SURFACE));
    t5ui_set_int(ed->out, T5UI_P_BORDER_OPA, t5ui_theme_emphasis(T5UI_EMPHASIS_DIVIDER));
    t5ui_set_int(ed->out, T5UI_P_HEIGHT, 1);   /* px: flex-grow sets it */
    t5ui_set_int(ed->out, T5UI_P_GROW, 1);
    t5ui_set_int(ed->out, T5UI_P_HIDDEN, true);
    cv_set_text(ed->cv, text ? text : "", len);
    free(text);
    cv_set_changed_cb(ed->cv, changed, ed);
    changed(ed->cv, ed);
    ed->timer = t5ui_timer(100, tick, ed);
    s_ed = ed;
    sa_set_refresh(&s_nav, NULL, ed, ed_free);
}

static void row_clicked(t5ui_event_t *e)
{
    const char *path = t5ui_event_user(e);
    open_file(path);
}

static void free_path(t5ui_event_t *e)
{
    free(t5ui_event_user(e));
}

static void list_dir(t5ui_obj_t *body, const char *dir, const char *title)
{
    DIR *d = opendir(dir);
    if (!d) return;
    t5ui_set_str(t5ui_create(body, T5UI_MD_SECTION), T5UI_P_TEXT, title);
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 4 || strcmp(e->d_name + l - 3, ".py")) continue;
        char path[160];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        char sub[48] = "";
        if (stat(path, &st) == 0) sa_fmt_bytes(sub, sizeof sub, (uint64_t)st.st_size);
        t5ui_obj_t *r = row(body, T5UI_ICON_CODE, e->d_name, sub);
        /* The row owns a copy of its path, freed with it. */
        char *p = malloc(strlen(path) + 1);
        if (!p) continue;
        strcpy(p, path);
        t5ui_on(r, T5UI_EV_CLICK, row_clicked, p);
        t5ui_on(r, T5UI_EV_DELETE, free_path, p);
        n++;
    }
    closedir(d);
    if (!n) sa_note(body, "No scripts here yet. Tap + to write one.");
}

static bool name_ok(const char *name)
{
    size_t l = strlen(name);
    if (l > 3 && !strcmp(name + l - 3, ".py")) l -= 3;
    if (!l || l > 48) return false;
    for (size_t i = 0; i < l; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

static void set_helper(const char *text, bool error)
{
    t5ui_set_int(s_newname, T5UI_P_HELPER_ERROR, error);
    t5ui_set_str(s_newname, T5UI_P_HELPER, text);
}

static void create_clicked(t5ui_event_t *e)
{
    (void)e;
    const char *name = t5ui_get_str(s_newname, T5UI_P_TEXT, "");
    if (!name || !name[0]) {
        set_helper("Type a name first", true);
        return;
    }
    if (!name_ok(name)) {
        set_helper("Use letters, digits, - and _ only", true);
        return;
    }
    char path[160];
    size_t l = strlen(name);
    snprintf(path, sizeof path, SCRIPTS "/%s%s", name, l > 3 && !strcmp(name + l - 3, ".py") ? "" : ".py");
    struct stat st;
    if (stat(path, &st) != 0 && !write_file(path, "import t5\n\n")) return;
    t5_shell_keyboard_hide();
    s_newname = NULL;
    /* Replace the "New script" page with the script itself, so Back from
     * the code goes to the list. */
    sa_pop(&s_nav);
    open_file(path);
}

static void name_ready(t5ui_event_t *e)
{
    create_clicked(e);
}

static void new_free(void *arg)
{
    (void)arg;
    t5_shell_keyboard_hide();
    s_newname = NULL;
}

static void new_clicked(t5ui_event_t *e)
{
    (void)e;
    t5ui_obj_t *body = sa_push(&s_nav, "New script");
    t5ui_set_int(body, T5UI_P_PAD_ALL, t5ui_dp(16));
    t5ui_set_int(body, T5UI_P_PAD_ROW, t5ui_dp(8));
    t5ui_set_int(body, T5UI_P_PAD_COLUMN, t5ui_dp(8));
    s_newname = t5ui_create(body, T5UI_MD_TEXTFIELD);
    t5ui_set_str(s_newname, T5UI_P_PLACEHOLDER, "Name");
    t5ui_set_int(s_newname, T5UI_P_WIDTH, T5UI_PCT(100));
    set_helper("Saved on the device as name.py", false);
    t5ui_on(s_newname, T5UI_EV_READY, name_ready, NULL);   /* its text area's */
    t5ui_obj_t *row = sa_button_row(body);
    t5ui_set_int(row, T5UI_P_PAD_LEFT, 0);
    t5ui_set_int(row, T5UI_P_PAD_RIGHT, 0);
    t5ui_set_int(row, T5UI_P_PLACE_MAIN, T5UI_PLACE_END);
    t5ui_set_int(row, T5UI_P_PLACE_CROSS, T5UI_PLACE_CENTER);
    t5ui_set_int(row, T5UI_P_PLACE_TRACK, T5UI_PLACE_CENTER);
    t5ui_obj_t *b = t5ui_create(row, T5UI_MD_BUTTON_CONTAINED);
    t5ui_set_str(b, T5UI_P_TEXT, "Create script");
    t5ui_on(b, T5UI_EV_CLICK, create_clicked, NULL);
    sa_set_refresh(&s_nav, NULL, s_newname, new_free);
    /* Focused, so the label floats and takes the accent: the keyboard's
     * focus gives its text area the focused state and the focus event
     * (the field reads both). Without a keyboard group, just the event. */
    if (t5ui_cmd(s_newname, T5UI_CMD_FOCUS, 0, 0, 0, NULL) < 0) {
        t5ui_cmd(s_newname, T5UI_CMD_SEND, T5UI_EV_FOCUS, 0, 0, NULL);
    }
    t5_shell_keyboard_show(t5ui_sys_native_input(s_newname));
}

static void fill_list(t5ui_obj_t *body)
{
    list_dir(body, SCRIPTS, "On the device");
    list_dir(body, SD_SCRIPTS, "On the SD card");
    sa_note(body, "Scripts can use the ports, the network, USB devices and storage.");
    /* Room under the last row so the floating + never covers it. */
    t5ui_obj_t *pad = t5ui_create(body, T5UI_BOX);
    t5ui_set_int(pad, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(pad, T5UI_P_HEIGHT, t5ui_dp(FAB_DP + 24));
}

static void list_refresh(sa_nav_t *nav, void *arg)
{
    (void)nav;
    (void)arg;
    if (!s_list_stale || !s_list) return;
    s_list_stale = false;
    t5ui_clean(s_list);
    fill_list(s_list);
}

static void build_list(void)
{
    t5ui_obj_t *body = sa_push(&s_nav, "Editor");
    s_list = body;
    fill_list(body);
    /* New script: a FAB over the list (not in its body), so it stays put
     * while the list scrolls. A page takes what's made in it into its
     * body, so the FAB goes on the screen, just above the list page: the
     * pages pushed later cover it (they fill the screen), and it goes with
     * the app's screen. The list page is the root and is never popped. */
    t5ui_obj_t *fab = t5ui_create(s_nav.root, T5UI_MD_FAB);
    t5ui_set_str(fab, T5UI_P_ICON, T5UI_ICON_ADD);
    t5ui_set_int(fab, T5UI_P_FLOATING, true);
    t5ui_set_int(fab, T5UI_P_ALIGN, T5UI_ALIGN_BOTTOM_RIGHT);
    t5ui_set_int(fab, T5UI_P_X, -t5ui_dp(16));
    t5ui_set_int(fab, T5UI_P_Y, -t5ui_dp(16));
    t5ui_on(fab, T5UI_EV_CLICK, new_clicked, NULL);
    sa_set_refresh(&s_nav, list_refresh, NULL, NULL);
}

static void on_create(t5_app_t *app)
{
    ensure_examples();
    sa_nav_init(&s_nav, app);
    build_list();
    const char *arg = t5_app_launch_arg(app);
    if (arg && arg[0] == '/') open_file(arg);     /* opened from Files */
}

static void on_destroy(t5_app_t *app)
{
    (void)app;
    sa_nav_destroy(&s_nav);
    s_list = NULL;
    s_newname = NULL;
    s_list_stale = false;
}

static const t5_app_desc_t s_desc = { .api = T5_API_LEVEL, .on_create = on_create, .on_destroy = on_destroy };

const t5_builtin_app_t sysapp_editor = {
    .id = T5_APP_ID_EDITOR,
    .name = "Editor",
    .desc = &s_desc,
    .perms = T5_PERM_PORTS | T5_PERM_NET | T5_PERM_USB | T5_PERM_STORAGE,
    .orientation = T5_ORIENT_ANY,
    .ports = sa_all_ports,
    .icon = T5UI_ICON_CODE,
};
