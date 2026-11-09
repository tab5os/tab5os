/* SPDX-License-Identifier: Apache-2.0
 * The small OS surface that service logic is allowed to use. On the device it
 * maps to FreeRTOS (t5_os_idf.c); host tests link t5_os_mock.c instead.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
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
/* Release m as many times as the calling task holds it. */
void t5_mutex_release_all(t5_mutex_t m);
/* *slot, created by the first caller, for a module whose functions may run
 * before its init and from several tasks at once. NULL if out of memory. */
static inline t5_mutex_t t5_mutex_once(t5_mutex_t *slot)
{
    t5_mutex_t m = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (m) return m;
    t5_mutex_t made = t5_mutex_create();
    if (!made) return NULL;
    if (__atomic_compare_exchange_n(slot, &m, made, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return made;
    t5_mutex_delete(made);
    return m;
}

/* Monotonic time since boot. */
int64_t t5_time_us(void);
static inline uint32_t t5_time_ms(void) { return (uint32_t)(t5_time_us() / 1000); }
void t5_delay_ms(uint32_t ms);

/* App context. Each task carries a pointer to the app it works for (NULL = the system).
 * The UI task sets it around app callbacks; t5_task_create() copies the
 * creator's pointer into the new task, so service calls know which app is
 * asking. t5_app defines what the pointer points to.
 */
void *t5_app_ctx_get(void);
void t5_app_ctx_set(void *ctx);

typedef void (*t5_task_fn_t)(void *arg);
typedef struct t5_task *t5_task_t;

#define T5_CORE_ANY (-1)

/* Create a task that inherits the caller's app context. core: 0, 1 or T5_CORE_ANY. */
t5_err_t t5_task_create(const char *name, t5_task_fn_t fn, void *arg, uint32_t stack_bytes,
                        int prio, int core, t5_task_t *out);

/* As t5_task_create, with the stack (and task block) in the 32 KB of LP RAM
 * when there is room, else in internal RAM as usual. For mostly idle system
 * service tasks: LP RAM is slower, but it is internal memory outside the
 * cache, so these tasks may still write flash, and it leaves the L2 RAM that
 * DMA can use (Wi-Fi, TLS, camera) to the things that need it. Not for tasks
 * that use the P4's SIMD (esp-dsp and the like): LP RAM has no 128-bit access. */
t5_err_t t5_task_create_lp(const char *name, t5_task_fn_t fn, void *arg, uint32_t stack_bytes,
                           int prio, int core, t5_task_t *out);

/* Called for every task t5_task_create() makes, with the inherited context.
 * The app resource tracker uses this to record app tasks. */
typedef void (*t5_task_hook_t)(t5_task_t task, void *app_ctx);
void t5_core_set_task_hook(t5_task_hook_t hook);
/* Called by a t5_task_create() task when its function returns, just before
 * it deletes itself, so the tracker forgets it. */
void t5_core_set_task_exit_hook(t5_task_hook_t hook);

/* Keep this many bytes at the bottom of a task's stack out of use, so a
 * stack overflow is caught before it reaches other memory. */
#define T5_STACK_MARGIN 512u
void t5_task_guard_margin(t5_task_t task);
uint32_t t5_task_margin_of(t5_task_t task);
/* Delete a task (NULL: the caller), however its stack was allocated. */
void t5_task_delete(t5_task_t task);
/* While on, tasks the calling task creates through plain FreeRTOS calls
 * (third-party drivers) get PSRAM stacks. Those tasks must never be deleted. */
void t5_task_stacks_in_psram(bool on);
/* The calling task (an opaque id, never NULL). */
t5_task_t t5_task_self(void);

#ifdef __cplusplus
}
#endif
