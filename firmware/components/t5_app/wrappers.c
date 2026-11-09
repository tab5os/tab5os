/* SPDX-License-Identifier: Apache-2.0
 * Tracking wrappers (plan 5.5). sdk/abi/api_v1.sym binds these in place of
 * the plain functions when an app imports malloc, xTaskCreatePinnedToCore,
 * lv_timer_create and friends, so the OS can free what the app forgets.
 *
 * Which app: the calling task's context; app code reached without one (a
 * system callback) is charged to the foreground app.
 */
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "lwip/sockets.h"
#include <setjmp.h>
#include "t5_app/apps.h"
#include "t5_app/fspolicy.h"
#include "t5_app/wrappers.h"
#include "t5_core/event.h"
#include "t5_core/fault.h"
#include "t5_core/log.h"
#include "t5_storage/t5_storage.h"
#include "t5_ui/t5_ui.h"
#include "app_priv.h"
#include "apps_hw.h"

static const char *TAG = "t5.app";

/* Kernel objects an app creates: never internal RAM. */
#define APP_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define APP_MAX_PRIO 2          /* below every system service that does work */
#define APP_MAX_SOCKETS 8       /* half of LWIP_MAX_SOCKETS: the OS keeps the rest */

static t5_app_t *owner(void)
{
    t5_app_t *a = t5_app_ctx_get();
    if (a && a->hdr.magic == T5_APP_CTX_MAGIC) return a;
    return apps_fallback();
}

static void track(t5_res_kind_t kind, void *h, t5_res_release_fn_t rel)
{
    t5_app_t *a = owner();
    if (!a || !h) return;
    apps_lock();
    t5_tracker_add(&a->trk, kind, h, rel);
    apps_unlock();
}

static bool untrack(void *h)
{
    t5_app_t *a = owner();
    if (!a || !h) return false;
    apps_lock();
    bool had = t5_tracker_remove(&a->trk, h);
    apps_unlock();
    return had;
}

/* As track, for what must not exist untracked: false if the table is full
 * (or there's no app), and the caller undoes the creation. */
static bool track_or_refuse(t5_res_kind_t kind, void *h, t5_res_release_fn_t rel)
{
    t5_app_t *a = owner();
    if (!a || !h) return false;
    apps_lock();
    bool ok = t5_tracker_add(&a->trk, kind, h, rel);
    apps_unlock();
    return ok;
}

void *t5_app_malloc(size_t n)
{
    t5_app_t *a = owner();
    if (!a) return malloc(n);
    apps_lock();
    void *p = t5_tracker_malloc(&a->trk, n);
    apps_unlock();
    return p;
}

void *t5_app_calloc(size_t n, size_t sz)
{
    t5_app_t *a = owner();
    if (!a) return calloc(n, sz);
    apps_lock();
    void *p = t5_tracker_calloc(&a->trk, n, sz);
    apps_unlock();
    return p;
}

/* Already freed: it's in the app's heap, but no longer a live block. Handing
 * it to the system allocator would corrupt that. */
static bool stale_block(t5_app_t *a, const void *p)
{
    return a && apps_hw_heap_contains(&a->trk, p);
}

static bool free_tracked(void *p)
{
    t5_app_t *a = owner();
    apps_lock();
    bool ours = t5_tracker_free(NULL, p);
    bool stale = !ours && stale_block(a, p);
    apps_unlock();
    if (stale) T5_LOGW(TAG, "%s: freed %p twice; ignored", a->id, p);
    return ours || stale;
}

void t5_app_free(void *p)
{
    if (!p) return;
    if (free_tracked(p)) return;
    /* Memory from newlib (strdup inside libc...). Not a heap address: a bug
     * in the app, which free() would turn into a corrupted system heap. */
    if (esp_ptr_external_ram(p) || esp_ptr_internal(p)) free(p);
    else T5_LOGW(TAG, "free(%p): not a heap address; ignored", p);
}

void *t5_app_realloc(void *p, size_t n)
{
    if (!p) return t5_app_malloc(n);
    t5_app_t *a = owner();
    if (!t5_tracker_owns(p)) {
        if (stale_block(a, p)) {
            T5_LOGW(TAG, "%s: realloc of freed memory %p; refused", a->id, p);
            return NULL;
        }
        return realloc(p, n);
    }
    apps_lock();
    void *q = a ? t5_tracker_realloc(&a->trk, p, n) : NULL;
    apps_unlock();
    if (!q && n && a) {
        /* Block owned by another tracker: move it into ours. */
        size_t old = t5_tracker_block_size(p);
        q = t5_app_malloc(n);
        if (q) {
            memcpy(q, p, old < n ? old : n);
            t5_app_free(p);
        }
    }
    return q;
}

char *t5_app_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = t5_app_malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* An app's lv_malloc comes from its own heap, so a leak ends with the app.
 * LVGL can be the one to free such a block (lv_event_free_user_data_cb):
 * its allocator asks here about anything outside its pool. */
static void *lv_foreign_realloc(void *p, size_t n, bool *handled)
{
    *handled = t5_tracker_owns(p);
    return *handled ? t5_app_realloc(p, n) : NULL;
}

