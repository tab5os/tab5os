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
#include <string.h>
#include <sys/stat.h>
#include "lvgl.h"
#include "md/md.h"
#include "t5_app/abi.h"
#include "t5_app/apps.h"
#include "t5_app/elfsym.h"
#include "t5_core/event.h"
#include "t5_core/log.h"
#include "t5_pkg/pkg.h"
#include "t5_pkg/zip.h"
#include "t5_power/autosave.h"
#include "t5_power/power.h"
#include "t5_storage/t5_storage.h"
#include "t5_time/t5_time.h"
#include "t5_ui/orient.h"
#include "t5_ui/t5_ui.h"
#include "app_priv.h"
#include "apps_hw.h"

static const char *TAG = "t5.app";

#define MAX_BUILTINS 24
#define CRASH_DB T5_SYS_DIR "/crashes.bin"
#define DUMP_DIR T5_SYS_DIR "/dumps"
#define ALARM_FILE T5_SYS_DIR "/alarm_app"
#define SESSION_PREFIX "app:"
#define TASK_STOP_GRACE_MS 150

static t5_mutex_t s_lock;
static t5_app_t *s_fg;              /* foreground app */
static t5_app_t *s_headless;        /* app running on_alarm */
static const t5_builtin_app_t *s_builtins[MAX_BUILTINS];
static int s_nbuiltins;
static t5_crash_db_t s_db;
static bool s_inited;
static char s_crashed[64];          /* the app that crashed or hung on the last run */

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

static bool valid(const t5_app_t *a)
{
    return a && a->hdr.magic == T5_APP_CTX_MAGIC;
}

/* ---- tracking hooks --------------------------------------------------------- */
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

void apps_task_release(void *task)
{
    apps_hw_task_delete(task);
}

static void task_created(t5_task_t task, void *ctx)
{
    hook_add(ctx, T5_RES_TASK, task, apps_task_release);
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

/* ---- app API ----------------------------------------------------------------- */
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
    if (perm & (T5_PERM_CAMERA | T5_PERM_MIC)) return (app->granted & perm) == perm;
    return true;
}

/* ---- crash records --------------------------------------------------------------- */
static int64_t now_wall(void)
{
    return t5_time_valid() ? (int64_t)t5_time_now() : 0;
}

static void db_save(void)
{
    if (t5_crash_db_save(&s_db, CRASH_DB) != T5_OK) T5_LOGW(TAG, "can't save crash records");
}

bool t5_apps_autolaunch_allowed(const char *id)
{
    return t5_crash_autolaunch_allowed(&s_db, id);
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
    snprintf(path, n, DUMP_DIR "/%.40s.elf", id);
    return T5_OK;
}

/* At boot: was an app running when the device reset? */
static bool s_crash_checked;

static void crash_check(void)
{
    if (s_crash_checked) return;
    s_crash_checked = true;
    t5_crash_db_load(&s_db, CRASH_DB);
    t5_crash_boot_t b;
    apps_hw_guard_read(&b);
    apps_hw_guard_clear();
    t5_crash_kind_t k = t5_crash_classify(&b, t5_power_wake_reason() == T5_WAKE_CRASH);
    if (k == T5_CRASH_NONE) return;
    t5_crash_rec_t *r = t5_crash_record(&s_db, b.running, k, now_wall());
    snprintf(s_crashed, sizeof s_crashed, "%s", b.running);
    if (k == T5_CRASH_PANIC) {
        mkdir(T5_SYS_DIR, 0755);
        mkdir(DUMP_DIR, 0755);
        char path[96];
        snprintf(path, sizeof path, DUMP_DIR "/%.40s.elf", b.running);
        if (apps_hw_coredump_save(path)) r->has_dump = 1;
    }
    db_save();
    T5_LOGW(TAG, "%s %s (%u in a row)%s", b.running, t5_crash_kind_name(k), r->consecutive,
            r->consecutive >= T5_CRASH_BLOCK_AFTER ? ": auto-launch blocked" : "");
    t5_ev_app_t ev = { .err = (int32_t)k };
    ev_id(&ev, b.running);
    t5_event_post(T5_EV_APP_CRASHED, &ev, sizeof ev);
}

