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
#include "t5_py/py.h"
#include "codeview.h"
#include "sysapps_priv.h"

#define SCRIPTS T5_USER_DIR "/scripts"
#define SD_SCRIPTS T5_SD_PATH "/scripts"
#define FILE_MAX (128 * 1024)

typedef struct {
    t5_app_t *app;
    char path[160];
    lv_obj_t *view, *out, *status, *run_icon;
    cv_t *cv;
    t5_py_t *vm;
    volatile bool run_done;
    lv_timer_t *timer;
} ed_t;

static sa_nav_t s_nav;
static ed_t *s_ed;          /* the open file's state (one at a time) */
static lv_obj_t *s_newname;
static lv_obj_t *s_list;    /* the script list's body, rebuilt when it comes back to the front */
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

static void update_title(ed_t *ed)
{
    char t[96];
    snprintf(t, sizeof t, "%s%s", base_name(ed->path), cv_dirty(ed->cv) ? " \xe2\x80\xa2" : "");
    md_appbar_set_title(sa_bar(&s_nav), t);
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
        md_snackbar_show(msg, NULL, NULL, NULL);
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
    t5_py_console_write(ed->out, msg, strlen(msg));
    ed->run_done = true;     /* the timer frees the VM on the UI task */
}

static void run_out(const char *s, size_t n, void *arg)
{
    t5_py_console_write(((ed_t *)arg)->out, s, n);
}

static void set_running(ed_t *ed, bool on)
{
    lv_label_set_text(ed->run_icon, on ? MD_ICON_STOP : MD_ICON_PLAY_ARROW);
}

static void stop_run(ed_t *ed)
{
    if (ed->vm) t5_py_stop(ed->vm, 1000);
    ed->vm = NULL;
    ed->run_done = false;
    set_running(ed, false);
}

static void run_clicked(lv_event_t *e)
{
    ed_t *ed = lv_event_get_user_data(e);
    if (ed->vm) {
        stop_run(ed);
        return;
    }
    if (!save(ed)) return;
    t5_shell_keyboard_hide();
    lv_obj_set_hidden(ed->out, false);
    t5_py_console_clear(ed->out);
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
        md_snackbar_show(m, NULL, NULL, NULL);
        return;
    }
    set_running(ed, true);
}

static void save_clicked(lv_event_t *e)
{
    ed_t *ed = lv_event_get_user_data(e);
    if (save(ed)) md_snackbar_show("Saved", NULL, NULL, NULL);
}

static void keyboard_clicked(lv_event_t *e)
{
    ed_t *ed = lv_event_get_user_data(e);
    cv_focus(ed->cv);
}

static void tick(lv_timer_t *t)
{
    ed_t *ed = lv_timer_get_user_data(t);
    if (ed->run_done) stop_run(ed);
}