void t5_app_lv_mem_install(void)
{
    t5_ui_lv_set_foreign(free_tracked, lv_foreign_realloc);
}

void *t5_app_lv_malloc_zeroed(size_t n)
{
    return t5_app_calloc(1, n);
}

void *t5_app_lv_realloc(void *p, size_t n)
{
    if (!n) {
        lv_free(p);              /* lv_realloc(p, 0) gives a valid empty block */
        return t5_app_malloc(0);
    }
    if (p && !t5_tracker_owns(p)) return lv_realloc(p, n);
    return t5_app_realloc(p, n);
}

void *t5_app_lv_reallocf(void *p, size_t n)
{
    void *q = t5_app_lv_realloc(p, n);
    if (!q) lv_free(p);
    return q;
}

char *t5_app_lv_strdup(const char *s)
{
    return s ? t5_app_strdup(s) : NULL;
}

/* Label text: setting the text a label already shows is a no-op. LVGL
 * re-lays out and redraws the label on every set, and apps that refresh a
 * label from a timer (a clock, a track title, a sensor value) mostly set
 * the same text again. Redraws are memory traffic, and while audio plays
 * the speaker amp picks that up as static. A label's own buffer passed back
 * (text edited in place) still refreshes. */
static bool label_same(lv_obj_t *obj, const char *text)
{
    if (!obj || !text || !lv_obj_check_type(obj, &lv_label_class)) return false;
    const char *cur = lv_label_get_text(obj);
    return cur && cur != text && strcmp(cur, text) == 0;
}

void t5_app_lv_label_set_text(lv_obj_t *obj, const char *text)
{
    if (label_same(obj, text)) return;
    lv_label_set_text(obj, text);
}

void t5_app_lv_label_set_text_vfmt(lv_obj_t *obj, const char *fmt, va_list args)
{
    if (!fmt) {
        lv_label_set_text_vfmt(obj, fmt, args);
        return;
    }
    char small[96];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(small, sizeof small, fmt, copy);
    va_end(copy);
    if (n < 0) return;
    char *text = small;
    if ((size_t)n >= sizeof small) {
        text = malloc((size_t)n + 1);
        if (!text) {
            lv_label_set_text_vfmt(obj, fmt, args);
            return;
        }
        vsnprintf(text, (size_t)n + 1, fmt, args);
    }
    t5_app_lv_label_set_text(obj, text);
    if (text != small) free(text);
}

void t5_app_lv_label_set_text_fmt(lv_obj_t *obj, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    t5_app_lv_label_set_text_vfmt(obj, fmt, args);
    va_end(args);
}

char *t5_app_lv_strndup(const char *s, size_t max)
{
    if (!s) return NULL;
    size_t n = strnlen(s, max);
    char *p = t5_app_malloc(n + 1);
    if (p) {
        memcpy(p, s, n);
        p[n] = 0;
    }
    return p;
}

static void drawbuf_release(void *b)
{
    lv_image_cache_drop(b);      /* may still be cached as an image source */
    lv_draw_buf_destroy(b);
}

static lv_draw_buf_t *drawbuf_keep(lv_draw_buf_t *b)
{
    if (b && !track_or_refuse(T5_RES_OTHER, b, drawbuf_release) && owner()) {
        T5_LOGW(TAG, "%s: too many open handles; draw buffer refused", owner()->id);
        lv_draw_buf_destroy(b);
        return NULL;
    }
    return b;
}

lv_draw_buf_t *t5_app_lv_draw_buf_create(uint32_t w, uint32_t h, lv_color_format_t cf, uint32_t stride)
{
    return drawbuf_keep(lv_draw_buf_create(w, h, cf, stride));
}

lv_draw_buf_t *t5_app_lv_draw_buf_create_ex(const lv_draw_buf_handlers_t *handlers, uint32_t w, uint32_t h,
                                            lv_color_format_t cf, uint32_t stride)
{
    return drawbuf_keep(lv_draw_buf_create_ex(handlers, w, h, cf, stride));
}

lv_draw_buf_t *t5_app_lv_draw_buf_dup(const lv_draw_buf_t *b)
{
    return drawbuf_keep(lv_draw_buf_dup(b));
}

lv_draw_buf_t *t5_app_lv_draw_buf_dup_ex(const lv_draw_buf_handlers_t *handlers, const lv_draw_buf_t *b)
{
    return drawbuf_keep(lv_draw_buf_dup_ex(handlers, b));
}

lv_draw_buf_t *t5_app_lv_snapshot_take(lv_obj_t *obj, lv_color_format_t cf)
{
    return drawbuf_keep(lv_snapshot_take(obj, cf));
}

void t5_app_lv_draw_buf_destroy(lv_draw_buf_t *b)
{
    untrack(b);
    lv_draw_buf_destroy(b);
}

static bool tasks_full(void)
{
    t5_app_t *a = owner();
    if (a && a->exiting) return true;                  /* exit is stopping them */
    if (!a || t5_tracker_count(&a->trk, T5_RES_TASK) < T5_APP_MAX_TASKS) return false;
    T5_LOGW(TAG, "%s: already has %d tasks; refused another", a->id, T5_APP_MAX_TASKS);
    return true;
}

