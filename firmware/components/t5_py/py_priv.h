/* SPDX-License-Identifier: Apache-2.0
 * Shared between the VM core and the t5 modules. Only one VM exists at a
 * time, so module state is global (reset per VM in py_modules_reset()).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "t5_core/err.h"
#include "t5_gui/t5ui.h"
#include "t5_app/fspolicy.h"
#include "t5_py/py.h"

#include "py/obj.h"


struct t5_py {
    t5_py_cfg_t cfg;
    char *path, *code;
    char cwd[T5_FS_PATH_MAX];
    void *app;                     /* t5_app_t of the owner, NULL = system */
    uint32_t perms;                /* T5_PERM_* the owner may use (all for system) */
    TaskHandle_t task;
    uint8_t *heap;
    size_t heap_size;
    StreamBufferHandle_t in;       /* REPL input */
    volatile bool stop;            /* asked to stop */
    volatile bool back;            /* the person pressed back in the script's UI: finish */
    volatile bool finished;        /* the VM task is done with the heap */
    volatile bool closing;         /* the VM is tearing down: no more callbacks */
    volatile bool in_main;         /* running the script itself (not the event loop or REPL) */
    volatile t5_py_state_t state;
    bool ui_used;                  /* t5.ui.screen() was called: keep running for callbacks */
    t5ui_obj_t *ui_root;           /* the container t5.ui.screen() returned */
};

/* The VM being run (valid on the VM task). */
extern t5_py_t *py_vm;

/* A mutex made on first use, by whichever task gets there first. */
SemaphoreHandle_t py_mutex_once(SemaphoreHandle_t *slot);

/* Helpers for module code (VM task). */
/* The errno for a t5 error. Sockets name the states the C API reports
 * generically (T5_ERR_BUSY: the address is in use, T5_ERR_STATE: not
 * connected; anything unexpected: the connection was reset). */
typedef enum { PY_ERRNO_GENERIC, PY_ERRNO_SOCKET } py_errno_ctx_t;
int py_errno(t5_err_t e, py_errno_ctx_t ctx);
/* Raise OSError(EACCES, msg). */
MP_NORETURN void py_raise_perm(const char *msg);
/* Raise OSError for a t5 error (with its text). */
MP_NORETURN void py_raise_t5(t5_err_t e);
static inline void py_check(t5_err_t e)
{
    if (e != T5_OK) py_raise_t5(e);
}
/* Raise PermissionError unless the owner may use perm (T5_PERM_*). Pass the
 * symbol's own permission from bind_perm.h (T5G_PERM_<symbol>, generated
 * from api_v1.sym), so Python asks for exactly what the native ABI does. */
void py_need_perm(uint32_t perm);
/* Take the UI lock, or unwind if the VM is being stopped while waiting. */
void py_ui_lock(void);
void py_ui_unlock(void);
/* Sleep ms, running scheduled callbacks; raises on interrupt or stop. */
void py_sleep_ms(uint32_t ms);
/* Wake the VM task (a callback was scheduled, input arrived, stop). */
void py_wake(void);
/* Schedule fn(arg) on the VM task, from any task (the UI task's event
 * callbacks). Drops it if the VM is stopping or the queue is full. */
void py_schedule(mp_obj_t fn, mp_obj_t arg);
/* Path checks: resolve against cwd, refuse what the owner may not touch.
 * Returns the absolute path in out or raises. */
void py_resolve_path(const char *in, bool write, char *out, size_t n);

/* Per-VM module state. */
void py_modules_reset(void);           /* VM task, after mp_init */
void py_ui_reset(void);                /* mod_ui.c, from py_modules_reset */
void py_ui_teardown_vm(void);          /* VM task, before the heap goes: detach if the lock is free */
void py_ui_detach_all(t5_py_t *vm);    /* stopper, under the UI lock: drop every callback into the heap */
void py_net_reset(void);              /* VM task: Stop cancels network waits */
void py_app_lifecycle(int what);       /* 0 suspend, 1 resume, from the app runtime (UI task) */
void py_app_rotate(int degrees);
bool py_ui_wants_events(void);         /* the script left UI callbacks behind */

/* What the UI and the OS hold for a callback into the heap: a slot number
 * tagged with the VM generation, which changes before a heap goes, so a
 * late callback finds nothing. */
extern volatile uint32_t py_gen;
static inline uintptr_t py_tag(uint32_t slot)
{
    return ((uintptr_t)(py_gen & 0xffff) << 16) | (slot & 0xffff);
}
/* The slot, or -1 for a tag of an earlier VM or one that is closing. */
static inline int py_untag(uintptr_t h)
{
    if ((h >> 16) != (py_gen & 0xffff) || !py_vm || py_vm->closing) return -1;
    return (int)(h & 0xffff);
}

/* Callables the OS calls back later (dialogs, snackbars, permission
 * answers), in tagged slots (mod_t5.c). */
uintptr_t py_slot_put(mp_obj_t fn);
mp_obj_t py_slot_take(uintptr_t h);     /* UI task: NULL if stale */

/* mod_socket.c: when Python last polled a socket (select, asyncio); the VM
 * wakes more often for a while after. */
extern uint32_t py_net_poll_tick;
