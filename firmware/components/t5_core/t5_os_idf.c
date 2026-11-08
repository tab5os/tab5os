/* SPDX-License-Identifier: Apache-2.0 */
#include <stdbool.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "t5_core/os.h"
#include "sdkconfig.h"

/* TLS slot for the app context. IDF's pthread layer uses slot 0. */
#define T5_TLS_APP_CTX (CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS - 1)
_Static_assert(CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS >= 2, "need a TLS slot for the app context");

t5_mutex_t t5_mutex_create(void)
{
    return (t5_mutex_t)xSemaphoreCreateRecursiveMutex();
}

void t5_mutex_delete(t5_mutex_t m)
{
    if (m) vSemaphoreDelete((SemaphoreHandle_t)m);
}

void t5_mutex_lock(t5_mutex_t m)
{
    xSemaphoreTakeRecursive((SemaphoreHandle_t)m, portMAX_DELAY);
}

bool t5_mutex_lock_timeout(t5_mutex_t m, uint32_t ms)
{
    return xSemaphoreTakeRecursive((SemaphoreHandle_t)m, pdMS_TO_TICKS(ms)) == pdTRUE;
}

void t5_mutex_unlock(t5_mutex_t m)
{
    xSemaphoreGiveRecursive((SemaphoreHandle_t)m);
}

int64_t t5_time_us(void)
{
    return esp_timer_get_time();
}

void t5_delay_ms(uint32_t ms)
{
    vTaskDelay(ms ? pdMS_TO_TICKS(ms) + (pdMS_TO_TICKS(ms) == 0) : 0);
}

void *t5_app_ctx_get(void)
{
    return pvTaskGetThreadLocalStoragePointer(NULL, T5_TLS_APP_CTX);
}

void t5_app_ctx_set(void *ctx)
{
    vTaskSetThreadLocalStoragePointer(NULL, T5_TLS_APP_CTX, ctx);
}

static t5_task_hook_t s_hook, s_exit_hook;

void t5_core_set_task_hook(t5_task_hook_t hook)
{
    s_hook = hook;
}

void t5_core_set_task_exit_hook(t5_task_hook_t hook)
{
    s_exit_hook = hook;
}

typedef struct {
    t5_task_fn_t fn;
    void *arg;
    void *ctx;
    bool caps;      /* made WithCaps: delete WithCaps too */
} tramp_t;

/* The stack guard fires only once SP is past the end of the stack, and the
 * interrupt saves its frame further down: without room there, an overflow
 * overwrites whatever heap block sits below the stack (found 2026-11-08: an
 * app's own control block). A task given a margin has its stack's lower
 * bound raised, so that happens inside memory of its own. */
#define MAX_MARGIN_TASKS 48
typedef struct {
    TaskHandle_t volatile task;
    void *stack;                /* the block's real start */
} margin_t;
static margin_t s_margin[MAX_MARGIN_TASKS];

void t5_task_guard_margin(t5_task_t task)
{
    for (int i = 0; i < MAX_MARGIN_TASKS; i++) {
        TaskHandle_t expect = NULL;
        if (__atomic_compare_exchange_n(&s_margin[i].task, &expect, (TaskHandle_t)task, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_RELAXED)) {
            StaticTask_t *tcb = (StaticTask_t *)task;     /* pxDummy6 is the TCB's pxStack */
            s_margin[i].stack = tcb->pxDummy6;
            tcb->pxDummy6 = (uint8_t *)tcb->pxDummy6 + T5_STACK_MARGIN;
            return;
        }
    }
}

static margin_t *margin_find(TaskHandle_t t)
{
    for (int i = 0; i < MAX_MARGIN_TASKS; i++) {
        if (s_margin[i].task == t) return &s_margin[i];
    }
    return NULL;
}

uint32_t t5_task_margin_of(t5_task_t task)
{
    return margin_find((TaskHandle_t)task) ? T5_STACK_MARGIN : 0;
}

/* As vTaskDeleteWithCaps, but freeing the block's real start. The TCB's
 * stack pointer can't be put back first: the port rewrites it on delete to
 * what it saw when the task first used the FPU. */
static void margin_delete(TaskHandle_t t, margin_t *m)
{
    void *stack = m->stack;
    StaticTask_t *tcb = (StaticTask_t *)t;
    m->task = NULL;
    vTaskSuspend(t);
    for (bool on_core = true; on_core;) {
        on_core = false;
        for (int c = 0; c < portNUM_PROCESSORS; c++) {
            if (xTaskGetCurrentTaskHandleForCore(c) == t) on_core = true;
        }
        if (on_core) taskYIELD();
    }
    vTaskDelete(t);              /* not current anywhere: done before it returns */
    heap_caps_free(stack);
    vPortFree(tcb);
}

static void margin_reaper(void *arg)
{
    TaskHandle_t t = arg;
    margin_t *m = margin_find(t);
    if (m) margin_delete(t, m);
    vTaskDelete(NULL);
}