t5_err_t t5_app_task_create(const char *name, t5_task_fn_t fn, void *arg, uint32_t stack_bytes, int prio, int core,
                            t5_task_t *out)
{
    if (tasks_full()) return T5_ERR_NO_MEM;
    if (prio > APP_MAX_PRIO) prio = APP_MAX_PRIO;
    return t5_task_create(name, fn, arg, stack_bytes, prio, core, out);
}

/* An app's event handler that doesn't return would stop every system event:
 * the watchdog moves the dispatcher out of it (t5_fault_interrupt_task). */
#define EVENT_STUCK_MS 3000
#define GUARD_SLOTS 6
typedef struct {
    t5_app_t *volatile app;
    void *volatile task;
    volatile uint32_t since;
} guard_t;
static guard_t s_guards[GUARD_SLOTS];

static guard_t *guard_begin(t5_app_t *a)
{
    void *self = t5_task_self();
    for (int i = 0; i < GUARD_SLOTS; i++) {
        void *expect = NULL;
        if (s_guards[i].task == self ||
            __atomic_compare_exchange_n(&s_guards[i].task, &expect, self, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            s_guards[i].since = t5_time_ms();
            s_guards[i].app = a;
            return &s_guards[i];
        }
    }
    return NULL;
}

static void guard_end(guard_t *g)
{
    if (!g) return;
    g->app = NULL;
    __atomic_store_n(&g->task, NULL, __ATOMIC_RELEASE);
}

void t5_app_events_check_stuck(void)
{
    for (int i = 0; i < GUARD_SLOTS; i++) {
        guard_t *g = &s_guards[i];
        t5_app_t *a = g->app;
        void *task = g->task;
        if (!a || !task || t5_time_ms() - g->since < EVENT_STUCK_MS) continue;
        g->since = t5_time_ms();         /* one try per period, whatever comes of it */
        T5_LOGW(TAG, "%s: a callback has not returned for %d s; stopping it", a->id, EVENT_STUCK_MS / 1000);
        if (t5_fault_interrupt_task(task, 500) != T5_OK) T5_LOGE(TAG, "can't stop it");
    }
}

/* An app callback on a system task (event dispatcher, USB driver): a fault
 * comes back here and ends the app, not the task (fault.h), and one that
 * never returns is stopped (t5_app_events_check_stuck). */
static void guarded_call(t5_app_t *a, void (*fn)(void *), void *ctx)
{
    if (!a || a->exiting) return;
    void *saved = t5_app_ctx_get();
    t5_app_ctx_set(a);
    guard_t *g = guard_begin(a);
    t5_fault_point_t fp;
    t5_fault_push(&fp, a);
    if (setjmp(fp.jb) == 0) fn(ctx);
    t5_fault_pop(&fp);
    guard_end(g);
    t5_app_ctx_set(saved);
}

/* Calls the app leaves queued for the UI task must not run after it has
 * gone: its code would no longer be there. */
typedef struct late {
    void (*fn)(void *);
    void *arg;
    uint32_t instance;
    struct late *next;          /* lv_async_call only, for cancelling */
} late_t;

static void late_run(const late_t *l)
{
    t5_app_t *a = apps_by_instance(l->instance);
    if (!a) return;
    void *saved = t5_app_ctx_get();
    t5_app_ctx_set(a);
    l->fn(l->arg);
    t5_app_ctx_set(saved);
}

static void late_async(void *p)
{
    late_t l = *(late_t *)p;
    heap_caps_free(p);
    late_run(&l);
}

static void late_sync(void *p)
{
    late_run(p);
}

static uint32_t instance_of_caller(void)
{
    t5_app_t *a = owner();
    return a ? a->instance : 0;
}

t5_err_t t5_app_ui_async(t5_ui_fn_t fn, void *arg)
{
    if (!owner()) return t5_ui_async(fn, arg);
    late_t *l = heap_caps_malloc(sizeof *l, APP_CAPS);
    if (!l) return T5_ERR_NO_MEM;
    *l = (late_t){ fn, arg, instance_of_caller(), NULL };
    t5_err_t e = t5_ui_async(late_async, l);
    if (e != T5_OK) heap_caps_free(l);
    return e;
}

t5_err_t t5_app_ui_sync(t5_ui_fn_t fn, void *arg, uint32_t timeout_ms)
{
    if (!owner()) return t5_ui_sync(fn, arg, timeout_ms);
    late_t l = { fn, arg, instance_of_caller(), NULL };   /* t5_ui_sync doesn't return while it's queued */
    return t5_ui_sync(late_sync, &l, timeout_ms);
}

/* Both run under the UI lock, as lv_async_call requires. */
static late_t *s_lv_late;
static uint32_t s_lv_late_n;
#define LV_LATE_MAX 256

static void lv_late_unlink(late_t *l)
{
    for (late_t **pp = &s_lv_late; *pp; pp = &(*pp)->next) {
        if (*pp == l) {
            *pp = l->next;
            s_lv_late_n--;
            return;
        }
    }
}

static void lv_late_tramp(void *p)
{
    late_t l = *(late_t *)p;
    lv_late_unlink(p);
    heap_caps_free(p);
    late_run(&l);
}

lv_result_t t5_app_lv_async_call(lv_async_cb_t cb, void *arg)
{
    if (!owner()) return lv_async_call(cb, arg);
    if (s_lv_late_n >= LV_LATE_MAX) return LV_RESULT_INVALID;
    late_t *l = heap_caps_malloc(sizeof *l, APP_CAPS);
    if (!l) return LV_RESULT_INVALID;
    *l = (late_t){ cb, arg, instance_of_caller(), s_lv_late };
    lv_result_t r = lv_async_call(lv_late_tramp, l);
    if (r == LV_RESULT_OK) {
        s_lv_late = l;
        s_lv_late_n++;
    } else {
        heap_caps_free(l);
    }
    return r;
}

lv_result_t t5_app_lv_async_call_cancel(lv_async_cb_t cb, void *arg)
{
    for (late_t *l = s_lv_late; l; l = l->next) {
        if (l->fn == cb && l->arg == arg && l->instance == instance_of_caller()) {
            lv_result_t r = lv_async_call_cancel(lv_late_tramp, l);
            if (r == LV_RESULT_OK) {
                lv_late_unlink(l);
                heap_caps_free(l);
            }
            return r;
        }
    }
    return lv_async_call_cancel(cb, arg);
}

/* A global hook would outlive the app and call into nothing. */
void t5_app_ui_noop_log_cb(lv_log_print_g_cb_t cb)
{
    (void)cb;
}

typedef struct {
    t5_usb_midi_cb_t cb;
    void *arg;
    t5_app_t *app;
} midi_guard_t;
static midi_guard_t s_midi;

typedef struct {
    const midi_guard_t *m;
    const uint8_t *msg;
    size_t len;
} midi_call_t;

static void midi_call(void *p)
{
    const midi_call_t *c = p;
    c->m->cb(c->msg, c->len, c->m->arg);
}

static void midi_tramp(const uint8_t *msg, size_t len, void *arg)
{
    midi_guard_t *m = arg;
    midi_call_t c = { m, msg, len };
    guarded_call(m->app, midi_call, &c);
}

static void midi_release(void *p)
{
    midi_guard_t *m = p;
    t5_usb_midi_listen(NULL, NULL);
    m->app = NULL;
}

t5_err_t t5_app_usb_midi_listen(t5_usb_midi_cb_t cb, void *arg)
{
    t5_app_t *a = owner();
    if (!a || !cb) {
        if (a) untrack(&s_midi);
        s_midi.app = NULL;
        return t5_usb_midi_listen(cb, arg);
    }
    s_midi = (midi_guard_t){ cb, arg, a };
    track(T5_RES_OTHER, &s_midi, midi_release);
    return t5_usb_midi_listen(midi_tramp, &s_midi);
}

/* One per open stream: the app's callback, freed when the stream closes or
 * the app exits (after the stream, which is tracked later). */
typedef struct {
    t5_usb_uvc_frame_cb_t cb;
    void *arg;
    t5_app_t *app;
    t5_usb_uvc_t *stream;
} uvc_guard_t;

static void uvc_guard_free(void *g)
{
    free(g);
}

typedef struct {
    const uvc_guard_t *u;
    const uint8_t *jpeg;
    size_t len;
} uvc_call_t;

static void uvc_call(void *p)
{
    const uvc_call_t *c = p;
    c->u->cb(c->jpeg, c->len, c->u->arg);
}

static void uvc_tramp(const uint8_t *jpeg, size_t len, void *arg)
{
    uvc_guard_t *u = arg;
    uvc_call_t c = { u, jpeg, len };
    guarded_call(u->app, uvc_call, &c);
}

t5_err_t t5_app_usb_uvc_open(uint16_t w, uint16_t h, uint8_t fps, t5_usb_uvc_frame_cb_t cb, void *arg, t5_usb_uvc_t **out)
{
    t5_app_t *a = owner();
    if (!a || !cb || !out) return t5_usb_uvc_open(w, h, fps, cb, arg, out);
    uvc_guard_t *g = malloc(sizeof *g);
    if (!g) return T5_ERR_NO_MEM;
    *g = (uvc_guard_t){ cb, arg, a, NULL };
    if (!track_or_refuse(T5_RES_OTHER, g, uvc_guard_free)) {
        free(g);
        return T5_ERR_NO_MEM;
    }
    t5_err_t e = t5_usb_uvc_open(w, h, fps, uvc_tramp, g, out);
    if (e != T5_OK) {
        untrack(g);
        free(g);
        return e;
    }
    g->stream = *out;
    return T5_OK;
}

t5_err_t t5_app_usb_uvc_close(t5_usb_uvc_t *u)
{
    t5_err_t e = t5_usb_uvc_close(u);
    t5_app_t *a = owner();
    if (!a || e != T5_OK) return e;
    uvc_guard_t *found = NULL;
    apps_lock();
    for (int i = 0; i < a->trk.n && !found; i++) {
        const t5_track_ent_t *t = &a->trk.ent[i];
        if (t->release == uvc_guard_free && ((uvc_guard_t *)t->handle)->stream == u) found = t->handle;
    }
    if (found) t5_tracker_remove(&a->trk, found);
    apps_unlock();
    free(found);
    return e;
}

/* An app's task is deleted at exit wherever it stands, but not inside a
 * filesystem call: it would die holding the filesystem's lock. Exit waits
 * for the count to reach zero, and once the app is exiting its tasks stop
 * here instead of starting another call. The order matters: raise the count,
 * then look at exiting; exit does the reverse. The UI task and event
 * dispatchers are exempt, because on_suspend and on_destroy still save. */
static t5_app_t *file_enter(void)
{
    t5_app_t *a = owner();
    if (!a) return NULL;
    __atomic_add_fetch(&a->file_calls, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&a->exiting, __ATOMIC_SEQ_CST)) {
        apps_lock();
        bool mine = t5_tracker_has(&a->trk, t5_task_self());
        apps_unlock();
        if (mine) {
            __atomic_sub_fetch(&a->file_calls, 1, __ATOMIC_SEQ_CST);
            for (;;) vTaskSuspend(NULL);    /* until the exit deletes this task */
        }
    }
    return a;
}

