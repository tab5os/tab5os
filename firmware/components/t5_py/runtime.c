/* SPDX-License-Identifier: Apache-2.0
 * Python apps (runtime = "micropython"): the app descriptor the app runtime
 * uses for them (plan 8.1).
 *
 * The app's screen starts as a page with the app's name, a back arrow and a
 * console, so a script that only prints is usable as it is. t5.ui.screen()
 * covers it with the script's own UI. Output also goes to the device log
 * (one line per entry, "py <id>: ..."), which the developer-mode log stream
 * and `t5 run` show.
 *
 * Exit: back on the page (or in the script's own app bar) finishes the
 * script and closes the app; leaving the app any other way stops the VM. An
 * uncaught exception keeps the app open on the console with the traceback.
 *
 * Background wakes ([background] in the manifest) import the entry module
 * and call its on_alarm() (or the [background] entry name); UI code should
 * sit under `if __name__ == "__main__":`.
 */
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "md/md.h"
#include "t5_app/app.h"
#include "t5_app/apps.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_ui/t5_ui.h"
#include "py_priv.h"

static const char *TAG = "py";

typedef struct {
    t5_app_t *app;
    t5_py_t *vm;
    lv_obj_t *console;
    char line[160];
    size_t llen;
} pyapp_t;

static pyapp_t s_pa;    /* one Python app at a time (one VM) */
/* A background run's own: it can start while a Python app is open (standby
 * refreshes); the VM then says it's busy and the run ends without touching
 * the open app's. */
static pyapp_t s_bg;

static void log_line(pyapp_t *p)
{
    p->line[p->llen] = 0;
    T5_LOGI(TAG, "%s: %s", t5_app_id(p->app), p->line);
    p->llen = 0;
}

static void app_out(const char *s, size_t n, void *arg)
{
    pyapp_t *p = arg;
    if (p->console) t5_py_console_write(p->console, s, n);
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\r') continue;
        if (c == '\n' || p->llen >= sizeof p->line - 1) {
            log_line(p);
            if (c == '\n') continue;
        }
        p->line[p->llen++] = c;
    }
}

static void reveal_console(void *arg)
{
    pyapp_t *p = arg;
    t5_py_t *vm = p->vm;
    /* The script's UI covers the console with the traceback. */
    if (vm && vm->ui_root) {
        lv_obj_delete(vm->ui_root);
        vm->ui_root = NULL;
    }
}

static void app_done(t5_py_t *vm, t5_py_state_t how, void *arg)
{
    pyapp_t *p = arg;
    if (p->llen) log_line(p);
    T5_LOGI(TAG, "%s: %s", t5_app_id(p->app), t5_py_state_name(how));
    if (how == T5_PY_EXCEPTION) {
        if (vm->ui_root) t5_ui_async(reveal_console, p);
    } else if (how == T5_PY_DONE && (vm->back || vm->ui_used)) {
        t5_app_exit(p->app);       /* back pressed, or the script's UI is done */
    } else if (how == T5_PY_DONE && p->console) {
        static const char msg[] = "\n[finished]\n";
        t5_py_console_write(p->console, msg, sizeof msg - 1);
    }
}

static void page_back(lv_event_t *e)
{
    pyapp_t *p = lv_event_get_user_data(e);
    t5_app_exit(p->app);
}

static void on_create(t5_app_t *app)
{
    pyapp_t *p = &s_pa;
    memset(p, 0, sizeof *p);
    p->app = app;
    lv_obj_t *scr = t5_app_screen(app);
    lv_obj_t *page = md_page_create(scr, t5_app_id(app), page_back, p, NULL);
    lv_obj_t *body = md_page_body(page);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_scrollable(body, false);
    p->console = t5_py_console_create(body);
    const char *entry = t5_apps_entry_path(app);
    t5_py_cfg_t cfg = {
        .path = entry && entry[0] ? entry : NULL,
        .ui_parent = scr,
        .heap_bytes = t5_apps_min_heap(app),
        .out = app_out,
        .done = app_done,
        .arg = p,
    };
    t5_err_t e = cfg.path ? t5_py_start(&cfg, &p->vm) : T5_ERR_NOT_FOUND;
    if (e != T5_OK) {
        char msg[160];
        snprintf(msg, sizeof msg, "Can't start Python: %s%s\n", t5_err_str(e),
                 e == T5_ERR_BUSY ? " (another Python program is running: close the Terminal or Editor run)" : "");
        t5_py_console_write(p->console, msg, strlen(msg));
    }
}

static void on_destroy(t5_app_t *app)
{
    (void)app;
    pyapp_t *p = &s_pa;
    if (p->vm) t5_py_stop(p->vm, 1500);
    p->vm = NULL;
    p->console = NULL;
}

static void on_suspend(t5_app_t *app)
{
    (void)app;
    py_app_lifecycle(0);
}

static void on_resume(t5_app_t *app)
{
    (void)app;
    py_app_lifecycle(1);
}

static void on_rotate(t5_app_t *app, int degrees)
{
    (void)app;
    py_app_rotate(degrees);
}

static void alarm_done(t5_py_t *vm, t5_py_state_t how, void *arg)
{
    (void)vm;
    pyapp_t *p = arg;
    if (p->llen) log_line(p);
    T5_LOGI(TAG, "%s: background run %s", t5_app_id(p->app), t5_py_state_name(how));
}

static void on_alarm(t5_app_t *app)
{
    pyapp_t *p = &s_bg;
    memset(p, 0, sizeof *p);
    p->app = app;
    const char *entry = t5_apps_entry_path(app);
    if (!entry || !entry[0]) return;
    const char *base = strrchr(entry, '/');
    base = base ? base + 1 : entry;
    char mod[64];
    snprintf(mod, sizeof mod, "%s", base);
    char *dot = strrchr(mod, '.');
    if (dot) *dot = 0;
    char code[200];
    snprintf(code, sizeof code, "import %s\n%s.%s()\n", mod, mod, t5_apps_bg_entry(app));
    t5_py_cfg_t cfg = { .path = entry, .code = code, .out = app_out, .done = alarm_done, .arg = p,
                        .heap_bytes = t5_apps_min_heap(app) };
    if (t5_py_start(&cfg, &p->vm) != T5_OK) return;
    /* Past max_seconds the app runtime deletes this task and stops the VM. */
    (void)t5_py_wait(p->vm, T5_PY_FOREVER);
    t5_py_stop(p->vm, 500);
    p->vm = NULL;
}

static const t5_app_desc_t s_desc = {
    .api = T5_API_LEVEL,
    .on_create = on_create,
    .on_resume = on_resume,
    .on_suspend = on_suspend,
    .on_destroy = on_destroy,
    .on_rotate = on_rotate,
    .on_alarm = on_alarm,
};

t5_err_t t5_py_init(void)
{
    return t5_apps_set_runtime(T5_RUNTIME_MICROPYTHON, &s_desc);
}
