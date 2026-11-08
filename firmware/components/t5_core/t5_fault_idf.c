/* SPDX-License-Identifier: Apache-2.0
 * App fault recovery (fault.h) on ESP-IDF, RISC-V.
 *
 * IDF's exception vector saves the registers in a frame on the faulting
 * task's stack and calls xt_unhandled_exception(frame); if that returns,
 * the vector restores the frame and resumes at frame->mepc. We wrap it
 * (-Wl,--wrap=xt_unhandled_exception): when the blame hook says the fault
 * is an app's, mepc is pointed at a recovery routine and we return, so the
 * task carries on there instead of at the faulting instruction. Everything
 * else goes to IDF's panic handler unchanged.
 *
 * A task switched out by the scheduler has the same frame layout at the
 * top of its stack (TCB.pxTopOfStack), which is how a hung task is moved.
 */
#include <string.h>
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_private/cache_utils.h"
#include "esp_private/hw_stack_guard.h"
#include "hal/assist_debug_hal.h"
#include "soc/interrupts.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "riscv/rvruntime-frames.h"
#include "t5_core/fault.h"

/* IDF's port (port.c): per-core scheduler state. */
extern volatile UBaseType_t port_xSchedulerRunning[portNUM_PROCESSORS];
extern volatile UBaseType_t port_uxInterruptNesting[portNUM_PROCESSORS];
extern volatile UBaseType_t port_uxCriticalNesting[portNUM_PROCESSORS];

void __real_xt_unhandled_exception(void *frame);
void __real_panicHandler(void *frame);

#define TLS_APP_CTX (CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS - 1)   /* as t5_os_idf.c */
#define MAX_POINT_TASKS 16
#define MAX_DYING 8

/* Each task's innermost recovery point. A slot is claimed with a CAS and
 * only ever changed by its own task, so the exception path can read it
 * without a lock. */
typedef struct {
    void *volatile task;
    t5_fault_point_t *volatile top;
} point_slot_t;

/* Tasks on their way to ending, with what happened. */
typedef struct {
    void *volatile task;
    t5_fault_info_t info;
} dying_t;

static DRAM_ATTR point_slot_t s_points[MAX_POINT_TASKS];
static DRAM_ATTR dying_t s_dying[MAX_DYING];
static DRAM_ATTR t5_fault_blame_fn_t s_blame;
static DRAM_ATTR t5_fault_report_fn_t s_report;
static DRAM_ATTR volatile uint32_t s_recovered;

void t5_fault_set_hooks(t5_fault_blame_fn_t blame, t5_fault_report_fn_t report)
{
    s_report = report;
    s_blame = blame;
}

uint32_t t5_fault_recovered(void)
{
    return s_recovered;
}

void *t5_app_ctx_of(t5_task_t task)
{
    return task ? pvTaskGetThreadLocalStoragePointer((TaskHandle_t)task, TLS_APP_CTX) : NULL;
}

void t5_mutex_release_all(t5_mutex_t m)
{
    if (!m) return;
    while (xSemaphoreGetMutexHolder((SemaphoreHandle_t)m) == xTaskGetCurrentTaskHandle()) {
        if (xSemaphoreGiveRecursive((SemaphoreHandle_t)m) != pdTRUE) break;
    }
}

bool t5_mutex_held(t5_mutex_t m)
{
    return m && xSemaphoreGetMutexHolder((SemaphoreHandle_t)m) == xTaskGetCurrentTaskHandle();
}

/* ---- recovery points ------------------------------------------------------ */
static point_slot_t *IRAM_ATTR slot_find(void *task)
{
    for (int i = 0; i < MAX_POINT_TASKS; i++) {
        if (s_points[i].task == task) return &s_points[i];
    }
    return NULL;
}