static void file_leave(t5_app_t *a)
{
    if (a) __atomic_sub_fetch(&a->file_calls, 1, __ATOMIC_SEQ_CST);
}

static bool path_ok(const char *path, bool write)
{
    t5_app_t *a = owner();
    if (!a) return true;
    if (path && t5_app_fs_check(a, path, write)) return true;
    T5_LOGW(TAG, "%s: no access to %s", a->id, path ? path : "(null)");
    errno = EACCES;
    return false;
}

int t5_app_remove(const char *path)
{
    if (!path_ok(path, true)) return -1;
    t5_app_t *fa = file_enter();
    int r = remove(path);
    file_leave(fa);
    return r;
}

int t5_app_rename(const char *from, const char *to)
{
    if (!path_ok(from, true) || !path_ok(to, true)) return -1;
    t5_app_t *fa = file_enter();
    int r = rename(from, to);
    file_leave(fa);
    return r;
}

int t5_app_mkdir(const char *path, mode_t mode)
{
    if (!path_ok(path, true)) return -1;
    t5_app_t *fa = file_enter();
    int r = mkdir(path, mode);
    file_leave(fa);
    return r;
}

int t5_app_stat(const char *path, struct stat *st)
{
    if (!path_ok(path, false)) return -1;
    t5_app_t *fa = file_enter();
    int r = stat(path, st);
    file_leave(fa);
    return r;
}