static void trampoline(void *p)
{
    tramp_t t = *(tramp_t *)p;
    free(p);
    t5_app_ctx_set(t.ctx);
    t.fn(t.arg);
    if (s_exit_hook) s_exit_hook((t5_task_t)xTaskGetCurrentTaskHandle(), t5_app_ctx_get());
    t5_task_delete(NULL);
}

static t5_err_t task_create(const char *name, t5_task_fn_t fn, void *arg, uint32_t stack_bytes,
                            int prio, int core, t5_task_t *out, bool lp)
{
    if (!fn || stack_bytes < 1024) {
        return T5_ERR_INVALID_ARG;
    }
    tramp_t *t = malloc(sizeof *t);
    if (!t) {
        return T5_ERR_NO_MEM;
    }
    void *ctx = t5_app_ctx_get();
    *t = (tramp_t){ fn, arg, ctx, false };
    TaskHandle_t h = NULL;
    BaseType_t c = core == T5_CORE_ANY ? tskNO_AFFINITY : core;
    BaseType_t ok = pdFAIL;
    /* An app's task never takes internal RAM: however many it starts, the
     * OS keeps what Wi-Fi, DMA and its own tasks need. */
    if (ctx) {
        t->caps = true;
        ok = xTaskCreatePinnedToCoreWithCaps(trampoline, name, stack_bytes, t, prio, &h, c,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ok != pdPASS) {
            free(t);
            return T5_ERR_NO_MEM;
        }
        t5_task_guard_margin((t5_task_t)h);
    }
    /* Room for the stack and the task block, with some left over. */
    if (ok != pdPASS && lp && heap_caps_get_largest_free_block(MALLOC_CAP_RTCRAM) >= stack_bytes + 1024) {
        t->caps = true;
        ok = xTaskCreatePinnedToCoreWithCaps(trampoline, name, stack_bytes, t, prio, &h, c,
                                             MALLOC_CAP_RTCRAM | MALLOC_CAP_8BIT);
        if (ok != pdPASS) t->caps = false;
    }
    if (ok != pdPASS) ok = xTaskCreatePinnedToCore(trampoline, name, stack_bytes, t, prio, &h, c);
    if (ok != pdPASS) {
        free(t);
        return T5_ERR_NO_MEM;
    }
    if (s_hook) {
        s_hook((t5_task_t)h, ctx); /* not t->ctx: the task may already have freed t */
    }
    if (out) {
        *out = (t5_task_t)h;
    }
    return T5_OK;
}

t5_err_t t5_task_create(const char *name, t5_task_fn_t fn, void *arg, uint32_t stack_bytes,
                        int prio, int core, t5_task_t *out)
{
    return task_create(name, fn, arg, stack_bytes, prio, core, out, false);
}

t5_err_t t5_task_create_lp(const char *name, t5_task_fn_t fn, void *arg, uint32_t stack_bytes,
                           int prio, int core, t5_task_t *out)
{
    return task_create(name, fn, arg, stack_bytes, prio, core, out, true);
}

void t5_task_delete(t5_task_t task)
{
    TaskHandle_t t = task ? (TaskHandle_t)task : xTaskGetCurrentTaskHandle();
    margin_t *m = margin_find(t);
    if (m) {
        if (t != xTaskGetCurrentTaskHandle()) {
            margin_delete(t, m);
            return;
        }
        /* Deleting itself: something else has to free its stack. If that
         * can't be started the task just stays suspended. */
        (void)xTaskCreatePinnedToCore(margin_reaper, "t5_reap", 2048, t, uxTaskPriorityGet(t), NULL, xPortGetCoreID());
        vTaskSuspend(NULL);
        for (;;) {}
    }
    StackType_t *stack;
    StaticTask_t *tcb;
    if (xTaskGetStaticBuffers(t, &stack, &tcb) == pdTRUE) vTaskDeleteWithCaps(t);
    else vTaskDelete(t);
}

/* Tasks third-party code creates with internal stacks. It can't be told to
 * use PSRAM, so its creation call is caught here (--wrap). Only for tasks
 * that are never deleted: a task made WithCaps must be deleted WithCaps. */
BaseType_t __real_xTaskCreatePinnedToCore(TaskFunction_t fn, const char *const name, const configSTACK_DEPTH_TYPE stack,
                                          void *const arg, UBaseType_t prio, TaskHandle_t *const out, const BaseType_t core);
static volatile TaskHandle_t s_psram_stacks_for;

void t5_task_stacks_in_psram(bool on)
{
    s_psram_stacks_for = on ? xTaskGetCurrentTaskHandle() : NULL;
}

BaseType_t __wrap_xTaskCreatePinnedToCore(TaskFunction_t fn, const char *const name, const configSTACK_DEPTH_TYPE stack,
                                          void *const arg, UBaseType_t prio, TaskHandle_t *const out, const BaseType_t core)
{
    if (s_psram_stacks_for && s_psram_stacks_for == xTaskGetCurrentTaskHandle()) {
        return xTaskCreatePinnedToCoreWithCaps(fn, name, stack, arg, prio, out, core, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return __real_xTaskCreatePinnedToCore(fn, name, stack, arg, prio, out, core);
}

t5_task_t t5_task_self(void)
{
    return (t5_task_t)xTaskGetCurrentTaskHandle();
}
