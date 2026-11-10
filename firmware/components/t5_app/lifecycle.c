/* SPDX-License-Identifier: Apache-2.0
 * App lifecycle and resource tracking (plan 5.4, 5.10, 5.11).
 *
 * Launch (UI task): exit the current app; find the app (built-in or
 * installed); mark it running in RTC memory (crash guard); load its ELF;
 * create its root screen; make it the UI task's context; on_create,
 * show the screen, on_resume.
 *
 * Exit (UI task): on_suspend, on_destroy; show the launcher; then free what
 * is left, in a fixed order: tasks (asked to stop first), LVGL timers,
 * animations, UI roots, port claims, rails, wake locks, other handles, the
 * heap; unload the ELF; clear the crash guard.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "md/md.h"
#include "t5_app/abi.h"
#include "t5_battery/battery.h"
#include "t5_app/apps.h"
#include "t5_app/elfsym.h"
#include "t5_app/isolate.h"
#include "t5_app/wrappers.h"
#include "t5_core/fault.h"
#include "t5_core/event.h"
#include "t5_core/log.h"
#include "t5_pkg/pkg.h"
#include "t5_pkg/zip.h"
#include "t5_ports/access.h"
#include "t5_power/autosave.h"
#include "t5_power/power.h"
#include "t5_storage/paths.h"
#include "t5_storage/t5_storage.h"
#include "t5_time/t5_time.h"
#include "t5_ui/orient.h"
#include "t5_ui/t5_ui.h"
#include "app_priv.h"
#include "app_session.h"
#include "apps_hw.h"

static const char *TAG = "t5.app";

#define ALARM_FILE "alarm_app"
#define TASK_STOP_GRACE_MS 150
#define FILE_CALL_GRACE_MS 3000   /* one write to a slow card */
#define INSTALL_EXIT_MS 3000

static t5_mutex_t s_lock;
static t5_app_t *s_fg;
static t5_app_t *s_headless;        /* app running on_alarm */
static const t5_builtin_app_t *s_builtins[T5_APPS_BUILTIN_MAX];
static int s_nbuiltins;
static t5_crash_db_t s_db;
static bool s_inited;
static char s_crashed[T5_APP_ID_LEN];   /* the app that crashed or hung on the last run */
static const t5_apps_home_t *s_home;
static t5_energy_db_t s_energy;
static uint32_t s_energy_saved_seq;
static bool s_restoring;            /* the next launch is a session restore */
static bool s_safe;                 /* safe mode: installed apps don't run */
static const t5_app_desc_t *s_runtime_desc[T5_RUNTIME_COUNT];   /* [native] unused */

#define ENERGY_DB "energy.bin"
#define STATE_FILE "/.state"

void apps_lock(void)
{
    t5_mutex_lock(s_lock);
}

void apps_unlock(void)
{
    t5_mutex_unlock(s_lock);
}

t5_app_t *apps_fallback(void)
{
    return s_headless ? s_headless : s_fg;
}

/* Event payloads carry a truncated id. */
static void ev_id(t5_ev_app_t *ev, const char *id)
{
    size_t n = strlen(id);
    if (n > sizeof ev->id - 1) n = sizeof ev->id - 1;
    memcpy(ev->id, id, n);
    ev->id[n] = 0;
}

#define T5_FS_DATA_RESERVE (128u * 1024u)   /* of /data, kept for the OS */

static bool valid(const t5_app_t *a)
{
    return a && a->hdr.magic == T5_APP_CTX_MAGIC;
}

static void hook_add(void *app, t5_res_kind_t kind, void *handle, t5_res_release_fn_t release)
{
    t5_app_t *a = app;
    if (!valid(a)) return;
    apps_lock();
    if (!t5_tracker_add(&a->trk, kind, handle, release)) {
        T5_LOGW(TAG, "%s: resource table full, a handle will leak", a->id);
    }
    apps_unlock();
}

static void hook_remove(void *app, void *handle)
{
    t5_app_t *a = app;
    if (!valid(a)) return;
    apps_lock();
    t5_tracker_remove(&a->trk, handle);
    apps_unlock();
}

static const t5_res_hooks_t s_hooks = { hook_add, hook_remove };

/* Release runs with the context set to the exiting app (release_everything). */
static int app_mutexes_held(void *task)
{
    t5_app_t *a = t5_app_ctx_get();
    if (!valid(a)) return -1;
    if (!t5_mutex_lock_timeout(s_lock, 0)) return -1;   /* maybe held by task itself */
    int n = t5_app_mutexes_held_by(a, task);
    t5_mutex_unlock(s_lock);
    return n;
}

static void task_created(t5_task_t task, void *ctx)
{
    hook_add(ctx, T5_RES_TASK, task, apps_hw_task_delete);
}

static void task_exited(t5_task_t task, void *ctx)
{
    hook_remove(ctx, task);
}

static void obj_delete(void *obj)
{
    lv_obj_delete(obj);
}

static void tracked_obj_deleted(lv_event_t *e)
{
    hook_remove(lv_event_get_user_data(e), lv_event_get_target_obj(e));
}

/* An object an app owns (its screen, its top-layer dialogs): deleted on
 * exit unless the app deleted it first. */
static void track_obj(t5_app_t *a, lv_obj_t *obj)
{
    hook_add(a, T5_RES_UI, obj, obj_delete);
    lv_obj_add_event_cb(obj, tracked_obj_deleted, LV_EVENT_DELETE, a);
}

static void top_child_created(lv_event_t *e)
{
    t5_app_t *a = t5_app_ctx_get();
    lv_obj_t *child = lv_event_get_param(e);
    if (valid(a) && child && lv_obj_get_parent(child) == lv_layer_top()) track_obj(a, child);
}

void *t5_sys_enter(void)
{
    void *saved = t5_app_ctx_get();
    t5_app_ctx_set(NULL);
    return saved;
}

void t5_sys_leave(void *saved)
{
    t5_app_ctx_set(saved);
}

void t5_apps_set_home(const t5_apps_home_t *home)
{
    s_home = home;
}

lv_obj_t *apps_home_screen(void)
{
    return s_home->screen();
}

static void home_refresh_ui(void *arg)
{
    (void)arg;
    if (s_home->refresh) s_home->refresh();
}

/* The home screen is LVGL: rebuild it on the UI task. Installs and removals
 * run on the console or a file manager's worker, and rebuilding the
 * launcher from there raced the UI task (heap corruption in LVGL's pool,
 * 2026-11-07). Queued, not waited for: the caller may hold things the UI
 * task wants. */
void apps_home_refresh(void)
{
    if (t5_ui_on_ui_task() || !t5_ui_ready()) home_refresh_ui(NULL);
    else if (t5_ui_async(home_refresh_ui, NULL) != T5_OK) T5_LOGW(TAG, "home refresh: UI queue full");
}

static void home_changed(const char *id)
{
    if (s_home->changed) s_home->changed(id);
}

static void home_notice(t5_apps_notice_t what, const t5_app_t *a)
{
    if (s_home->notice) s_home->notice(what, a ? (a->man.name[0] ? a->man.name : a->id) : NULL);
}

lv_obj_t *t5_app_screen(t5_app_t *app)
{
    return valid(app) ? app->screen : NULL;
}

const char *t5_app_id(t5_app_t *app)
{
    return valid(app) ? app->id : "";
}

const char *t5_app_data_dir(t5_app_t *app)
{
    return valid(app) ? app->data_dir : "";
}

const char *t5_app_install_dir(t5_app_t *app)
{
    return valid(app) ? app->install_dir : "";
}

const char *t5_app_launch_arg(t5_app_t *app)
{
    return valid(app) ? app->arg : "";
}

bool t5_app_restored(t5_app_t *app)
{
    return valid(app) && app->restored;
}

static void state_path(const t5_app_t *a, char *out, size_t n)
{
    snprintf(out, n, "%s" STATE_FILE, a->data_dir);
}

t5_err_t t5_app_state_save(t5_app_t *app, const void *data, size_t len)
{
    if (!valid(app) || (len && !data)) return T5_ERR_INVALID_ARG;
    if (len > T5_APP_STATE_MAX) return T5_ERR_NO_MEM;
    char path[T5_PKG_PATH_MAX + 8];
    state_path(app, path, sizeof path);
    if (!len) {
        remove(path);
        return T5_OK;
    }
    return t5_storage_blob_write(path, 1, data, len);
}

t5_err_t t5_app_state_load(t5_app_t *app, void *buf, size_t cap, size_t *len)
{
    if (!valid(app) || !buf) return T5_ERR_INVALID_ARG;
    char path[T5_PKG_PATH_MAX + 8];
    state_path(app, path, sizeof path);
    return t5_storage_blob_read(path, 1, buf, cap, len);
}

