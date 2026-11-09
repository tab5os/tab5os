/* SPDX-License-Identifier: Apache-2.0
 * Surviving a misbehaving app without a reboot.
 *
 * Native apps share the OS address space, so a bad pointer in an app used
 * to panic the whole device. Instead, when a CPU exception (bad load or
 * store, illegal instruction, jump to nowhere) happens in a task, the OS
 * asks a blame hook whether the fault belongs to an app. If it does, the
 * faulting task does not return to the instruction that faulted: it is sent
 * to a recovery routine instead, which either
 *
 *   - unwinds to the task's most recent recovery point (t5_fault_push +
 *     setjmp), for system tasks that call into apps (the UI task's loop,
 *     the event dispatcher), or
 *   - ends the task, for tasks that belong to the app.
 *
 * The same redirection stops a task stuck in app code (the UI watchdog's
 * "Close app"): t5_fault_interrupt_task().
 *
 * Anything the hook doesn't blame on an app panics and reboots as before.
 *
 *     t5_fault_point_t fp;
 *     t5_fault_push(&fp, app);
 *     if (setjmp(fp.jb) == 0) {
 *         app_callback();
 *     } else {
 *         // the app faulted (fp.info says how); it's already been reported
 *     }
 *     t5_fault_pop(&fp);
 *
 * The point must live on the stack of the task that pushes it, in the frame
 * that calls into the app: the stack between a fault and the point is what
 * the blame hook looks at.
 */
#pragma once

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include "t5_core/err.h"
#include "t5_core/os.h"

#ifdef __cplusplus
extern "C" {
#endif

/* info.cause: RISC-V mcause exception codes, or this for a stopped hang. */
#define T5_FAULT_HANG 0x100u
#define T5_FAULT_STACK 0x101u   /* the task ran off the end of its stack */

typedef struct {
    uint32_t cause;
    uint32_t pc, ra, sp;
    uint32_t addr;              /* mtval: the bad address or instruction */
    void *app;                  /* the app blamed (an app context) */
    void *task;
    bool guess;                 /* blamed without the app on the call chain */
} t5_fault_info_t;

typedef struct t5_fault_point {
    jmp_buf jb;
    struct t5_fault_point *prev;
    void *app;                  /* app the point protects, or NULL: whichever app is blamed */
    t5_fault_info_t info;       /* filled in before the unwind */
} t5_fault_point_t;

typedef enum {
    T5_BLAME_NONE,              /* not an app's fault: panic as usual */
    T5_BLAME_UNWIND,            /* unwind to the task's recovery point */
    T5_BLAME_END_TASK,          /* the task is the app's: end it */
} t5_blame_t;

/* What the blame hook sees. scan_lo..scan_hi is the stack between the fault
 * and the task's recovery point (empty if it has none): return addresses
 * into the app there mean the app is on the call chain. */
typedef struct {
    void *task;
    void *task_ctx;             /* the task's app context (t5_app_ctx) */
    const t5_fault_point_t *point;   /* the task's recovery point, or NULL */
    uintptr_t scan_lo, scan_hi;
    bool hang;                  /* t5_fault_interrupt_task, not an exception */
} t5_blame_args_t;

/* Runs in exception context (interrupts off, other core running): no
 * locks, no allocation, no logging. Set info->app to the app blamed. */
typedef t5_blame_t (*t5_fault_blame_fn_t)(const t5_blame_args_t *args, t5_fault_info_t *info);
/* Runs on the faulting task after it left the fault, before it unwinds or
 * ends (ends = true). Keep it short: the stack may be deep. */
typedef void (*t5_fault_report_fn_t)(const t5_fault_info_t *info, bool ends);

void t5_fault_set_hooks(t5_fault_blame_fn_t blame, t5_fault_report_fn_t report);

void t5_fault_push(t5_fault_point_t *fp, void *app);
/* Safe to call after an unwind (the point is already gone then). */
void t5_fault_pop(t5_fault_point_t *fp);

/* Stop a running or blocked task and, if the blame hook blames an app,
 * send it to its recovery point (or end it). T5_OK if redirected; the task
 * is left running untouched otherwise. Not for the calling task. */
T5_MUST_CHECK t5_err_t t5_fault_interrupt_task(t5_task_t task, uint32_t timeout_ms);

/* Faults survived since boot. */
uint32_t t5_fault_recovered(void);

#ifdef __cplusplus
}
#endif