void t5_fault_push(t5_fault_point_t *fp, void *app)
{
    void *self = xTaskGetCurrentTaskHandle();
    fp->app = app;
    fp->prev = NULL;
    memset(&fp->info, 0, sizeof fp->info);
    point_slot_t *s = slot_find(self);
    for (int i = 0; !s && i < MAX_POINT_TASKS; i++) {
        void *expect = NULL;
        if (__atomic_compare_exchange_n(&s_points[i].task, &expect, self, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            s = &s_points[i];
            s->top = NULL;
        }
    }
    if (!s) return;              /* table full: this call isn't protected */
    fp->prev = s->top;
    __atomic_store_n(&s->top, fp, __ATOMIC_RELEASE);
}

static void pop_slot(point_slot_t *s, t5_fault_point_t *fp)
{
    __atomic_store_n(&s->top, fp->prev, __ATOMIC_RELEASE);
    if (!fp->prev) __atomic_store_n(&s->task, NULL, __ATOMIC_RELEASE);
}

void t5_fault_pop(t5_fault_point_t *fp)
{
    point_slot_t *s = slot_find(xTaskGetCurrentTaskHandle());
    if (s && s->top == fp) pop_slot(s, fp);
}

/* ---- where a recovered task resumes ------------------------------------------ */
/* In case it was the stack guard that sent this task here. The scheduler
 * also turns it on at every context switch; starting it twice is harmless. */
static void guard_rearm(void)
{
    UBaseType_t st = portSET_INTERRUPT_MASK_FROM_ISR();
    esp_hw_stack_guard_monitor_start();
    portCLEAR_INTERRUPT_MASK_FROM_ISR(st);
}

static void __attribute__((noreturn, used)) unwind_entry(void)
{
    guard_rearm();
    point_slot_t *s = slot_find(xTaskGetCurrentTaskHandle());
    t5_fault_point_t *fp = s ? s->top : NULL;
    if (!fp) abort();            /* can't happen: redirect checked it */
    pop_slot(s, fp);
    if (s_report) s_report(&fp->info, false);
    longjmp(fp->jb, 1);
}

static dying_t *dying_find(void *task)
{
    for (int i = 0; i < MAX_DYING; i++) {
        if (s_dying[i].task == task) return &s_dying[i];
    }
    return NULL;
}

static void __attribute__((noreturn, used)) end_entry(void)
{
    guard_rearm();
    void *self = xTaskGetCurrentTaskHandle();
    dying_t *d = dying_find(self);
    t5_fault_info_t info;
    if (d) info = d->info;
    else memset(&info, 0, sizeof info);
    if (s_report) s_report(&info, true);
    point_slot_t *s = slot_find(self);
    if (s) {
        s->top = NULL;
        __atomic_store_n(&s->task, NULL, __ATOMIC_RELEASE);
    }
    if (d) __atomic_store_n(&d->task, NULL, __ATOMIC_RELEASE);
    t5_task_delete(NULL);
    for (;;) {}
}

/* The vector doesn't restore SP from the frame: resume here, with the new
 * stack pointer in a0, when the old one is unusable. */
__attribute__((naked, used)) static void unwind_on_new_stack(void)
{
    __asm__("mv sp, a0\n j unwind_entry");
}

__attribute__((naked, used)) static void end_on_new_stack(void)
{
    __asm__("mv sp, a0\n j end_entry");
}

/* ---- deciding ------------------------------------------------------------------------ */
/* Ask the blame hook; on a yes, point the saved frame at a recovery routine. */
static bool IRAM_ATTR redirect(void *task, RvExcFrame *f, t5_fault_info_t *info, bool hang, uint32_t stack_top)
{
    point_slot_t *s = slot_find(task);
    t5_fault_point_t *fp = s ? s->top : NULL;
    uintptr_t sp = info->sp;
    t5_blame_args_t a = {
        .task = task,
        .task_ctx = pvTaskGetThreadLocalStoragePointer((TaskHandle_t)task, TLS_APP_CTX),
        .point = fp,
        .scan_lo = sp,
        .scan_hi = fp && (uintptr_t)fp > sp ? (uintptr_t)fp : sp,
        .hang = hang,
    };
    t5_blame_t b = s_blame(&a, info);
    if (b == T5_BLAME_UNWIND && fp) {
        fp->info = *info;
        f->mepc = (uintptr_t)unwind_entry;
        f->ra = 0;
        /* Out of stack: resume just below the point, whose frame is intact. */
        if (stack_top) {
            f->mepc = (uintptr_t)unwind_on_new_stack;
            f->a0 = ((uintptr_t)fp - 1024) & ~(uintptr_t)15;
        }
        return true;
    }
    if (b == T5_BLAME_END_TASK) {
        for (int i = 0; i < MAX_DYING; i++) {
            void *expect = NULL;
            if (__atomic_compare_exchange_n(&s_dying[i].task, &expect, task, false, __ATOMIC_ACQ_REL,
                                            __ATOMIC_RELAXED)) {
                s_dying[i].info = *info;
                f->mepc = (uintptr_t)end_entry;
                f->ra = 0;
                if (stack_top) {                /* nothing on the old stack is needed again */
                    f->mepc = (uintptr_t)end_on_new_stack;
                    f->a0 = (stack_top - 64) & ~(uint32_t)15;
                }
                return true;
            }
        }
    }
    return false;
}

static bool IRAM_ATTR try_recover(RvExcFrame *f)
{
    if (!s_blame) return false;
    uint32_t cause = (uint32_t)f->mcause;
    /* Misaligned or faulting fetch, load or store; illegal instruction.
     * Not breakpoints (debugger, abort paths) or environment calls. */
    if (cause > 7 || cause == 3) return false;
    /* The handler and everything it calls must be reachable. */
    if (!spi_flash_cache_enabled()) return false;
    int core = esp_cpu_get_core_id();
    if (!port_xSchedulerRunning[core] || port_uxInterruptNesting[core] || port_uxCriticalNesting[core]) return false;
    if (!(f->mstatus & MSTATUS_MPIE)) return false;     /* interrupts were off: inside a critical section */
    void *task = xTaskGetCurrentTaskHandleForCore(core);
    /* Faulting again on its way out: an ending task panics; an unwinding
     * one has already popped its point, so only an outer point can catch. */
    if (!task || dying_find(task)) return false;
    t5_fault_info_t info = {
        .cause = cause, .pc = f->mepc, .ra = f->ra, .sp = f->sp, .addr = f->mtval, .task = task,
    };
    return redirect(task, f, &info, false, 0);
}

/* The stack guard (an assist-debug interrupt, so before the overflow writes
 * anything past the margin below the stack) comes here instead of to an
 * exception. Same decision; the task resumes with a usable stack. */
static bool IRAM_ATTR try_recover_stack(RvExcFrame *f)
{
    if (!s_blame || f->mcause != ETS_ASSIST_DEBUG_INUM) return false;
    if (!spi_flash_cache_enabled()) return false;
    int core = esp_cpu_get_core_id();
    if (esp_hw_stack_guard_get_fired_cpu() != (uint32_t)core) return false;
    if (!port_xSchedulerRunning[core] || port_uxInterruptNesting[core] || port_uxCriticalNesting[core]) return false;
    if (!(f->mstatus & MSTATUS_MPIE)) return false;
    void *task = xTaskGetCurrentTaskHandleForCore(core);
    if (!task || dying_find(task)) return false;
    uint32_t sp_min, sp_max;
    esp_hw_stack_guard_get_bounds(core, &sp_min, &sp_max);
    /* The stack the blame hook scans starts where it is valid. */
    t5_fault_info_t info = {
        .cause = T5_FAULT_STACK, .pc = f->mepc, .ra = f->ra, .sp = f->sp < sp_min ? sp_min : f->sp, .addr = f->sp,
        .task = task,
    };
    if (!redirect(task, f, &info, false, sp_max)) return false;
    /* SP is still out of bounds until the frame is restored, which would
     * fire this again: off here, on again where the task resumes. */
    esp_hw_stack_guard_monitor_stop();
    assist_debug_hal_sp_int_clear(core);
    return true;
}

void IRAM_ATTR __wrap_panicHandler(void *frame)
{
    /* The guard's interrupt reaches both cores. On the other one there is
     * nothing to do (IDF's handler would park it forever, expecting a panic
     * that this recovery avoids). If the core that fired does panic, it
     * stalls this one itself. */
    if (((RvExcFrame *)frame)->mcause == ETS_ASSIST_DEBUG_INUM &&
        esp_hw_stack_guard_get_fired_cpu() != (uint32_t)esp_cpu_get_core_id()) {
        return;
    }
    if (try_recover_stack((RvExcFrame *)frame)) {
        s_recovered++;
        return;
    }
    __real_panicHandler(frame);
}

void IRAM_ATTR __wrap_xt_unhandled_exception(void *frame)
{
    if (try_recover((RvExcFrame *)frame)) {
        s_recovered++;
        return;                  /* the vector resumes at the new mepc */
    }
    __real_xt_unhandled_exception(frame);
}

/* ---- hung tasks --------------------------------------------------------------------- */
static bool running_anywhere(TaskHandle_t t)
{
    for (int c = 0; c < portNUM_PROCESSORS; c++) {
        if (xTaskGetCurrentTaskHandleForCore(c) == t) return true;
    }
    return false;
}

t5_err_t t5_fault_interrupt_task(t5_task_t task, uint32_t timeout_ms)
{
    TaskHandle_t t = (TaskHandle_t)task;
    if (!t || t == xTaskGetCurrentTaskHandle() || !s_blame) return T5_ERR_INVALID_ARG;
    vTaskSuspend(t);
    /* Suspending a task running on the other core takes effect at its next
     * interrupt; a task in a critical section never gets there. */
    TickType_t t0 = xTaskGetTickCount();
    while (running_anywhere(t)) {
        if ((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS > timeout_ms) {
            vTaskResume(t);
            return T5_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    /* Switched out: its registers are in an interrupt frame at the top of
     * its stack (pxTopOfStack is the TCB's first field). Only the general
     * registers and mepc are in that frame. */
    RvExcFrame *f = *(RvExcFrame **)t;
    t5_fault_info_t info = {
        .cause = T5_FAULT_HANG, .pc = f->mepc, .ra = f->ra, .sp = f->sp, .task = t,
    };
    bool ok = redirect(t, f, &info, true, 0);
    vTaskResume(t);
    if (!ok) return T5_ERR_NOT_SUPPORTED;
    s_recovered++;
    return T5_OK;
}