void t5_app_set_user(t5_app_t *app, void *user)
{
    if (valid(app)) app->user = user;
}

void *t5_app_get_user(t5_app_t *app)
{
    return valid(app) ? app->user : NULL;
}

t5_app_t *t5_app_self(void)
{
    t5_app_t *a = t5_app_ctx_get();
    return valid(a) ? a : NULL;
}

bool t5_app_running(t5_app_t *app)
{
    return valid(app) && !app->exiting;
}

bool t5_app_has_permission(t5_app_t *app, uint32_t perm)
{
    if (!valid(app) || !(app->man.perms & perm)) return false;
    if (perm & t5_perm_prompted()) return (app->granted & perm) == perm;
    return true;
}

static int64_t now_wall(void)
{
    return t5_time_valid() ? (int64_t)t5_time_now() : 0;
}

static void db_save(void)
{
    char path[T5_APP_PATH_MAX];
    if (t5_sys_path(path, sizeof path, T5_CRASH_DB_NAME) != T5_OK || t5_crash_db_save(&s_db, path) != T5_OK) {
        T5_LOGW(TAG, "can't save crash records");
    }
}

/* "<sys dir>/dumps[/<id>.elf]" */
static t5_err_t dump_path(char *out, size_t n, const char *id)
{
    T5_RETURN_ON_ERR(t5_sys_path(out, n, T5_DUMP_DIR_NAME));
    if (!id) return T5_OK;
    size_t len = strlen(out);
    int w = snprintf(out + len, n - len, "/%.40s.elf", id);
    return w > 0 && (size_t)w < n - len ? T5_OK : T5_ERR_INVALID_ARG;
}

int t5_apps_crashes(t5_crash_rec_t *out, int max)
{
    int n = s_db.n < max ? s_db.n : max;
    memcpy(out, s_db.rec, sizeof(t5_crash_rec_t) * (size_t)n);
    return n;
}

t5_err_t t5_apps_coredump_path(const char *id, char *path, size_t n)
{
    t5_crash_rec_t *r = t5_crash_find(&s_db, id);
    if (!r || !r->has_dump) return T5_ERR_NOT_FOUND;
    return dump_path(path, n, id);
}

/* At boot: was an app running when the device reset? */
static bool s_crash_checked;

static void crash_check(void)
{
    if (s_crash_checked) return;
    s_crash_checked = true;
    char path[T5_APP_PATH_MAX];
    if (t5_sys_path(path, sizeof path, T5_CRASH_DB_NAME) == T5_OK) {
        T5_IGNORE(t5_crash_db_load(&s_db, path));   /* empty on failure: no history */
    }
    t5_crash_boot_t b;
    apps_hw_guard_read(&b);
    apps_hw_guard_clear();
    t5_crash_kind_t k = t5_crash_classify(&b, t5_power_wake_reason() == T5_WAKE_CRASH);
    if (k == T5_CRASH_NONE) return;
    t5_crash_rec_t *r = t5_crash_record(&s_db, b.running, k, now_wall());
    snprintf(s_crashed, sizeof s_crashed, "%s", b.running);
    if (k == T5_CRASH_PANIC) {
        if (dump_path(path, sizeof path, NULL) == T5_OK) mkdir(path, 0755);
        if (dump_path(path, sizeof path, b.running) == T5_OK && apps_hw_coredump_save(path)) r->has_dump = 1;
    }
    db_save();
    T5_LOGW(TAG, "%s %s (%u in a row)%s", b.running, t5_crash_kind_name(k), r->consecutive,
            r->consecutive >= T5_CRASH_BLOCK_AFTER ? ": auto-launch blocked" : "");
    t5_ev_app_t ev = { .err = (int32_t)k };
    ev_id(&ev, b.running);
    t5_event_post(T5_EV_APP_CRASHED, &ev, sizeof ev);
}

void t5_apps_set_safe_mode(bool on)
{
    s_safe = on;
}

bool t5_apps_safe_mode(void)
{
    return s_safe;
}

t5_err_t t5_apps_set_runtime(t5_runtime_t runtime, const t5_app_desc_t *desc)
{
    if (runtime <= T5_RUNTIME_NATIVE || runtime >= T5_RUNTIME_COUNT || !desc) return T5_ERR_INVALID_ARG;
    s_runtime_desc[runtime] = desc;
    return T5_OK;
}

const char *t5_apps_entry_path(t5_app_t *app)
{
    return valid(app) ? app->entry_path : "";
}

const char *t5_apps_bg_entry(t5_app_t *app)
{
    return valid(app) ? app->man.bg_entry : "";
}

size_t t5_apps_min_heap(t5_app_t *app)
{
    return valid(app) ? (size_t)app->man.min_heap_kb * 1024u : 0;
}

t5_err_t t5_apps_register_builtin(const t5_builtin_app_t *b)
{
    if (!b || !b->id || !b->desc) return T5_ERR_INVALID_ARG;
    if (s_nbuiltins >= T5_APPS_BUILTIN_MAX) return T5_ERR_NO_MEM;
    s_builtins[s_nbuiltins++] = b;
    return T5_OK;
}

static const t5_builtin_app_t *find_builtin(const char *id)
{
    for (int i = 0; i < s_nbuiltins; i++) {
        if (!strcmp(s_builtins[i]->id, id)) return s_builtins[i];
    }
    return NULL;
}

static t5_err_t make_app(const char *id, t5_app_t **out)
{
    t5_app_t *a = calloc(1, sizeof *a);
    static uint32_t s_instances;
    if (a) {
        a->instance = ++s_instances;
        a->fs_used = -1;
    }
    if (!a) return T5_ERR_NO_MEM;
    snprintf(a->id, sizeof a->id, "%s", id);
    a->hdr.magic = T5_APP_CTX_MAGIC;
    a->hdr.id = a->id;
    const t5_builtin_app_t *b = find_builtin(id);
    if (b) {
        a->builtin = b;
        a->desc = b->desc;
        snprintf(a->man.id, sizeof a->man.id, "%s", b->id);
        snprintf(a->man.name, sizeof a->man.name, "%s", b->name ? b->name : b->id);
        strcpy(a->man.version, "builtin");
        a->man.api = T5_API_LEVEL;
        a->man.perms = b->perms;
        a->granted = b->perms;          /* system apps don't ask */
        a->man.orientation = (t5_orient_pref_t)b->orientation;
        for (int i = 0; b->ports && b->ports[i] && i < T5_MANIFEST_MAX_PORTS; i++) {
            snprintf(a->man.ports[i], sizeof a->man.ports[i], "%s", b->ports[i]);
            a->man.nports = i + 1;
        }
        if (t5_pkg_data_dir(id, a->data_dir, sizeof a->data_dir) != T5_OK) {
            free(a);
            return T5_ERR_INVALID_ARG;
        }
    } else {
        t5_pkg_info_t *info = malloc(sizeof *info);
        if (!info) { free(a); return T5_ERR_NO_MEM; }
        t5_err_t e = t5_pkg_find(id, info);
        if (e != T5_OK) {
            free(info);
            free(a);
            return e;
        }
        a->man = info->manifest;
        snprintf(a->install_dir, sizeof a->install_dir, "%s", info->dir);
        snprintf(a->data_dir, sizeof a->data_dir, "%s", info->data_dir);
        snprintf(a->entry_path, sizeof a->entry_path, "%s", info->entry_path);
        free(info);
        if (a->man.runtime != T5_RUNTIME_NATIVE) {
            if ((unsigned)a->man.runtime >= T5_RUNTIME_COUNT || !s_runtime_desc[a->man.runtime]) {
                free(a);
                return T5_ERR_NOT_SUPPORTED;
            }
            a->desc = s_runtime_desc[a->man.runtime];
        }
        T5_IGNORE(t5_pkg_grants_load(id, &a->granted, &a->denied));   /* 0/0 on failure: ask again */
    }
    t5_tracker_init(&a->trk);
    *out = a;
    return T5_OK;
}

static uint32_t allowed_rotations(const t5_app_t *a)
{
    switch (a->man.orientation) {
    case T5_ORIENT_ANY: return T5_ROT_ALL;
    case T5_ORIENT_PORTRAIT: return T5_ROT_PORTRAIT;
    default: return T5_ROT_LANDSCAPE;
    }
}