static void dir_release(void *d)
{
    closedir(d);
}

DIR *t5_app_opendir(const char *path)
{
    if (!path_ok(path, false)) return NULL;
    t5_app_t *fa = file_enter();
    DIR *d = opendir(path);
    file_leave(fa);
    if (d && !track_or_refuse(T5_RES_OTHER, d, dir_release)) {
        closedir(d);
        errno = EMFILE;
        return NULL;
    }
    return d;
}

int t5_app_closedir(DIR *d)
{
    t5_app_t *fa = file_enter();    /* first: a task parked here must leave it tracked */
    untrack(d);
    int r = closedir(d);
    file_leave(fa);
    return r;
}

struct dirent *t5_app_readdir(DIR *d)
{
    t5_app_t *fa = file_enter();
    struct dirent *e = readdir(d);
    file_leave(fa);
    return e;
}

t5_err_t t5_app_storage_write_atomic(const char *path, const void *data, size_t len)
{
    if (!path_ok(path, true)) return T5_ERR_DENIED;
    t5_app_t *a = owner();
    if (a && t5_app_fs_quota_applies(a, path) && !t5_app_fs_room(a, len)) return T5_ERR_NO_MEM;
    t5_app_t *fa = file_enter();
    t5_err_t e = t5_storage_write_atomic(path, data, len);
    file_leave(fa);
    return e;
}

bool apps_path_check(const char *path, bool write)
{
    t5_app_t *a = t5_app_self();
    if (!a || t5_app_fs_check(a, path, write)) return true;
    T5_LOGW(TAG, "%s: no access to %s", a->id, path);
    return false;
}

/* Writes that count against the data dir's quota. */
static bool write_allowed(FILE *f, size_t n)
{
    t5_app_t *a = owner();
    if (!a) return true;
    bool counted = false;
    for (size_t i = 0; i < sizeof a->qfiles / sizeof a->qfiles[0]; i++) {
        if (a->qfiles[i] == f) counted = true;
    }
    if (!counted || t5_app_fs_room(a, n)) return true;
    T5_LOGW(TAG, "%s: data folder is full (%u KB allowed)", a->id, T5_FS_QUOTA_BYTES / 1024u);
    errno = ENOSPC;
    return false;
}