/* ---- finding apps -------------------------------------------------------------------- */
t5_err_t t5_apps_register_builtin(const t5_builtin_app_t *b)
{
    if (!b || !b->id || !b->desc) return T5_ERR_INVALID_ARG;
    if (s_nbuiltins >= MAX_BUILTINS) return T5_ERR_NO_MEM;
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
        a->man.orientation = (t5_orient_pref_t)b->orientation;
        for (int i = 0; b->ports && b->ports[i] && i < T5_MANIFEST_MAX_PORTS; i++) {
            snprintf(a->man.ports[i], sizeof a->man.ports[i], "%s", b->ports[i]);
            a->man.nports = i + 1;
        }
        snprintf(a->data_dir, sizeof a->data_dir, "/data/appdata/%.48s", id);
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
            free(a);
            return T5_ERR_NOT_SUPPORTED;   /* MicroPython: Phase 8 */
        }
        t5_pkg_grants_load(id, &a->granted, &a->denied);
    }
    t5_tracker_init(&a->trk, 0);
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

/* ---- tearing down ------------------------------------------------------------------------ */
static void ensure_dir(const char *path)
{
    /* data dir: /data/appdata/<id> */
    char tmp[160];
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

static void release_everything(t5_app_t *a)
{
    a->exiting = true;
    /* Tasks first, and politely: loops that check t5_app_running() get a
     * moment to finish (and release locks they hold) before the rest is
     * deleted. */
    for (uint32_t t = 0; t < TASK_STOP_GRACE_MS && t5_tracker_count(&a->trk, T5_RES_TASK); t += 10) {
        t5_delay_ms(10);
    }
    void *saved = t5_app_ctx_get();
    t5_app_ctx_set(a);          /* release paths report their removals for this app */
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

/* UI task. */
static void exit_fg(bool clean)
{
    t5_app_t *a = s_fg;
    if (!a) return;
    T5_LOGI(TAG, "exit %s", a->id);
    a->exiting = true;
    t5_app_ctx_set(a);
    if (a->state == APP_RESUMED) call(a, a->desc->on_suspend);
    a->state = APP_EXITING;
    call(a, a->desc->on_destroy);
    t5_app_ctx_set(NULL);
    lv_screen_load(launcher_screen());
    t5_ui_set_allowed_rotations(T5_ROT_ALL);
    s_fg = NULL;
    anr_app_changed(NULL);
    release_everything(a);
    t5_session_set(NULL, 0);
    destroy(a, clean);
    launcher_refresh();
}

static t5_err_t launch_fg(const char *id)
{
    if (s_fg) {
        if (!strcmp(s_fg->id, id)) return T5_OK;
        exit_fg(true);
    }
    t5_app_t *a;
    t5_err_t e = make_app(id, &a);
    if (e != T5_OK) {
        T5_LOGE(TAG, "launch %s: %s", id, t5_err_str(e));
        return e;
    }
    ensure_dir(a->data_dir);
    a->started_ms = t5_time_ms();
    apps_hw_guard_set(a->id, false);
    if (!a->builtin) {
        e = apps_hw_elf_load(a, a->entry_path, &a->elf, &a->desc);
        if (e == T5_OK && (!a->desc || a->desc->api < 1 || a->desc->api > T5_API_LEVEL)) e = T5_ERR_NOT_SUPPORTED;
        if (e != T5_OK) {
            T5_LOGE(TAG, "load %s: %s", a->entry_path, t5_err_str(e));
            destroy(a, false);
            return e;
        }
    }
    s_fg = a;
    anr_app_changed(a);
    a->state = APP_CREATED;
    t5_ui_set_allowed_rotations(allowed_rotations(a));
    /* The UI task now works for the app (see apps.h). */
    t5_app_ctx_set(a);
    a->screen = lv_obj_create(NULL);
    track_obj(a, a->screen);
    call(a, a->desc->on_create);
    if (a->exiting) {                 /* exited inside on_create */
        t5_app_ctx_set(NULL);
        return T5_OK;
    }
    lv_screen_load_anim(a->screen, LV_SCREEN_LOAD_ANIM_FADE_IN, MD_DUR_DIALOG_IN_MS, 0, false);
    call(a, a->desc->on_resume);
    a->state = APP_RESUMED;
    char sess[80];
    int n = snprintf(sess, sizeof sess, SESSION_PREFIX "%s", a->id);
    t5_session_set(sess, (size_t)n + 1);
    t5_ev_app_t ev = { 0 };
    ev_id(&ev, a->id);
    t5_event_post(T5_EV_APP_STARTED, &ev, sizeof ev);
    T5_LOGI(TAG, "started %s %s (%s)", a->id, a->man.version, a->builtin ? "built-in" : "native");
    return T5_OK;
}

/* ---- public runtime API (any task) ---------------------------------------------------------- */
typedef struct {
    char id[64];
    t5_err_t result;
} launch_req_t;

static void launch_async(void *p)
{
    launch_req_t *r = p;
    r->result = launch_fg(r->id);
}

static void launch_async_free(void *p)
{
    launch_async(p);
    free(p);
}

t5_err_t t5_apps_launch(const char *id)
{
    if (!s_inited || !id) return T5_ERR_STATE;
    /* Opening an app is the person using the device: wake the screen (the
     * UI doesn't run while it's off). */
    t5_power_user_activity();
    launch_req_t *r = calloc(1, sizeof *r);
    if (!r) return T5_ERR_NO_MEM;
    snprintf(r->id, sizeof r->id, "%s", id);
    t5_err_t e = t5_ui_async(launch_async_free, r);
    if (e != T5_OK) free(r);
    return e;
}

t5_err_t t5_apps_launch_wait(const char *id, uint32_t timeout_ms)
{
    if (!s_inited || !id) return T5_ERR_STATE;
    t5_power_user_activity();
    t5_delay_ms(50);                    /* let the power task bring the UI back */
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
    return t5_ui_sync(exit_async, NULL, timeout_ms);
}

void t5_apps_current(char *id, size_t n)
{
    apps_lock();
    snprintf(id, n, "%s", s_fg ? s_fg->id : "");
    apps_unlock();
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

/* ---- install (plan 5.6, 5.8) ------------------------------------------------------------------- */
typedef struct {
    char *buf;
    size_t n, len;
    int missing;
} missing_t;

static bool check_sym(const char *name, bool weak, void *arg)
{
    missing_t *m = arg;
    if (weak || t5_abi_find(name) || t5_abi_is_runtime_helper(name)) return true;
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

t5_err_t t5_apps_install(const char *path, int dest, char *err, size_t errlen)
{
    t5_manifest_t *man = malloc(sizeof *man);
    t5_pkg_info_t *info = malloc(sizeof *info);
    t5_err_t e = (man && info) ? T5_OK : T5_ERR_NO_MEM;
    if (e == T5_OK) e = t5_pkg_inspect(path, man, err, errlen);
    if (e == T5_OK && man->runtime == T5_RUNTIME_NATIVE) e = check_package_abi(path, man, err, errlen);
    if (e == T5_OK && s_fg && !strcmp(s_fg->id, man->id)) {
        snprintf(err, errlen, "%s is running; close it first", man->id);
        e = T5_ERR_BUSY;
    }
    if (e == T5_OK) e = t5_pkg_install(path, (t5_pkg_dest_t)dest, info, err, errlen);
    if (e == T5_OK) {
        /* Camera and mic are asked for on first use; nothing else needs an answer. */
        t5_crash_forget(&s_db, man->id);
        db_save();
        t5_ev_app_t ev = { 0 };
        ev_id(&ev, man->id);
        t5_event_post(T5_EV_APP_INSTALLED, &ev, sizeof ev);
        T5_LOGI(TAG, "installed %s %s to %s", man->id, man->version, info->dir);
        launcher_refresh();
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
    launcher_refresh();
    return T5_OK;
}

/* ---- power, rotation ------------------------------------------------------------------------------ */
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
    /* on_suspend gets 500 ms (plan 2.3); the power service logs overruns. */
    return t5_ui_sync(suspend_on_ui, NULL, 500);
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

/* ---- scheduled alarms (plan 5.10) ----------------------------------------------------------------- */
t5_err_t t5_app_schedule_alarm(t5_app_t *app, time_t utc)
{
    if (!valid(app)) return T5_ERR_INVALID_ARG;
    if (!(app->man.perms & T5_PERM_BACKGROUND_WAKE) || !app->man.background) return T5_ERR_DENIED;
    T5_RETURN_ON_ERR(t5_storage_write_atomic(ALARM_FILE, app->id, strlen(app->id) + 1));
    return t5_power_schedule_wake(utc, T5_WAKE_FLAG_BACKGROUND);
}

typedef struct {
    t5_app_t *app;
    volatile bool done;
} alarm_run_t;

static void alarm_task(void *p)
{
    alarm_run_t *r = p;
    if (r->app->desc->on_alarm) r->app->desc->on_alarm(r->app);
    r->done = true;
    /* Stay alive until the runner deletes us, so the handle stays valid. */
    for (;;) t5_delay_ms(1000);
}

t5_err_t t5_apps_run_alarm(const char *id)
{
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
    if (!a->builtin) {
        t5_err_t e = apps_hw_elf_load(a, a->entry_path, &a->elf, &a->desc);
        if (e != T5_OK || !a->desc) {
            destroy(a, false);
            return e != T5_OK ? e : T5_ERR_NOT_SUPPORTED;
        }
    }
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
    uint32_t limit = (a->man.bg_max_seconds ? a->man.bg_max_seconds : 30) * 1000u;
    uint32_t t0 = t5_time_ms();
    while (e == T5_OK && !r->done && t5_time_ms() - t0 < limit) t5_delay_ms(50);
    bool overrun = e == T5_OK && !r->done;
    if (overrun) {
        T5_LOGW(TAG, "%s on_alarm ran past %u s: stopped", a->id, (unsigned)(limit / 1000));
        t5_crash_record(&s_db, a->id, T5_CRASH_ALARM_OVERRUN, now_wall());
        db_save();
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
    char id[64] = "";
    size_t len = 0;
    if (t5_power_wake_reason() == T5_WAKE_RTC_ALARM && !t5_power_should_restore_session()) {
        FILE *f = fopen(ALARM_FILE, "r");
        if (f) {
            len = fread(id, 1, sizeof id - 1, f);
            fclose(f);
            id[len] = 0;
            remove(ALARM_FILE);
        }
        if (id[0]) {
            T5_LOGI(TAG, "background alarm for %s", id);
            t5_err_t e = t5_apps_run_alarm(id);
            T5_LOGI(TAG, "alarm run: %s", t5_err_str(e));
            /* Background work done: back to soft off (design, "Power API"). */
            t5_power_soft_off(T5_OFF_IDLE, false);
            return;
        }
    }
    if (!t5_power_should_restore_session()) return;
    char sess[80];
    if (t5_session_get(sess, sizeof sess, &len) != T5_OK || len < sizeof SESSION_PREFIX ||
        strncmp(sess, SESSION_PREFIX, strlen(SESSION_PREFIX))) {
        return;
    }
    sess[sizeof sess - 1] = 0;
    const char *last = sess + strlen(SESSION_PREFIX);
    if (!strcmp(last, s_crashed)) {
        /* Restoring it would likely crash or hang again straight away. */
        T5_LOGW(TAG, "not restoring %s: it was running when the device reset", last);
        t5_session_set(NULL, 0);
        return;
    }
    if (!t5_crash_autolaunch_allowed(&s_db, last)) {
        T5_LOGW(TAG, "not restoring %s: it crashed %d times in a row", last, T5_CRASH_BLOCK_AFTER);
        return;
    }
    T5_LOGI(TAG, "restoring %s", last);
    t5_apps_launch(last);
}

/* ---- ANR restart --------------------------------------------------------------------------------------- */
void apps_restart_after_anr(t5_app_t *app)
{
    /* The UI task is stuck in the app: nothing can run on it. Record the
     * kind for the next boot and restart to the launcher. */
    (void)app;
    apps_hw_guard_pending(T5_CRASH_ANR);
    t5_autosave_now();
    apps_hw_restart();
}

/* ---- init ------------------------------------------------------------------------------------------------ */
/* Plan 4.7: a full ripple only when the frame budget allows; a plain fade in
 * low-power mode or after a frame that took longer than one refresh. */
static bool ripple_affordable(void)
{
    if (t5_ui_low_power()) return false;
    t5_ui_stats_t s;
    t5_ui_get_stats(&s);
    return s.render_us_max + s.copy_us_avg < 16000;
}

static void ui_setup(void *p)
{
    (void)p;
    md_init(lv_display_get_default());
    md_ripple_set_probe(ripple_affordable);
    lv_obj_add_event_cb(lv_layer_top(), top_child_created, LV_EVENT_CHILD_CREATED, NULL);
    lv_screen_load(launcher_screen());
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
    if (!t5_ui_ready()) return T5_ERR_STATE;
    s_lock = t5_mutex_create();
    if (!s_lock) return T5_ERR_NO_MEM;
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
    t5_power_register_hook(&hook);
    s_inited = true;
    T5_RETURN_ON_ERR(anr_init());
    T5_LOGI(TAG, "app runtime up: API level %d, %u exported symbols, %d built-in apps", T5_API_LEVEL,
            (unsigned)t5_abi_count, s_nbuiltins);
    boot_actions();
    return T5_OK;
}