static void ensure_dir(const char *path)
{
    char tmp[T5_APP_PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    tmp[sizeof tmp - 1] = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* Stop every task of the app, each where it holds no OS mutex, before any is
 * deleted: the release walk below changes the app's handle table without a
 * lock, so nothing of the app may run during it. Runs with the apps lock held,
 * so a task can't leave the table (it exits through hook_remove) meanwhile;
 * a parked task never holds that lock. */
#define QUIESCE_MS 3000
#define QUIESCE_STUCK_MS 50
static void quiesce(t5_app_t *a)
{
    void *parked[T5_APP_MAX_TASKS] = { 0 };
    int np = 0;
    uint32_t t0 = t5_time_ms(), progress = t0;
    for (;;) {
        int pending = 0;
        apps_lock();
        void *tasks[T5_APP_MAX_TASKS];
        int nt = t5_tracker_list(&a->trk, T5_RES_TASK, tasks, T5_APP_MAX_TASKS);
        if (nt > T5_APP_MAX_TASKS) {
            pending += nt - T5_APP_MAX_TASKS;
            nt = T5_APP_MAX_TASKS;
        }
        for (int i = 0; i < nt; i++) {
            void *task = tasks[i];
            bool done = false;
            for (int j = 0; j < np && !done; j++) done = parked[j] == task;
            if (done) continue;
            if (np < T5_APP_MAX_TASKS && apps_hw_task_park(task, app_mutexes_held)) {
                parked[np++] = task;
                progress = t5_time_ms();
            } else {
                pending++;
            }
        }
        apps_unlock();
        uint32_t now = t5_time_ms();
        if (!pending) return;
        if (now - t0 >= QUIESCE_MS) {
            T5_LOGE(TAG, "%s: %d task(s) still hold OS locks after %u ms", a->id, pending, QUIESCE_MS);
            return;
        }
        if (now - progress >= QUIESCE_STUCK_MS) {
            /* A parked task may hold an app mutex one of the others waits for. */
            for (int j = 0; j < np; j++) apps_hw_task_pause(parked[j], false);
            np = 0;
            progress = now;
        }
        t5_delay_ms(1);
    }
}

static void release_everything(t5_app_t *a)
{
    a->exiting = true;
    perm_app_gone(a);
    picker_app_gone(a);
    /* Its network calls end: new ones are refused now that it is exiting
     * (netguard.c), and closing its sockets wakes those blocked on them. */
    int socks = app_close_sockets(a);
    if (socks) T5_LOGI(TAG, "%s: closed %d socket(s)", a->id, socks);
    /* Tasks first, and politely: loops that check t5_app_running() get a
     * moment to finish (and release locks they hold) before the rest is
     * deleted. */
    for (uint32_t t = 0; t < TASK_STOP_GRACE_MS && t5_tracker_count(&a->trk, T5_RES_TASK); t += 10) {
        t5_delay_ms(10);
    }
    /* A task inside a filesystem call holds the filesystem's lock. Deleted
     * there, it would leave /data locked for good and the device wedged.
     * New calls can't start (file_enter in wrappers.c), so this drains. */
    uint32_t file_grace = a->faulted ? 300 : FILE_CALL_GRACE_MS;
    for (uint32_t t = 0; t < file_grace && __atomic_load_n(&a->file_calls, __ATOMIC_SEQ_CST) > 0; t += 10) {
        t5_delay_ms(10);
    }
    if (__atomic_load_n(&a->file_calls, __ATOMIC_SEQ_CST) > 0) {
        T5_LOGE(TAG, "%s: a file call is still running after %u ms", a->id, (unsigned)file_grace);
    }
    void *saved = t5_app_ctx_get();
    t5_app_ctx_set(a);          /* release paths report their removals for this app */
    quiesce(a);
    /* No lock around the walk: release functions call back into the
     * tracker (hook_remove takes the lock), and app tasks, the only other
     * writers, are the first kind released. */
    int n = t5_tracker_release_all(&a->trk);
    t5_app_ctx_set(saved);
    if (n) T5_LOGI(TAG, "%s: released %d leftover handles", a->id, n);
}

static void destroy(t5_app_t *a, bool clean)
{
    if (a->elf) {
        apps_hw_elf_unload(a->elf);
        a->elf = NULL;
    }
    apps_hw_guard_clear();
    if (clean) {
        t5_crash_rec_t *r = t5_crash_find(&s_db, a->id);
        if (r && r->consecutive) {
            t5_crash_clean_exit(&s_db, a->id);
            db_save();
        }
    }
    t5_ev_app_t ev = { 0 };
    ev_id(&ev, a->id);
    t5_event_post(T5_EV_APP_STOPPED, &ev, sizeof ev);
    a->hdr.magic = 0;
    free(a);
}

static void call(t5_app_t *a, void (*fn)(t5_app_t *))
{
    if (fn) fn(a);
}

/* UI task. callbacks: false when the app's code can't be trusted to run
 * again (it crashed or hung): on_suspend and on_destroy are skipped and
 * the OS just reclaims everything. */
static void exit_fg_ex(bool clean, bool callbacks)
{
    t5_app_t *a = s_fg;
    if (!a) return;
    T5_LOGI(TAG, "exit %s%s", a->id, callbacks ? "" : " (stopped)");
    a->exiting = true;
    t5_app_ctx_set(a);
    if (callbacks && a->state == APP_RESUMED) call(a, a->desc->on_suspend);
    a->state = APP_EXITING;
    if (callbacks) call(a, a->desc->on_destroy);
    t5_app_ctx_set(NULL);
    lv_screen_load(apps_home_screen());
    t5_ui_set_allowed_rotations(T5_ROT_ALL);
    apps_lock();
    s_fg = NULL;
    apps_unlock();
    anr_app_changed(NULL);
    home_changed("");
    release_everything(a);
    app_session_set(NULL);
    destroy(a, clean);
    apps_home_refresh();
}

static void exit_fg(bool clean)
{
    /* An app that crashed or hung doesn't get to run its exit callbacks. */
    bool broken = s_fg && s_fg->faulted;
    exit_fg_ex(clean && !broken, !broken);
}

static t5_err_t launch_fg(const char *id, const char *arg)
{
    bool restoring = s_restoring;
    s_restoring = false;
    if (s_fg) {
        if (!strcmp(s_fg->id, id) && (!arg || !strcmp(s_fg->arg, arg))) return T5_OK;
        exit_fg(true);
    }
    T5_ASSERT(s_fg == NULL);   /* one foreground app: the old one is fully gone */
    if (s_safe && !find_builtin(id)) {
        T5_LOGW(TAG, "safe mode: not launching %s", id);
        home_notice(T5_APPS_NOTICE_SAFE_MODE, NULL);
        return T5_ERR_DENIED;
    }
    t5_app_t *a;
    t5_err_t e = make_app(id, &a);
    if (e != T5_OK) {
        T5_LOGE(TAG, "launch %s: %s", id, t5_err_str(e));
        return e;
    }
    ensure_dir(a->data_dir);
    a->started_ms = t5_time_ms();
    a->restored = restoring;
    if (arg) snprintf(a->arg, sizeof a->arg, "%s", arg);
    apps_hw_guard_set(a->id, false);
    if (!a->builtin && a->man.runtime == T5_RUNTIME_NATIVE) {
        e = apps_hw_elf_load(a, a->entry_path, &a->elf, &a->desc);
        if (e == T5_OK && (!a->desc || a->desc->api < 1 || a->desc->api > T5_API_LEVEL)) e = T5_ERR_NOT_SUPPORTED;
        if (e != T5_OK) {
            T5_LOGE(TAG, "load %s: %s", a->entry_path, t5_err_str(e));
            destroy(a, false);
            return e;
        }
    }
    apps_lock();
    s_fg = a;
    apps_unlock();
    anr_app_changed(a);
    a->state = APP_CREATED;
    t5_ui_set_allowed_rotations(allowed_rotations(a));
    /* The UI task now works for the app (see apps.h). */
    t5_app_ctx_set(a);
    a->screen = lv_obj_create(NULL);
    track_obj(a, a->screen);
    if (s_home->prepare) {
        void *saved = t5_sys_enter();
        s_home->prepare(a->screen, a->id);
        t5_sys_leave(saved);
    }
    home_changed(a->id);
    call(a, a->desc->on_create);
    if (a->exiting) {                 /* exited inside on_create */
        t5_app_ctx_set(NULL);
        return T5_OK;
    }
    lv_screen_load_anim(a->screen, LV_SCREEN_LOAD_ANIM_FADE_IN, MD_DUR_DIALOG_IN_MS, 0, false);
    call(a, a->desc->on_resume);
    a->state = APP_RESUMED;
    app_session_set(a->id);
    t5_ev_app_t ev = { 0 };
    ev_id(&ev, a->id);
    t5_event_post(T5_EV_APP_STARTED, &ev, sizeof ev);
    T5_LOGI(TAG, "started %s %s (%s)", a->id, a->man.version, a->builtin ? "built-in" : t5_runtime_name(a->man.runtime));
    return T5_OK;
}

typedef struct {
    char id[T5_APP_ID_LEN];
    char arg[128];
    bool has_arg;
    t5_err_t result;
} launch_req_t;

static void launch_async(void *p)
{
    launch_req_t *r = p;
    r->result = launch_fg(r->id, r->has_arg ? r->arg : NULL);
}

static void launch_async_free(void *p)
{
    launch_async(p);
    free(p);
}

t5_err_t t5_apps_launch(const char *id)
{
    return t5_apps_launch_arg(id, NULL);
}

t5_err_t t5_apps_launch_arg(const char *id, const char *arg)
{
    if (!s_inited || !id) return T5_ERR_STATE;
    /* Opening an app is the person using the device: wake the screen (the
     * UI doesn't run while it's off). */
    t5_power_user_activity();
    launch_req_t *r = calloc(1, sizeof *r);
    if (!r) return T5_ERR_NO_MEM;
    snprintf(r->id, sizeof r->id, "%s", id);
    if (arg) {
        snprintf(r->arg, sizeof r->arg, "%s", arg);
        r->has_arg = true;
    }
    t5_err_t e = t5_ui_async(launch_async_free, r);
    if (e != T5_OK) free(r);
    return e;
}

t5_err_t t5_apps_launch_wait(const char *id, uint32_t timeout_ms)
{
    if (!s_inited || !id) return T5_ERR_STATE;
    /* The UI task doesn't run while the screen is off: a call queued for it
     * runs once the power task has the screen back on. */
    t5_power_user_activity();
    launch_req_t r = { .result = T5_ERR_TIMEOUT };
    snprintf(r.id, sizeof r.id, "%s", id);
    T5_RETURN_ON_ERR(t5_ui_sync(launch_async, &r, timeout_ms));
    return r.result;
}

static void exit_async(void *p)
{
    t5_app_t *a = p;
    if (a && a != s_fg) return;          /* already gone */
    exit_fg(true);
}

void t5_app_exit(t5_app_t *app)
{
    if (!valid(app) || app != s_fg) return;
    app->exiting = true;
    t5_ui_async(exit_async, app);
}

t5_err_t t5_apps_exit_current(void)
{
    return t5_ui_async(exit_async, NULL);
}

t5_err_t t5_apps_exit_wait(uint32_t timeout_ms)
{
    t5_power_user_activity();   /* the UI task runs only with the screen on */
    return t5_ui_sync(exit_async, NULL, timeout_ms);
}

static void exit_id_async(void *id)
{
    if (s_fg && !strcmp(s_fg->id, id)) exit_fg(true);
}

static bool is_fg(const char *id)
{
    if (!s_inited) return false;
    apps_lock();
    bool fg = s_fg && !strcmp(s_fg->id, id);
    apps_unlock();
    return fg;
}

static t5_err_t stop_running(const char *id, char *err, size_t errlen)
{
    if (!is_fg(id)) return T5_OK;
    t5_err_t e = T5_OK;
    if (t5_ui_on_ui_task()) {
        exit_id_async((void *)id);
    } else {
        t5_power_user_activity();   /* the UI task runs only with the screen on */
        e = t5_ui_sync(exit_id_async, (void *)id, INSTALL_EXIT_MS);
    }
    if (e == T5_OK && is_fg(id)) e = T5_ERR_BUSY;
    if (e != T5_OK) snprintf(err, errlen, "the running copy of %s didn't stop (%s)", id, t5_err_str(e));
    return e == T5_OK ? T5_OK : T5_ERR_BUSY;
}

void t5_apps_current(char *id, size_t n)
{
    apps_lock();
    snprintf(id, n, "%s", s_fg ? s_fg->id : "");
    apps_unlock();
}

bool t5_apps_fg_manifest(t5_manifest_t *out)
{
    apps_lock();
    bool ok = s_fg != NULL;
    if (ok) *out = s_fg->man;
    apps_unlock();
    return ok;
}

bool t5_apps_headless(char *id, size_t n)
{
    apps_lock();
    bool on = s_headless != NULL;
    if (on) snprintf(id, n, "%s", s_headless->id);
    apps_unlock();
    return on;
}

bool t5_apps_alarm_pending(char *id, size_t n, time_t *when)
{
    /* The file is read again only when the scheduled time changes. */
    static time_t s_seen;
    static char s_id[T5_APP_ID_LEN];
    uint32_t flags = 0;
    time_t t = t5_power_next_wake(&flags);
    if (!t || !(flags & T5_WAKE_FLAG_BACKGROUND)) return false;
    if (t != s_seen) {
        char path[T5_APP_PATH_MAX];
        s_id[0] = 0;
        FILE *f = t5_sys_path(path, sizeof path, ALARM_FILE) == T5_OK ? fopen(path, "r") : NULL;
        if (f) {
            size_t k = fread(s_id, 1, sizeof s_id - 1, f);
            s_id[k] = 0;
            fclose(f);
        }
        s_seen = t;
    }
    if (!s_id[0]) return false;
    snprintf(id, n, "%s", s_id);
    if (when) *when = t;
    return true;
}

int t5_apps_list(t5_app_entry_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < s_nbuiltins && n < max; i++, n++) {
        t5_app_entry_t *e = &out[n];
        memset(e, 0, sizeof *e);
        snprintf(e->id, sizeof e->id, "%s", s_builtins[i]->id);
        snprintf(e->name, sizeof e->name, "%s", s_builtins[i]->name ? s_builtins[i]->name : s_builtins[i]->id);
        strcpy(e->version, "builtin");
        e->builtin = true;
        if (s_builtins[i]->icon) snprintf(e->icon, sizeof e->icon, "%s", s_builtins[i]->icon);
        e->perms = s_builtins[i]->perms;
    }
    int cap = max - n;
    if (cap > 0) {
        t5_pkg_info_t *infos = calloc((size_t)cap, sizeof *infos);
        if (infos) {
            int k = t5_pkg_list(infos, cap);
            for (int i = 0; i < k && i < cap; i++, n++) {
                t5_app_entry_t *e = &out[n];
                memset(e, 0, sizeof *e);
                snprintf(e->id, sizeof e->id, "%s", infos[i].manifest.id);
                snprintf(e->name, sizeof e->name, "%s", infos[i].manifest.name);
                snprintf(e->version, sizeof e->version, "%s", infos[i].manifest.version);
                e->perms = infos[i].manifest.perms;
            }
            free(infos);
        }
    }
    for (int i = 0; i < n; i++) {
        t5_app_entry_t *e = &out[i];
        e->running = s_fg && !strcmp(s_fg->id, e->id);
        t5_crash_rec_t *r = t5_crash_find(&s_db, e->id);
        e->crashes = r ? r->consecutive : 0;
        e->autolaunch_blocked = !t5_crash_autolaunch_allowed(&s_db, e->id);
    }
    return n;
}

t5_err_t t5_apps_usage(t5_app_usage_t *out)
{
    apps_lock();
    t5_app_t *a = s_fg;
    if (!a) {
        apps_unlock();
        return T5_ERR_STATE;
    }
    out->heap_bytes = a->trk.heap_bytes;
    out->heap_peak = a->trk.heap_peak;
    out->heap_blocks = a->trk.heap_blocks;
    for (int k = 0; k < T5_RES_KIND_COUNT; k++) out->res[k] = a->trk.counts[k];
    out->dropped = a->trk.dropped;
    out->denied_symbols = a->denied_symbols;
    apps_unlock();
    return T5_OK;
}

typedef struct {
    char *buf;
    size_t n, len;
    int missing;
} missing_t;

static bool check_sym(const char *name, bool weak, void *arg)
{
    missing_t *m = arg;
    if (weak || t5_abi_find(name)) return true;
    m->missing++;
    if (m->buf && m->len + strlen(name) + 3 < m->n) {
        m->len += (size_t)snprintf(m->buf + m->len, m->n - m->len, "%s%s", m->len ? ", " : "", name);
    }
    return true;
}

t5_err_t t5_apps_check_abi(const uint8_t *elf, size_t len, char *missing, size_t n)
{
    missing_t m = { missing, n, 0, 0 };
    if (missing && n) missing[0] = 0;
    T5_RETURN_ON_ERR(t5_elfsym_undefined(elf, len, check_sym, &m));
    if (!t5_elfsym_defines(elf, len, T5_APP_ENTRY_SYMBOL)) {
        if (missing && n) snprintf(missing, n, "no %s (missing T5_APP?)", T5_APP_ENTRY_SYMBOL);
        return T5_ERR_INVALID_ARG;
    }
    return m.missing ? T5_ERR_NOT_SUPPORTED : T5_OK;
}

static t5_err_t check_package_abi(const char *path, const t5_manifest_t *man, char *err, size_t errlen)
{
    t5_zip_t *z;
    T5_RETURN_ON_ERR(t5_zip_open(path, &z));
    const t5_zip_entry_t *e = t5_zip_find(z, man->entry);
    t5_err_t r = T5_ERR_NOT_FOUND;
    uint8_t *buf = e ? malloc(e->size) : NULL;
    size_t len = 0;
    if (buf && t5_zip_extract_mem(z, e, buf, e->size, &len) == T5_OK) {
        char missing[200];
        r = t5_apps_check_abi(buf, len, missing, sizeof missing);
        if (r == T5_ERR_NOT_SUPPORTED) snprintf(err, errlen, "needs symbols this OS doesn't export: %s", missing);
        else if (r != T5_OK) snprintf(err, errlen, "not a Tab5 app ELF: %s", missing[0] ? missing : t5_err_str(r));
    } else {
        snprintf(err, errlen, "can't read %s from the package", man->entry);
    }
    free(buf);
    t5_zip_close(z);
    return r;
}

t5_pkg_dest_t t5_apps_dest_parse(const char *s)
{
    if (s && !strcmp(s, "sd")) return T5_PKG_DEST_SD;
    if (s && !strcmp(s, "data")) return T5_PKG_DEST_DATA;
    return T5_PKG_DEST_AUTO;
}

t5_err_t t5_apps_install(const char *path, t5_pkg_dest_t dest, t5_manifest_t *out, char *err, size_t errlen)
{
    t5_manifest_t *man = malloc(sizeof *man);
    t5_pkg_info_t *info = malloc(sizeof *info);
    t5_err_t e = (man && info) ? T5_OK : T5_ERR_NO_MEM;
    if (e == T5_OK) e = t5_pkg_inspect(path, man, err, errlen);
    if (e == T5_OK && man->runtime == T5_RUNTIME_NATIVE) e = check_package_abi(path, man, err, errlen);
    if (e == T5_OK) e = stop_running(man->id, err, errlen);
    if (e == T5_OK) e = t5_pkg_install(path, dest, info, err, errlen);
    if (e == T5_OK) {
        t5_crash_forget(&s_db, man->id);
        db_save();
        t5_ev_app_t ev = { 0 };
        ev_id(&ev, man->id);
        t5_event_post(T5_EV_APP_INSTALLED, &ev, sizeof ev);
        T5_LOGI(TAG, "installed %s %s to %s", man->id, man->version, info->dir);
        apps_home_refresh();
        if (out) *out = *man;
    }
    free(man);
    free(info);
    return e;
}

t5_err_t t5_apps_remove(const char *id, bool keep_data)
{
    if (s_fg && !strcmp(s_fg->id, id)) return T5_ERR_BUSY;
    if (find_builtin(id)) return T5_ERR_DENIED;
    T5_RETURN_ON_ERR(t5_pkg_remove(id, keep_data));
    t5_crash_forget(&s_db, id);
    db_save();
    t5_ev_app_t ev = { 0 };
    ev_id(&ev, id);
    t5_event_post(T5_EV_APP_REMOVED, &ev, sizeof ev);
    apps_home_refresh();
    return T5_OK;
}

static void suspend_on_ui(void *p)
{
    (void)p;
    t5_app_t *a = s_fg;
    if (a && a->state == APP_RESUMED) {
        call(a, a->desc->on_suspend);
        a->state = APP_SUSPENDED;
    }
}

static void resume_on_ui(void *p)
{
    (void)p;
    t5_app_t *a = s_fg;
    if (a && a->state == APP_SUSPENDED) {
        call(a, a->desc->on_resume);
        a->state = APP_RESUMED;
    }
}

static t5_err_t apps_suspend(void *arg)
{
    (void)arg;
    /* on_suspend gets 500 ms (plan 2.3); the power service logs overruns.
     * Not t5_ui_sync: the UI has been paused since Screen off, so a queued
     * call wouldn't run. */
    return t5_ui_call_paused(suspend_on_ui, NULL, 500);
}

static t5_err_t apps_resume(void *arg)
{
    (void)arg;
    return t5_ui_async(resume_on_ui, NULL);
}

static void on_rotate(t5_rotation_t r, void *arg)
{
    (void)arg;
    t5_app_t *a = s_fg;
    if (a && a->desc->on_rotate && a->man.orientation == T5_ORIENT_ANY) a->desc->on_rotate(a, t5_rot_degrees(r));
}

t5_err_t t5_app_schedule_alarm(t5_app_t *app, time_t utc)
{
    if (!valid(app)) return T5_ERR_INVALID_ARG;
    if (!(app->man.perms & T5_PERM_BACKGROUND_WAKE) || !app->man.background) return T5_ERR_DENIED;
    char path[T5_APP_PATH_MAX];
    T5_RETURN_ON_ERR(t5_sys_path(path, sizeof path, ALARM_FILE));
    T5_RETURN_ON_ERR(t5_storage_write_atomic(path, app->id, strlen(app->id) + 1));
    return t5_power_schedule_wake(utc, T5_WAKE_FLAG_BACKGROUND);
}

typedef struct {
    t5_app_t *app;
    bool done;
} alarm_run_t;
/* Given when on_alarm returns or the app faults. */
static SemaphoreHandle_t s_alarm_end;

static void alarm_task(void *p)
{
    alarm_run_t *r = p;
    if (r->app->desc->on_alarm) r->app->desc->on_alarm(r->app);
    __atomic_store_n(&r->done, true, __ATOMIC_RELEASE);
    xSemaphoreGive(s_alarm_end);
    /* Stay alive until the runner deletes us, so the handle stays valid. */
    for (;;) vTaskSuspend(NULL);
}

t5_err_t t5_apps_run_alarm(const char *id)
{
    if (!s_alarm_end) return T5_ERR_STATE;
    if (!t5_crash_autolaunch_allowed(&s_db, id)) return T5_ERR_DENIED;
    t5_app_t *a;
    T5_RETURN_ON_ERR(make_app(id, &a));
    if (!a->man.background) {
        destroy(a, false);
        return T5_ERR_DENIED;
    }
    a->headless = true;
    ensure_dir(a->data_dir);
    apps_hw_guard_set(a->id, true);
    if (!a->builtin && a->man.runtime == T5_RUNTIME_NATIVE) {
        t5_err_t e = apps_hw_elf_load(a, a->entry_path, &a->elf, &a->desc);
        if (e != T5_OK || !a->desc) {
            destroy(a, false);
            return e != T5_OK ? e : T5_ERR_NOT_SUPPORTED;
        }
    }
    xSemaphoreTake(s_alarm_end, 0);   /* one left over from an earlier run */
    s_headless = a;
    alarm_run_t *r = calloc(1, sizeof *r);
    t5_err_t e = r ? T5_OK : T5_ERR_NO_MEM;
    t5_task_t task = NULL;
    if (e == T5_OK) {
        r->app = a;
        void *saved = t5_app_ctx_get();
        t5_app_ctx_set(a);
        e = t5_task_create("t5_alarm", alarm_task, r, 8192, 4, 0, &task);
        t5_app_ctx_set(saved);
    }
    uint32_t limit = a->man.bg_max_seconds * 1000u;
    if (e == T5_OK) xSemaphoreTake(s_alarm_end, pdMS_TO_TICKS(limit));
    bool faulted = a->faulted;
    bool overrun = e == T5_OK && !__atomic_load_n(&r->done, __ATOMIC_ACQUIRE) && !faulted;
    if (overrun) {
        T5_LOGW(TAG, "%s on_alarm ran past %u s: stopped", a->id, (unsigned)(limit / 1000));
        t5_crash_record(&s_db, a->id, T5_CRASH_ALARM_OVERRUN, now_wall());
        db_save();
    }
    if (faulted) {
        T5_LOGE(TAG, "%s crashed in on_alarm (pc %s+0x%x, cause %u)", a->id,
                a->fault.pc >= a->text_lo && a->fault.pc < a->text_hi ? "app" : "os",
                (unsigned)(a->fault.pc - (a->fault.pc >= a->text_lo && a->fault.pc < a->text_hi ? a->text_lo : 0)),
                (unsigned)a->fault.cause);
        t5_crash_record(&s_db, a->id, T5_CRASH_PANIC, now_wall());
        db_save();
        overrun = true;          /* not a clean exit */
        e = T5_ERR_IO;
    }
    /* The runner task is tracked like any other app task: released first. */
    release_everything(a);
    s_headless = NULL;
    free(r);
    destroy(a, !overrun);
    return overrun ? T5_ERR_TIMEOUT : e;
}

/* At boot: a background alarm wake runs its app and goes back to soft off;
 * a normal wake may restore the last app. */
static void boot_actions(void)
{
    char id[T5_APP_ID_LEN] = "";
    size_t len = 0;
    if (s_safe) {
        T5_LOGW(TAG, "safe mode: no session restore, no background alarms");
        return;
    }
    if (t5_power_wake_reason() == T5_WAKE_RTC_ALARM && !t5_power_should_restore_session()) {
        char path[T5_APP_PATH_MAX];
        FILE *f = t5_sys_path(path, sizeof path, ALARM_FILE) == T5_OK ? fopen(path, "r") : NULL;
        if (f) {
            len = fread(id, 1, sizeof id - 1, f);
            fclose(f);
            id[len] = 0;
            remove(path);
        }
        if (id[0]) {
            T5_LOGI(TAG, "background alarm for %s", id);
            t5_err_t e = t5_apps_run_alarm(id);
            T5_LOGI(TAG, "alarm run: %s", t5_err_str(e));
            /* Background work done: back to soft off (design, "Power API"). */
            T5_LOG_ON_ERR(TAG, t5_power_soft_off(T5_OFF_IDLE, false));
            return;
        }
    }
    if (!t5_power_should_restore_session()) return;
    const char *last = app_session_restored();
    if (!last[0]) return;
    if (!strcmp(last, s_crashed)) {
        /* Restoring it would likely crash or hang again straight away. */
        T5_LOGW(TAG, "not restoring %s: it was running when the device reset", last);
        app_session_set(NULL);
        return;
    }
    if (!t5_crash_autolaunch_allowed(&s_db, last)) {
        T5_LOGW(TAG, "not restoring %s: it crashed %d times in a row", last, T5_CRASH_BLOCK_AFTER);
        return;
    }
    T5_LOGI(TAG, "restoring %s", last);
    s_restoring = true;      /* read by launch_fg on the UI task */
    T5_LOG_ON_ERR(TAG, t5_apps_launch(last));
}

static uint64_t du(const char *path, int depth)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return (uint64_t)st.st_size;
    if (depth > 6) return 0;
    uint64_t total = 0;
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *e;
    char sub[256];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (snprintf(sub, sizeof sub, "%.190s/%.60s", path, e->d_name) >= (int)sizeof sub) continue;
        total += du(sub, depth + 1);
    }
    closedir(d);
    return total;
}