size_t t5_app_fwrite(const void *buf, size_t size, size_t n, FILE *f)
{
    if (!write_allowed(f, size * n)) return 0;
    t5_app_t *fa = file_enter();
    size_t r = fwrite(buf, size, n, f);
    file_leave(fa);
    return r;
}

size_t t5_app_fread(void *buf, size_t size, size_t n, FILE *f)
{
    t5_app_t *fa = file_enter();
    size_t r = fread(buf, size, n, f);
    file_leave(fa);
    return r;
}

char *t5_app_fgets(char *s, int n, FILE *f)
{
    t5_app_t *fa = file_enter();
    char *r = fgets(s, n, f);
    file_leave(fa);
    return r;
}

int t5_app_fgetc(FILE *f)
{
    t5_app_t *fa = file_enter();
    int r = fgetc(f);
    file_leave(fa);
    return r;
}

int t5_app_fflush(FILE *f)
{
    t5_app_t *fa = file_enter();
    int r = fflush(f);
    file_leave(fa);
    return r;
}

int t5_app_fseek(FILE *f, long off, int whence)
{
    t5_app_t *fa = file_enter();
    int r = fseek(f, off, whence);
    file_leave(fa);
    return r;
}

int t5_app_fputs(const char *s, FILE *f)
{
    if (!write_allowed(f, strlen(s))) return EOF;
    t5_app_t *fa = file_enter();
    int r = fputs(s, f);
    file_leave(fa);
    return r;
}

int t5_app_fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int len = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    int r = -1;
    if (len >= 0 && write_allowed(f, (size_t)len)) {
        t5_app_t *fa = file_enter();
        r = vfprintf(f, fmt, ap);
        file_leave(fa);
    }
    va_end(ap);
    return r;
}

typedef struct {
    TaskFunction_t fn;
    void *arg;
} rtos_tramp_t;

static void rtos_task(void *p)
{
    rtos_tramp_t t = *(rtos_tramp_t *)p;
    free(p);
    t.fn(t.arg);
    /* FreeRTOS tasks must not return; t5_task_create's trampoline deletes us. */
}

BaseType_t t5_app_xTaskCreatePinnedToCore(TaskFunction_t fn, const char *const name, const configSTACK_DEPTH_TYPE stack,
                                          void *const arg, UBaseType_t prio, TaskHandle_t *const out,
                                          const BaseType_t core)
{
    rtos_tramp_t *t = malloc(sizeof *t);
    if (!t) return pdFAIL;
    if (tasks_full()) {
        free(t);
        return pdFAIL;
    }
    *t = (rtos_tramp_t){ fn, arg };
    int p = prio > APP_MAX_PRIO ? APP_MAX_PRIO : (int)prio;
    t5_task_t h;
    void *saved = t5_app_ctx_get();
    if (!saved) t5_app_ctx_set(apps_fallback());     /* the new task inherits the app */
    t5_err_t e = t5_task_create(name, rtos_task, t, stack, p, core == tskNO_AFFINITY ? T5_CORE_ANY : (int)core, &h);
    t5_app_ctx_set(saved);
    if (e != T5_OK) {
        free(t);
        return pdFAIL;
    }
    if (out) *out = (TaskHandle_t)h;
    return pdPASS;
}

void t5_app_vTaskDelete(TaskHandle_t h)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    untrack(h ? h : self);
    t5_task_delete((t5_task_t)h);
}

static void queue_release(void *q)
{
    uint8_t *storage;
    StaticQueue_t *st;
    /* A semaphore's "storage" is the queue block itself, which
     * vQueueDeleteWithCaps would then free twice. */
    if (xQueueGetStaticBuffers(q, &storage, &st) != pdTRUE) vQueueDelete(q);    /* a queue set: ordinary memory */
    else if (!storage || (void *)storage == (void *)st) vSemaphoreDeleteWithCaps((SemaphoreHandle_t)q);
    else vQueueDeleteWithCaps(q);
}

static QueueHandle_t queue_keep(QueueHandle_t q)
{
    if (q && !track_or_refuse(T5_RES_OTHER, q, queue_release)) {
        queue_release(q);
        return NULL;
    }
    return q;
}

QueueHandle_t t5_app_xQueueGenericCreate(const UBaseType_t len, const UBaseType_t item, const uint8_t type)
{
    if (type == queueQUEUE_TYPE_BASE) return queue_keep(xQueueCreateWithCaps(len, item, APP_CAPS));
    if (type == queueQUEUE_TYPE_BINARY_SEMAPHORE) return queue_keep(xSemaphoreCreateGenericWithCaps(0, 0, type, APP_CAPS));
    return queue_keep(xQueueGenericCreate(len, item, type));
}

QueueHandle_t t5_app_xQueueCreateMutex(const uint8_t type)
{
    return queue_keep(xSemaphoreCreateGenericWithCaps(0, 0, type, APP_CAPS));
}

QueueHandle_t t5_app_xQueueCreateCountingSemaphore(const UBaseType_t max, const UBaseType_t initial)
{
    return queue_keep(xSemaphoreCreateGenericWithCaps(max, initial, queueQUEUE_TYPE_COUNTING_SEMAPHORE, APP_CAPS));
}

