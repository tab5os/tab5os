/* SPDX-License-Identifier: Apache-2.0
 * Tracking wrappers (plan 5.5). sdk/abi/api_v1.sym binds these in place of
 * the plain functions when an app imports malloc, xTaskCreatePinnedToCore,
 * lv_timer_create and friends, so the OS can free what the app forgets.
 *
 * Which app: the calling task's context; app code reached without one (a
 * system callback) is charged to the foreground app.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "t5_app/wrappers.h"
#include "t5_core/event.h"
#include "t5_core/log.h"
#include "t5_ports/ports.h"
#include "app_priv.h"

static const char *TAG = "t5.app";

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

static void untrack(void *h)
{
    t5_app_t *a = owner();
    if (!a || !h) return;
    apps_lock();
    t5_tracker_remove(&a->trk, h);
    apps_unlock();
}

/* ---- heap -------------------------------------------------------------------- */
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

void t5_app_free(void *p)
{
    if (!p) return;
    apps_lock();
    bool ours = t5_tracker_free(NULL, p);
    apps_unlock();
    if (!ours) free(p);          /* memory from newlib (strdup inside libc...) */
}

void *t5_app_realloc(void *p, size_t n)
{
    if (!p) return t5_app_malloc(n);
    if (!t5_tracker_owns(p)) return realloc(p, n);
    t5_app_t *a = owner();
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

/* ---- FreeRTOS ---------------------------------------------------------------------- */
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
    *t = (rtos_tramp_t){ fn, arg };
    /* Apps stay below the UI task and system services. */
    int p = prio > 4 ? 4 : (int)prio;
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
    vTaskDelete(h);
}

static void queue_release(void *q)
{
    vQueueDelete(q);
}

QueueHandle_t t5_app_xQueueGenericCreate(const UBaseType_t len, const UBaseType_t item, const uint8_t type)
{
    QueueHandle_t q = xQueueGenericCreate(len, item, type);
    track(T5_RES_OTHER, q, queue_release);
    return q;
}

QueueHandle_t t5_app_xQueueCreateMutex(const uint8_t type)
{
    QueueHandle_t q = xQueueCreateMutex(type);
    track(T5_RES_OTHER, q, queue_release);
    return q;
}

QueueHandle_t t5_app_xQueueCreateCountingSemaphore(const UBaseType_t max, const UBaseType_t initial)
{
    QueueHandle_t q = xQueueCreateCountingSemaphore(max, initial);
    track(T5_RES_OTHER, q, queue_release);
    return q;
}

void t5_app_vQueueDelete(QueueHandle_t q)
{
    untrack(q);
    vQueueDelete(q);
}

static void mutex_release(void *m)
{
    t5_mutex_delete(m);
}

t5_mutex_t t5_app_mutex_create(void)
{
    t5_mutex_t m = t5_mutex_create();
    track(T5_RES_OTHER, m, mutex_release);
    return m;
}

void t5_app_mutex_delete(t5_mutex_t m)
{
    untrack(m);
    t5_mutex_delete(m);
}

/* ---- LVGL timers and animations ---------------------------------------------------------- */
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

/* Finished animations leave their refs behind; sweep them before adding. */
static void anim_sweep(t5_app_t *a)
{
    t5_tracker_t *t = &a->trk;
    for (int i = t->n - 1; i >= 0; i--) {
        if (t->ent[i].kind != T5_RES_ANIM) continue;
        anim_ref_t *r = t->ent[i].handle;
        if (!anim_alive(r)) {
            t5_tracker_remove(t, r);
            free(r);
        }
    }
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
    if (app->trk.counts[T5_RES_ANIM] >= 32) anim_sweep(app);
    if (!t5_tracker_add(&app->trk, T5_RES_ANIM, r, anim_release)) free(r);
    apps_unlock();
    return run;
}

/* ---- system events ------------------------------------------------------------------------- */
typedef struct {
    t5_event_id_t id;
    t5_event_handler_t fn;
    void *arg;
    t5_app_t *app;
} ev_sub_t;

static void ev_tramp(t5_event_id_t id, const void *data, size_t len, void *p)
{
    ev_sub_t *s = p;
    void *saved = t5_app_ctx_get();
    t5_app_ctx_set(s->app);
    if (!s->app->exiting) s->fn(id, data, len, s->arg);
    t5_app_ctx_set(saved);
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

/* ---- files ------------------------------------------------------------------------------------- */
static void file_release(void *f)
{
    fclose(f);
}

FILE *t5_app_fopen(const char *path, const char *mode)
{
    FILE *f = fopen(path, mode);
    track(T5_RES_OTHER, f, file_release);
    return f;
}

int t5_app_fclose(FILE *f)
{
    untrack(f);
    return fclose(f);
}

/* ---- ports: only the ones the manifest declares (plan 5.9) ------------------------------------- */
static bool port_declared(const t5_app_t *a, const char *name)
{
    if (!a) return true;               /* system caller */
    for (int i = 0; i < a->man.nports; i++) {
        if (!strcmp(a->man.ports[i], name)) return true;
    }
    return false;
}

t5_err_t t5_app_port_open(const char *name, t5_port_mode_t mode, const void *cfg, t5_port_t **out)
{
    t5_app_t *a = owner();
    if (name && !port_declared(a, name)) {
        T5_LOGW(TAG, "%s: port %s not in its manifest", a->id, name);
        return T5_ERR_DENIED;
    }
    return t5_port_open(name, mode, cfg, out);
}

t5_err_t t5_app_port_open_ex(const char *name, t5_port_mode_t mode, const void *cfg, uint32_t flags, t5_port_t **out,
                             char *why, size_t whylen)
{
    t5_app_t *a = owner();
    if (name && !port_declared(a, name)) {
        if (why && whylen) snprintf(why, whylen, "port %s is not declared in the app's manifest", name);
        return T5_ERR_DENIED;
    }
    return t5_port_open_ex(name, mode, cfg, flags, out, why, whylen);
}