t5_err_t t5_apps_info(const char *id, t5_app_info_t *out)
{
    if (!id || !out) return T5_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    t5_app_entry_t *list = calloc(T5_APPS_MAX, sizeof *list);
    if (!list) return T5_ERR_NO_MEM;
    int n = t5_apps_list(list, T5_APPS_MAX);
    t5_err_t e = T5_ERR_NOT_FOUND;
    for (int i = 0; i < n; i++) {
        if (!strcmp(list[i].id, id)) {
            out->entry = list[i];
            e = T5_OK;
        }
    }
    free(list);
    if (e != T5_OK) return e;
    if (out->entry.builtin) {
        snprintf(out->runtime, sizeof out->runtime, "built-in");
        T5_RETURN_ON_ERR(t5_pkg_data_dir(id, out->data_dir, sizeof out->data_dir));
    } else {
        t5_pkg_info_t *info = malloc(sizeof *info);
        if (!info) return T5_ERR_NO_MEM;
        if (t5_pkg_find(id, info) == T5_OK) {
            snprintf(out->install_dir, sizeof out->install_dir, "%s", info->dir);
            snprintf(out->data_dir, sizeof out->data_dir, "%s", info->data_dir);
            out->install_bytes = info->size;
            out->on_sd = info->root == T5_PKG_DEST_SD;
            out->install_time = info->install_time;
            snprintf(out->runtime, sizeof out->runtime, "%s", t5_runtime_name(info->manifest.runtime));
        }
        free(info);
        T5_IGNORE(t5_pkg_grants_load(id, &out->granted, &out->denied));   /* 0/0 on failure */
    }
    out->data_bytes = du(out->data_dir, 0);
    apps_lock();
    t5_crash_rec_t *r = t5_crash_find(&s_db, id);
    if (r) {
        out->crashes_total = r->total;
        out->last_crash_kind = r->last_kind;
        out->last_crash_time = r->last_time;
        out->has_dump = r->has_dump;
    }
    apps_unlock();
    if (out->has_dump && t5_apps_coredump_path(id, out->dump_path, sizeof out->dump_path) != T5_OK) {
        out->has_dump = false;   /* the record went away under us */
    }
    return T5_OK;
}

