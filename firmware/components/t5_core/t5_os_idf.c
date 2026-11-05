/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
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

static t5_task_hook_t s_hook;

void t5_core_set_task_hook(t5_task_hook_t hook)
{
    s_hook = hook;
}

typedef struct {
    t5_task_fn_t fn;
    void *arg;
    void *ctx;
} tramp_t;

static void trampoline(void *p)
{
    tramp_t t = *(tramp_t *)p;
    free(p);
    t5_app_ctx_set(t.ctx);
    t.fn(t.arg);
    vTaskDelete(NULL);
}

t5_err_t t5_task_create(const char *name, t5_task_fn_t fn, void *arg, uint32_t stack_bytes,
                        int prio, int core, t5_task_t *out)
{
    if (!fn || stack_bytes < 1024) {
        return T5_ERR_INVALID_ARG;
    }
    tramp_t *t = malloc(sizeof *t);
    if (!t) {
        return T5_ERR_NO_MEM;
    }
    void *ctx = t5_app_ctx_get();
    *t = (tramp_t){ fn, arg, ctx };
    TaskHandle_t h = NULL;
    BaseType_t ok = xTaskCreatePinnedToCore(trampoline, name, stack_bytes, t, prio, &h,
                                            core == T5_CORE_ANY ? tskNO_AFFINITY : core);
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

t5_task_t t5_task_self(void)
{
    return (t5_task_t)xTaskGetCurrentTaskHandle();
}