void t5_app_vQueueDelete(QueueHandle_t q)
{
    untrack(q);
    queue_release(q);
}

static void mutex_release(void *m)
{
    vSemaphoreDeleteWithCaps((SemaphoreHandle_t)m);
}

t5_mutex_t t5_app_mutex_create(void)
{
    SemaphoreHandle_t m = xSemaphoreCreateGenericWithCaps(0, 0, queueQUEUE_TYPE_RECURSIVE_MUTEX, APP_CAPS);
    if (m && !track_or_refuse(T5_RES_OTHER, m, mutex_release)) {
        vSemaphoreDeleteWithCaps(m);
        return NULL;
    }
    return (t5_mutex_t)m;
}

void t5_app_mutex_delete(t5_mutex_t m)
{
    if (untrack(m)) vSemaphoreDeleteWithCaps((SemaphoreHandle_t)m);
    else t5_mutex_delete(m);
}

int t5_app_mutexes_held_by(t5_app_t *a, void *task)
{
    int n = 0;
    for (int i = 0; i < a->trk.n; i++) {
        const t5_track_ent_t *e = &a->trk.ent[i];
        if (e->release != mutex_release && e->release != queue_release) continue;
        if (xQueueGetMutexHolder((QueueHandle_t)e->handle) == (TaskHandle_t)task) n++;
    }
    return n;
}

static void timer_release(void *t)
{
    lv_timer_delete(t);
}

lv_timer_t *t5_app_lv_timer_create(lv_timer_cb_t cb, uint32_t period, void *user_data)
{
    lv_timer_t *t = lv_timer_create(cb, period, user_data);
    if (t) {
        /* A timer that runs out its repeat count would be deleted by LVGL
         * behind the tracker's back; pause it instead (freed on exit). */
        lv_timer_set_auto_delete(t, false);
        track(T5_RES_TIMER, t, timer_release);
    }
    return t;
}

void t5_app_lv_timer_delete(lv_timer_t *t)
{
    untrack(t);
    lv_timer_delete(t);
}

typedef struct {
    void *var;
    lv_anim_exec_xcb_t exec;
    lv_anim_custom_exec_cb_t custom;
} anim_ref_t;

static void anim_release(void *p)
{
    anim_ref_t *r = p;
    if (r->custom) lv_anim_delete(r->var, NULL);
    else lv_anim_delete(r->var, r->exec);
    free(r);
}

static bool anim_alive(const anim_ref_t *r)
{
    return lv_anim_get(r->var, r->exec) != NULL;
}

/* Finished animations leave their refs behind: swept before adding more. */
static bool anim_keep(void *p)
{
    if (anim_alive(p)) return true;
    free(p);
    return false;
}

lv_anim_t *t5_app_lv_anim_start(const lv_anim_t *a)
{
    lv_anim_t *run = lv_anim_start(a);
    t5_app_t *app = owner();
    if (!run || !app) return run;
    anim_ref_t *r = malloc(sizeof *r);
    if (!r) return run;
    *r = (anim_ref_t){ a->var, a->exec_cb, a->custom_exec_cb };
    apps_lock();
    if (t5_tracker_count(&app->trk, T5_RES_ANIM) >= 32) t5_tracker_prune(&app->trk, T5_RES_ANIM, anim_keep);
    if (!t5_tracker_add(&app->trk, T5_RES_ANIM, r, anim_release)) free(r);
    apps_unlock();
    return run;
}

typedef struct {
    t5_event_id_t id;
    t5_event_handler_t fn;
    void *arg;
    t5_app_t *app;
} ev_sub_t;

typedef struct {
    const ev_sub_t *s;
    t5_event_id_t id;
    const void *data;
    size_t len;
} ev_call_t;

static void ev_call(void *p)
{
    const ev_call_t *c = p;
    c->s->fn(c->id, c->data, c->len, c->s->arg);
}

/* The dispatcher carries on with the other subscribers if this one faults. */
static void ev_tramp(t5_event_id_t id, const void *data, size_t len, void *p)
{
    ev_sub_t *s = p;
    ev_call_t c = { s, id, data, len };
    guarded_call(s->app, ev_call, &c);
}

static void ev_release(void *p)
{
    ev_sub_t *s = p;
    t5_event_unsubscribe(s->id, ev_tramp, s);
    free(s);
}

t5_err_t t5_app_event_subscribe(t5_event_id_t id, t5_event_handler_t fn, void *arg)
{
    t5_app_t *a = owner();
    if (!a) return t5_event_subscribe(id, fn, arg);
    ev_sub_t *s = malloc(sizeof *s);
    if (!s) return T5_ERR_NO_MEM;
    *s = (ev_sub_t){ id, fn, arg, a };
    t5_err_t e = t5_event_subscribe(id, ev_tramp, s);
    if (e != T5_OK) {
        free(s);
        return e;
    }
    track(T5_RES_OTHER, s, ev_release);
    return T5_OK;
}

