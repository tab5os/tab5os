/* SPDX-License-Identifier: Apache-2.0
 * The small OS surface that service logic is allowed to use. On the device it
 * maps to FreeRTOS (t5_os_idf.c); host tests link t5_os_mock.c instead.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Recursive mutex. */
typedef struct t5_mutex *t5_mutex_t;
t5_mutex_t t5_mutex_create(void);
void t5_mutex_delete(t5_mutex_t m);
void t5_mutex_lock(t5_mutex_t m);
bool t5_mutex_lock_timeout(t5_mutex_t m, uint32_t ms);
void t5_mutex_unlock(t5_mutex_t m);

/* Monotonic time since boot. */
int64_t t5_time_us(void);
static inline uint32_t t5_time_ms(void) { return (uint32_t)(t5_time_us() / 1000); }
void t5_delay_ms(uint32_t ms);

/* ---- App context -------------------------------------------------------
 * Each task carries a pointer to the app it works for (NULL = the system).
 * The UI task sets it around app callbacks; t5_task_create() copies the
 * creator's pointer into the new task, so service calls know which app is
 * asking. Phase 5 defines what the pointer points to.
 */
void *t5_app_ctx_get(void);
void t5_app_ctx_set(void *ctx);

typedef void (*t5_task_fn_t)(void *arg);
typedef struct t5_task *t5_task_t;

#define T5_CORE_ANY (-1)

/* Create a task that inherits the caller's app context. core: 0, 1 or T5_CORE_ANY. */
t5_err_t t5_task_create(const char *name, t5_task_fn_t fn, void *arg, uint32_t stack_bytes,
                        int prio, int core, t5_task_t *out);

/* Called for every task t5_task_create() makes, with the inherited context.
 * Phase 5's resource tracker uses this to record app tasks. */
typedef void (*t5_task_hook_t)(t5_task_t task, void *app_ctx);
void t5_core_set_task_hook(t5_task_hook_t hook);

/* The calling task (an opaque id, never NULL). */
t5_task_t t5_task_self(void);

#ifdef __cplusplus
}
#endif