static void ed_free(void *arg)
{
    ed_t *ed = arg;
    stop_run(ed);
    save(ed);
    if (ed->timer) lv_timer_delete(ed->timer);
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
    lv_obj_t *body = sa_push(&s_nav, base_name(path));
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_pad_gap(body, 0, 0);
    lv_obj_set_scrollable(body, false);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *bar = sa_bar(&s_nav);
    lv_obj_t *run = md_appbar_add_action(bar, MD_ICON_PLAY_ARROW, run_clicked, ed);
    ed->run_icon = lv_obj_get_child(run, 0) ? lv_obj_get_child(run, 0) : run;
    md_appbar_add_action(bar, MD_ICON_SAVE, save_clicked, ed);
    md_appbar_add_action(bar, MD_ICON_KEYBOARD, keyboard_clicked, ed);
    ed->view = cv_create(body);
    lv_obj_set_height(ed->view, 1);   /* px: flex-grow sets it */
    lv_obj_set_flex_grow(ed->view, 2);
    ed->cv = cv_get(ed->view);
    ed->status = md_label_create(body, "", MD_TYPE_CAPTION);
    lv_obj_set_style_pad_hor(ed->status, T5_DP(16), 0);
    lv_obj_set_style_pad_ver(ed->status, T5_DP(4), 0);
    lv_obj_set_style_text_opa(ed->status, md_scheme()->on_surface.medium, 0);
    ed->out = t5_py_console_create(body);
    lv_obj_set_style_border_side(ed->out, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(ed->out, T5_DP(1), 0);
    lv_obj_set_style_border_color(ed->out, md_color(md_scheme()->c.on_surface), 0);
    lv_obj_set_style_border_opa(ed->out, md_scheme()->on_surface.divider, 0);
    lv_obj_set_height(ed->out, 1);   /* px: flex-grow sets it */
    lv_obj_set_flex_grow(ed->out, 1);
    lv_obj_set_hidden(ed->out, true);
    cv_set_text(ed->cv, text ? text : "", len);
    free(text);
    cv_set_changed_cb(ed->cv, changed, ed);
    changed(ed->cv, ed);
    ed->timer = lv_timer_create(tick, 100, ed);
    s_ed = ed;
    sa_set_refresh(&s_nav, NULL, ed, ed_free);
}

static void row_clicked(lv_event_t *e)
{
    const char *path = lv_event_get_user_data(e);
    open_file(path);
}

static void free_path(lv_event_t *e)
{
    lv_free(lv_event_get_user_data(e));
}

static void list_dir(lv_obj_t *body, const char *dir, const char *title)
{
    DIR *d = opendir(dir);
    if (!d) return;
    md_section_create(body, title);
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
        lv_obj_t *r = md_row_create(body, MD_ICON_CODE, e->d_name, sub);
        /* The row owns a copy of its path, freed with it. */
        char *p = lv_malloc(strlen(path) + 1);
        if (!p) continue;
        strcpy(p, path);
        md_row_set_clickable(r, row_clicked, p);
        lv_obj_add_event_cb(r, free_path, LV_EVENT_DELETE, p);
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

static void create_clicked(lv_event_t *e)
{
    (void)e;
    const char *name = md_textfield_text(s_newname);
    if (!name || !name[0]) {
        md_textfield_set_helper(s_newname, "Type a name first", true);
        return;
    }
    if (!name_ok(name)) {
        md_textfield_set_helper(s_newname, "Use letters, digits, - and _ only", true);
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

static void name_ready(lv_event_t *e)
{
    create_clicked(e);
}

static void new_free(void *arg)
{
    (void)arg;
    t5_shell_keyboard_hide();
    s_newname = NULL;
}

static void new_clicked(lv_event_t *e)
{
    (void)e;
    lv_obj_t *body = sa_push(&s_nav, "New script");
    lv_obj_set_style_pad_all(body, T5_DP(16), 0);
    lv_obj_set_style_pad_gap(body, T5_DP(8), 0);
    s_newname = md_textfield_create(body, "Name", false);
    lv_obj_set_width(s_newname, LV_PCT(100));
    md_textfield_set_helper(s_newname, "Saved on the device as name.py", false);
    lv_obj_t *ta = md_textfield_ta(s_newname);
    lv_obj_add_event_cb(ta, name_ready, LV_EVENT_READY, NULL);
    lv_obj_t *row = sa_button_row(body);
    lv_obj_set_style_pad_hor(row, 0, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *b = md_button_create(row, "Create script", MD_BUTTON_CONTAINED);
    lv_obj_add_event_cb(b, create_clicked, LV_EVENT_CLICKED, NULL);
    sa_set_refresh(&s_nav, NULL, s_newname, new_free);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    lv_obj_send_event(ta, LV_EVENT_FOCUSED, NULL);
    t5_shell_keyboard_show(ta);
}

static void fill_list(lv_obj_t *body)
{
    list_dir(body, SCRIPTS, "On the device");
    list_dir(body, SD_SCRIPTS, "On the SD card");
    sa_note(body, "Scripts can use the ports, the network, USB devices and storage.");
    /* Room under the last row so the floating + never covers it. */
    lv_obj_t *pad = lv_obj_create(body);
    lv_obj_remove_style_all(pad);
    lv_obj_set_size(pad, LV_PCT(100), T5_DP(MD_FAB_DP + 24));
}

static void list_refresh(sa_nav_t *nav, void *arg)
{
    (void)nav;
    (void)arg;
    if (!s_list_stale || !s_list) return;
    s_list_stale = false;
    lv_obj_clean(s_list);
    fill_list(s_list);
}

static void build_list(void)
{
    lv_obj_t *body = sa_push(&s_nav, "Editor");
    s_list = body;
    fill_list(body);
    /* New script: a FAB on the list page (not the body), so it stays put
     * while the list scrolls and goes away with the page. */
    lv_obj_t *fab = md_fab_create(lv_obj_get_parent(body), MD_ICON_ADD, false);
    lv_obj_set_floating(fab, true);
    lv_obj_align(fab, LV_ALIGN_BOTTOM_RIGHT, -T5_DP(16), -T5_DP(16));
    lv_obj_add_event_cb(fab, new_clicked, LV_EVENT_CLICKED, NULL);
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
    .icon = MD_ICON_CODE,
};