t5_err_t t5_app_event_unsubscribe(t5_event_id_t id, t5_event_handler_t fn, void *arg)
{
    t5_app_t *a = owner();
    if (!a) return t5_event_unsubscribe(id, fn, arg);
    ev_sub_t *found = NULL;
    apps_lock();
    for (int i = 0; i < a->trk.n; i++) {
        t5_track_ent_t *e = &a->trk.ent[i];
        if (e->release != ev_release) continue;
        ev_sub_t *s = e->handle;
        if (s->id == id && s->fn == fn && s->arg == arg) {
            found = s;
            t5_tracker_remove(&a->trk, s);
            break;
        }
    }
    apps_unlock();
    if (!found) return T5_ERR_NOT_FOUND;
    ev_release(found);
    return T5_OK;
}

static void file_release(void *f)
{
    fclose(f);
}

FILE *t5_app_fopen(const char *path, const char *mode)
{
    bool writing = mode && (mode[0] != 'r' || strchr(mode, '+'));
    if (!path_ok(path, writing)) return NULL;
    t5_app_t *a = owner();
    bool in_data = a && writing && t5_app_fs_quota_applies(a, path);
    int slot = -1;
    if (in_data) {
        for (int i = 0; slot < 0 && i < (int)(sizeof a->qfiles / sizeof a->qfiles[0]); i++) {
            if (!a->qfiles[i]) slot = i;
        }
        if (slot < 0 || !t5_app_fs_room(a, 0)) {
            errno = slot < 0 ? EMFILE : ENOSPC;
            return NULL;
        }
    }
    t5_app_t *fa = file_enter();
    FILE *f = fopen(path, mode);
    file_leave(fa);
    if (f && in_data) a->qfiles[slot] = f;
    track(T5_RES_OTHER, f, file_release);
    return f;
}

int t5_app_fclose(FILE *f)
{
    t5_app_t *fa = file_enter();    /* first: a task parked here must leave it tracked */
    t5_app_t *a = owner();
    for (size_t i = 0; a && i < sizeof a->qfiles / sizeof a->qfiles[0]; i++) {
        if (a->qfiles[i] == f) a->qfiles[i] = NULL;
    }
    untrack(f);
    int r = fclose(f);
    file_leave(fa);
    return r;
}

/* A socket is tracked under a key that can't be a heap pointer, so the app
 * runtime closes the fds an app leaves open (t5/net.h handles track
 * themselves). */
#define FD_KEY(fd) ((void *)(uintptr_t)(0xfd000000u | ((uint32_t)(fd) & 0xffffu)))

static void fd_release(void *key)
{
    lwip_close((int)((uintptr_t)key & 0xffffu));
}

/* The OS shares the socket table with apps. */
static bool sockets_full(void)
{
    t5_app_t *a = owner();
    int n = 0;
    if (!a) return false;
    apps_lock();
    for (int i = 0; i < a->trk.n; i++) {
        if (a->trk.ent[i].release == fd_release) n++;
    }
    apps_unlock();
    if (n < APP_MAX_SOCKETS) return false;
    T5_LOGW(TAG, "%s: already has %d sockets; refused another", a->id, APP_MAX_SOCKETS);
    errno = ENFILE;
    return true;
}

int t5_app_lwip_socket(int domain, int type, int protocol)
{
    if (sockets_full()) return -1;
    int fd = lwip_socket(domain, type, protocol);
    if (fd >= 0) track(T5_RES_OTHER, FD_KEY(fd), fd_release);
    return fd;
}

int t5_app_lwip_accept(int s, struct sockaddr *addr, socklen_t *addrlen)
{
    if (sockets_full()) return -1;
    int fd = lwip_accept(s, addr, addrlen);
    if (fd >= 0) track(T5_RES_OTHER, FD_KEY(fd), fd_release);
    return fd;
}

int t5_app_lwip_close(int s)
{
    untrack(FD_KEY(s));
    return lwip_close(s);
}

int t5_app_close(int fd)
{
    untrack(FD_KEY(fd));
    return close(fd);
}

/* errno is thread-local in picolibc, which the app loader can't relocate:
 * apps build with -D__PICOLIBC_ERRNO_FUNCTION=t5_errno (sdk/cmake). */
int *t5_errno(void)
{
    return &errno;
}

/* libc's would reboot the device. A bad store instead goes down the path every
 * app fault takes: the app is closed and the crash recorded, and the OS stays
 * up (isolate.h). */
__attribute__((noreturn)) static void stop_app(void)
{
    volatile uintptr_t bad = 4;     /* volatile: a constant address draws a compiler warning */
    *(volatile int *)bad = 0;
    __builtin_unreachable();
}

void t5_app_abort(void)
{
    T5_LOGE(TAG, "%s called abort()", owner() ? owner()->id : "?");
    stop_app();
}

void t5_app_assert_func(const char *file, int line, const char *func, const char *expr)
{
    T5_LOGE(TAG, "%s: assertion failed: %s, %s:%d (%s)", owner() ? owner()->id : "?", expr ? expr : "?",
            file ? file : "?", line, func ? func : "?");
    stop_app();
}