t5_err_t t5_apps_set_grant(const char *id, uint32_t perm, bool granted)
{
    if (find_builtin(id)) return T5_ERR_NOT_SUPPORTED;
    uint32_t g = 0, d = 0;
    T5_IGNORE(t5_pkg_grants_load(id, &g, &d));   /* 0/0 on failure */
    if (granted) {
        g |= perm;
        d &= ~perm;
    } else {
        d |= perm;
        g &= ~perm;
    }
    apps_lock();
    if (s_fg && !strcmp(s_fg->id, id)) {
        s_fg->granted = g;
        s_fg->denied = d;
    }
    apps_unlock();
    return t5_pkg_grants_save(id, g, d);
}

t5_err_t t5_apps_crash_reset(const char *id)
{
    apps_lock();
    t5_crash_forget(&s_db, id);
    apps_unlock();
    db_save();
    apps_home_refresh();
    return T5_OK;
}

#define ENERGY_PERIOD_MS 5000

void apps_energy_sample(void)
{
    t5_battery_status_t b;
    t5_battery_status(&b);
    if (!b.ready || b.usb || b.mw <= 0) return;      /* on USB the pack current says nothing about the load */
    t5_power_state_t ps = t5_power_state();
    char id[T5_ENERGY_ID_LEN];
    apps_lock();
    if (ps >= T5_PWR_SCREEN_OFF) snprintf(id, sizeof id, "%s", T5_ENERGY_SCREEN_OFF);
    else if (s_fg) snprintf(id, sizeof id, "%.47s", s_fg->id);
    else snprintf(id, sizeof id, "%s", T5_ENERGY_LAUNCHER);
    t5_energy_add(&s_energy, id, b.mw, ENERGY_PERIOD_MS);
    apps_unlock();
}

int t5_apps_energy(t5_energy_rec_t *out, int max)
{
    apps_lock();
    int n = t5_energy_sorted(&s_energy, out, max);
    apps_unlock();
    return n;
}

void t5_apps_energy_reset(void)
{
    apps_lock();
    t5_energy_init(&s_energy);
    s_energy.seq = s_energy_saved_seq + 1;   /* dirty: saved next time */
    apps_unlock();
}

static t5_err_t energy_save(void *arg)
{
    (void)arg;
    apps_lock();
    if (s_energy.seq == s_energy_saved_seq) {
        apps_unlock();
        return T5_OK;
    }
    t5_energy_db_t *copy = malloc(sizeof *copy);
    if (!copy) {
        apps_unlock();
        return T5_ERR_NO_MEM;
    }
    *copy = s_energy;
    apps_unlock();
    char path[T5_APP_PATH_MAX];
    t5_err_t e = t5_sys_path(path, sizeof path, ENERGY_DB);
    if (e == T5_OK) e = t5_storage_blob_write(path, T5_ENERGY_DB_VERSION, copy, sizeof *copy);
    if (e == T5_OK) s_energy_saved_seq = copy->seq;
    free(copy);
    return e;
}

static void energy_load(void)
{
    size_t len = 0;
    t5_energy_init(&s_energy);
    char path[T5_APP_PATH_MAX];
    if (t5_sys_path(path, sizeof path, ENERGY_DB) != T5_OK ||
        t5_storage_blob_read(path, T5_ENERGY_DB_VERSION, &s_energy, sizeof s_energy, &len) != T5_OK ||
        len != sizeof s_energy || s_energy.version != T5_ENERGY_DB_VERSION) {
        t5_energy_init(&s_energy);
    }
    s_energy_saved_seq = s_energy.seq;
}

static bool is_apps_task(const t5_app_t *a, void *task);

void apps_restart_after_anr(t5_app_t *app)
{
    /* Not stuck in the app at all, but waiting for the UI lock that one of
     * the app's tasks holds and isn't giving back: end that task, which
     * releases the lock and closes the app. */
    void *holder = t5_ui_lock_holder();
    if (valid(app) && holder && holder != t5_ui_task_handle() && is_apps_task(app, holder) &&
        t5_fault_interrupt_task(holder, 300) == T5_OK) {
        T5_LOGW(TAG, "%s: a task held the UI lock; stopped it", app->id);
        return;
    }
    /* The UI task is stuck in the app. If it can be moved out of the app's
     * code (fault.h), the UI task unwinds and ui_unwound() closes the app;
     * the device keeps running. */
    t5_err_t e = t5_fault_interrupt_task(t5_ui_task_handle(), 300);
    if (e == T5_OK) {
        T5_LOGW(TAG, "%s stopped without a restart", valid(app) ? app->id : "app");
        return;
    }
    /* Stuck where it can't be left safely (OS code with no app on the
     * call chain, a critical section, a frame being rendered): record
     * the kind for the next boot and restart to the launcher. */
    T5_LOGW(TAG, "can't stop the UI task (%s): restarting", t5_err_str(e));
    apps_hw_guard_pending(T5_CRASH_ANR);
    T5_LOG_ON_ERR(TAG, t5_autosave_now());
    apps_hw_restart();
}

static bool is_apps_task(const t5_app_t *a, void *task)
{
    /* Exception context: read without the lock (a racing edit can at worst
     * make us miss the task, and then the fault panics as before). */
    return t5_tracker_holds(&a->trk, T5_RES_TASK, task);
}

static volatile uint32_t s_guess_ms;      /* last unprovable fault put on an app */

/* Exception context (or the watchdog, for a hang): no locks, no logging. */
static t5_blame_t blame(const t5_blame_args_t *args, t5_fault_info_t *info)
{
    t5_app_t *cands[2] = { s_fg, s_headless };
    void *ui = t5_ui_task_handle();
    for (int i = 0; i < 2; i++) {
        t5_app_t *a = cands[i];
        if (!valid(a) || a->text_hi <= a->text_lo || a->faulted) continue;
        bool on_ui = args->task == ui;
        t5_iso_fault_t f = {
            .text_lo = a->text_lo,
            .text_hi = a->text_hi,
            .apps_task = !on_ui && !args->point && is_apps_task(a, args->task),
            .has_point = args->point != NULL,
            .point_ok = args->point && (!args->point->app || args->point->app == a),
            .unwind_safe = !on_ui || t5_ui_unwind_safe(),
            .pc = info->pc,
            .ra = info->ra,
            .stack = (const uint32_t *)args->scan_lo,
            .words = (args->scan_hi - args->scan_lo) / sizeof(uint32_t),
            .working_for_app = args->task_ctx == a,
            .data_fault = info->cause >= 4 && info->cause <= 7,
            .guess_ok = !s_guess_ms || t5_time_ms() - s_guess_ms >= T5_ISO_GUESS_GAP_MS,
        };
        /* A UI task that isn't working for this app (system UI between
         * apps) isn't the app's to unwind. */
        if (on_ui && args->task_ctx != a) continue;
        switch (t5_iso_blame(&f)) {
        case T5_ISO_UNWIND: info->app = a; return T5_BLAME_UNWIND;
        case T5_ISO_UNWIND_GUESS:
            info->app = a;
            info->guess = true;
            s_guess_ms = t5_time_ms() | 1;
            return T5_BLAME_UNWIND;
        case T5_ISO_END_TASK: info->app = a; return T5_BLAME_END_TASK;
        default: break;
        }
    }
    return T5_BLAME_NONE;
}

bool t5_apps_modal_dismiss(void)
{
    return picker_dismiss();
}

void apps_fs_add_pick(t5_app_t *a, const char *path, bool write)
{
    apps_lock();
    t5_fs_pick_t *p = &a->picks[a->pick_next];
    snprintf(p->path, sizeof p->path, "%s", path);
    p->write = write;
    a->pick_next = (uint8_t)((a->pick_next + 1) % T5_FS_MAX_PICKS);
    if (a->npicks < T5_FS_MAX_PICKS) a->npicks++;
    apps_unlock();
}

bool t5_app_fs_check(t5_app_t *a, const char *path, bool write)
{
    if (!valid(a) || a->builtin) return true;
    char abs[T5_FS_PATH_MAX];
    if (!t5_fs_normalise(path, abs, sizeof abs)) return false;
    t5_fs_scope_t scope = { a->data_dir, a->install_dir, a->picks, a->npicks };
    uint32_t need = 0;
    apps_lock();
    t5_fs_verdict_t v = t5_fs_check(&scope, abs, write, &need);
    apps_unlock();
    if (v == T5_FS_ALLOW) return true;
    if (v == T5_FS_DENY) return false;
    return perm_check_any(a, need) == T5_OK;
}

bool t5_app_fs_quota_applies(t5_app_t *a, const char *path)
{
    return valid(a) && path && t5_fs_under(path, a->data_dir);
}

bool t5_app_fs_room(t5_app_t *a, size_t n)
{
    if (!valid(a) || a->builtin) return true;
    if (a->fs_used >= 0 && t5_fs_quota_ok((uint64_t)a->fs_used, n)) {
        /* Now and then, that the OS itself still has somewhere to write. */
        a->fs_since_check += (uint32_t)n;
        if (a->fs_since_check >= 64u * 1024u) {
            a->fs_since_check = 0;
            uint64_t total = 0, used = 0;
            if (t5_storage_data_usage(&total, &used) == T5_OK && total - used < T5_FS_DATA_RESERVE) return false;
        }
        a->fs_used += (int64_t)n;
        return true;
    }
    /* Over by the running count, which only grows: count it properly.
     * A file still being written shows its size only once it's synced. */
    for (size_t i = 0; i < sizeof a->qfiles / sizeof a->qfiles[0]; i++) {
        if (a->qfiles[i]) {
            fflush(a->qfiles[i]);
            fsync(fileno(a->qfiles[i]));
        }
    }
    a->fs_used = (int64_t)du(a->data_dir, 0);
    if (!t5_fs_quota_ok((uint64_t)a->fs_used, n)) return false;
    a->fs_used += (int64_t)n;
    return true;
}

/* Apps, built-in ones included, open only the ports their manifest lists:
 * a script run in Terminal or Editor gets those apps' lists. */
static bool port_access(const char *name, char *why, size_t whylen)
{
    const t5_app_t *a = t5_app_self();
    if (!a) return true;
    for (int i = 0; i < a->man.nports; i++) {
        if (!strcmp(a->man.ports[i], name)) return true;
    }
    T5_LOGW(TAG, "%s: port %s not in its manifest", a->id, name);
    if (why && whylen) snprintf(why, whylen, "port %s is not declared in the app's manifest", name);
    return false;
}

t5_app_t *apps_by_instance(uint32_t instance)
{
    t5_app_t *c[2] = { s_fg, s_headless };
    for (int i = 0; i < 2; i++) {
        if (valid(c[i]) && c[i]->instance == instance && !c[i]->exiting) return c[i];
    }
    return NULL;
}

static void kill_fg(void *p);

/* On the faulting task, just after it left the fault. Keep it light: the
 * stack may be nearly used up. Logging and the crash record happen in
 * kill_fg() on the UI task. */
static void fault_report(const t5_fault_info_t *info, bool ends)
{
    t5_app_t *a = info->app;
    if (!valid(a)) return;
    a->fault = *info;
    a->faulted = true;
    a->exiting = true;
    /* Whichever task this is, a call abandoned half way must not take OS
     * locks with it: a free() of a bad pointer faults with the app table
     * locked. (The UI task's own lock is released after its unwind.) */
    if (!t5_ui_on_ui_task()) t5_ui_release_if_held();
    t5_mutex_release_all(s_lock);
    if (ends) {
        apps_lock();
        t5_tracker_remove(&a->trk, info->task);
        apps_unlock();
    }
    /* On the UI task, ui_unwound() takes over after the unwind. */
    if (a == s_headless) xSemaphoreGive(s_alarm_end);
    if (!t5_ui_on_ui_task() && a == s_fg) {
        if (t5_ui_async(kill_fg, a) != T5_OK) T5_LOGE(TAG, "%s crashed; can't queue its exit", a->id);
    }
}

/* UI task: get rid of a foreground app that crashed, hung or ran the device
 * out of memory, and record it. */
static void kill_fg(void *p)
{
    t5_app_t *a = p;
    if (!a || a != s_fg) return;
    t5_crash_kind_t k = a->oom ? T5_CRASH_OOM : a->fault.cause == T5_FAULT_HANG ? T5_CRASH_ANR : T5_CRASH_PANIC;
    if (a->faulted && a->fault.cause == T5_FAULT_STACK) {
        T5_LOGE(TAG, "%s ran out of stack (sp 0x%08x, %u survived since boot)", a->id, (unsigned)a->fault.addr,
                (unsigned)t5_fault_recovered());
    } else if (a->faulted) {
        const t5_fault_info_t *f = &a->fault;
        bool in_app = f->pc >= a->text_lo && f->pc < a->text_hi;
        bool ra_app = f->ra >= a->text_lo && f->ra < a->text_hi;
        if (f->guess) {
            T5_LOGW(TAG, "%s: the fault below is in OS code with no app frame on the stack; blamed on the "
                    "app (its last call into the OS); another one within %u s reboots", a->id,
                    T5_ISO_GUESS_GAP_MS / 1000);
        }
        T5_LOGE(TAG, "%s %s: pc %s+0x%x ra %s+0x%x addr 0x%08x cause %u (%u survived since boot)", a->id,
                t5_crash_kind_name(k), in_app ? "app" : "os", (unsigned)(f->pc - (in_app ? a->text_lo : 0)),
                ra_app ? "app" : "os", (unsigned)(f->ra - (ra_app ? a->text_lo : 0)), (unsigned)f->addr,
                (unsigned)f->cause, (unsigned)t5_fault_recovered());
    } else {
        T5_LOGW(TAG, "%s %s", a->id, t5_crash_kind_name(k));
    }
    apps_lock();
    t5_crash_rec_t *r = t5_crash_record(&s_db, a->id, k, now_wall());
    apps_unlock();
    db_save();
    t5_ev_app_t ev = { .err = (int32_t)k };
    ev_id(&ev, a->id);
    t5_event_post(T5_EV_APP_CRASHED, &ev, sizeof ev);
    home_notice(k == T5_CRASH_ANR ? T5_APPS_NOTICE_HUNG : k == T5_CRASH_OOM ? T5_APPS_NOTICE_LOW_MEMORY
                                                                           : T5_APPS_NOTICE_CRASHED, a);
    if (r && r->consecutive >= T5_CRASH_BLOCK_AFTER) {
        T5_LOGW(TAG, "%s: %u in a row, auto-launch blocked", a->id, r->consecutive);
    }
    /* Out of memory: the app's code is fine, let it save. Crashed or hung:
     * don't run any more of it. */
    exit_fg_ex(false, !a->faulted);
}

/* t5_ui's unwind hook: UI task, UI lock held, the iteration abandoned. */
static void ui_unwound(const void *p)
{
    const t5_fault_info_t *info = p;
    t5_app_t *a = info ? info->app : NULL;
    t5_app_ctx_set(NULL);
    if (a && a == s_fg) {
        kill_fg(a);
    } else {
        /* Already gone (it faulted in on_destroy, say) */
        T5_LOGW(TAG, "UI task unwound from an app that is no longer running");
        if (s_fg) t5_app_ctx_set(s_fg);
    }
}

static void oom_async(void *p)
{
    t5_app_t *a = p;
    if (a != s_fg || !valid(a)) return;
    a->oom = true;
    kill_fg(a);
}

void apps_close_for_memory(void)
{
    apps_lock();
    t5_app_t *a = s_fg;
    /* Only installed apps: built-ins are part of the OS. */
    bool target = valid(a) && !a->builtin && !a->exiting;
    apps_unlock();
    if (!target) return;
    T5_LOGW(TAG, "memory critically low (PSRAM %u KB, internal %u KB free) with %s open (heap %u KB): closing it",
            (unsigned)(apps_hw_free_psram() / 1024), (unsigned)(apps_hw_free_l2() / 1024), a->id,
            (unsigned)(a->trk.heap_bytes / 1024));
    if (t5_ui_async(oom_async, a) != T5_OK) T5_LOGE(TAG, "can't queue %s's exit", a->id);
}

#define HOG_MAX T5_APP_MAX_TASKS
static struct {
    void *task;
    uint64_t last;
    uint8_t strikes;
    bool throttled;
} s_hog[HOG_MAX];
_Static_assert(HOG_MAX <= 32, "seen is a 32-bit mask");
static uint32_t s_hog_t_ms;

void apps_check_hogs(void)
{
    uint32_t now = t5_time_ms();
    uint64_t wall = (uint64_t)(now - s_hog_t_ms) * 1000u;
    s_hog_t_ms = now;
    uint32_t seen = 0;
    apps_lock();
    t5_app_t *a = valid(s_fg) && !s_fg->builtin ? s_fg : NULL;
    void *tasks[HOG_MAX];
    int nt = a ? t5_tracker_list(&a->trk, T5_RES_TASK, tasks, HOG_MAX) : 0;
    for (int i = 0; i < nt && i < HOG_MAX; i++) {
        void *task = tasks[i];
        /* Tracked tasks are alive while we hold the lock: they untrack
         * themselves under it before they go away. */
        uint64_t rt = apps_hw_task_runtime_us(task);
        int k = -1;
        for (int j = 0; j < HOG_MAX && k < 0; j++) {
            if (s_hog[j].task == task) k = j;
        }
        if (k < 0) {
            for (int j = 0; j < HOG_MAX && k < 0; j++) {
                if (!s_hog[j].task) k = j;
            }
            if (k < 0) continue;
            s_hog[k].task = task;
            s_hog[k].last = rt;
            s_hog[k].strikes = 0;
            s_hog[k].throttled = false;
            seen |= 1u << k;
            continue;
        }
        seen |= 1u << k;
        /* The counter may be 32 bits wide and wrap. */
        uint64_t ran = rt >= s_hog[k].last ? rt - s_hog[k].last : (uint32_t)(rt - s_hog[k].last);
        s_hog[k].last = rt;
        if (!s_hog[k].throttled && t5_iso_hog(&s_hog[k].strikes, ran, wall)) {
            s_hog[k].throttled = true;
            apps_hw_task_set_prio(task, T5_ISO_HOG_PRIO);
            T5_LOGW(TAG, "%s: a task kept a core busy for %d s; moved to priority %d", a->id,
                    T5_ISO_HOG_STRIKES, T5_ISO_HOG_PRIO);
        }
    }
    apps_unlock();
    for (int j = 0; j < HOG_MAX; j++) {
        if (!(seen & (1u << j))) s_hog[j].task = NULL;     /* gone, or no app */
    }
}

/* A hog at the lowest priority still leaves nothing for the idle task on its
 * core, which frees deleted tasks and feeds the watchdog: take it off the
 * CPU one poll in every ten. */
void apps_throttle_tick(void)
{
    static uint8_t n;
    n = (uint8_t)((n + 1) % 10);
    if (n > 1) return;
    apps_lock();
    t5_app_t *a = valid(s_fg) && !s_fg->builtin ? s_fg : NULL;
    for (int j = 0; a && j < HOG_MAX; j++) {
        if (s_hog[j].task && s_hog[j].throttled && t5_tracker_has(&a->trk, s_hog[j].task)) {
            apps_hw_task_pause(s_hog[j].task, n == 0);
        }
    }
    apps_unlock();
}

static void ui_setup(void *p)
{
    (void)p;
    md_init(lv_display_get_default());
    lv_obj_add_event_cb(lv_layer_top(), top_child_created, LV_EVENT_CHILD_CREATED, NULL);
    lv_screen_load(apps_home_screen());
    t5_ui_reveal();     /* the launcher is the first thing anyone sees */
}

bool t5_apps_ready(void)
{
    return s_inited;
}

t5_err_t t5_apps_boot_check(void)
{
    crash_check();
    return T5_OK;
}

t5_err_t t5_apps_init(void)
{
    if (s_inited) return T5_OK;
    if (!t5_ui_ready() || !s_home || !s_home->screen) return T5_ERR_STATE;
    s_lock = t5_mutex_create();
    s_alarm_end = xSemaphoreCreateBinary();
    if (!s_lock || !s_alarm_end) return T5_ERR_NO_MEM;
    t5_pkg_recover();
    crash_check();
    t5_res_set_hooks(&s_hooks);
    t5_core_set_task_hook(task_created);
    t5_core_set_task_exit_hook(task_exited);
    T5_RETURN_ON_ERR(t5_ui_sync(ui_setup, NULL, 5000));
    t5_ui_set_rotate_hook(on_rotate, NULL);
    const t5_suspend_hook_t hook = {
        .name = "apps", .prio = T5_SUSPEND_PRIO_APPS, .budget_ms = 500,
        .suspend = apps_suspend, .resume = apps_resume,
    };
    T5_LOG_ON_ERR(TAG, t5_power_register_hook(&hook));
    t5_core_set_perm_check(perm_check_self);
    t5_core_set_path_check(apps_path_check);
    t5_ports_set_access_check(port_access);
    /* Isolation (isolate.h): app heaps away from the OS's internal RAM,
     * app faults and hangs survived. */
    apps_hw_heap_install();
    picker_init();
    t5_app_lv_mem_install();
    t5_ui_set_unwind_hook(ui_unwound);
    t5_fault_set_hooks(blame, fault_report);
    energy_load();
    T5_LOG_ON_ERR(TAG, t5_autosave_register("energy", energy_save, NULL));
    app_session_load();
    T5_LOG_ON_ERR(TAG, t5_autosave_register("session", app_session_save, NULL));
    apps_hw_periodic(apps_energy_sample, ENERGY_PERIOD_MS);
    s_inited = true;
    T5_RETURN_ON_ERR(anr_init());
    T5_LOGI(TAG, "app runtime up: API level %d, %u exported symbols, %d built-in apps", T5_API_LEVEL,
            (unsigned)t5_abi_size(), s_nbuiltins);
    boot_actions();
    return T5_OK;
}
