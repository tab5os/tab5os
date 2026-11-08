/* SPDX-License-Identifier: Apache-2.0
 * Shared between the VM core and the t5 modules. Only one VM exists at a
 * time, so module state is global (reset per VM in py_modules_reset()).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "t5_core/err.h"
#include "t5_pkg/manifest.h"
#include "t5_py/py.h"

#include "py/obj.h"

#define PY_PATH_MAX 192
#define PY_MAX_ROOTS 6

typedef struct {
    char path[PY_PATH_MAX];
    bool writable;
} py_root_t;

struct t5_py {
    t5_py_cfg_t cfg;
    char *path, *code;
    char cwd[PY_PATH_MAX];
    void *app;                     /* t5_app_t of the owner, NULL = system */
    uint32_t perms;                /* T5_PERM_* the owner may use (all for system) */
    TaskHandle_t task;
    uint8_t *heap;
    size_t heap_size;
    StreamBufferHandle_t in;       /* REPL input */
    py_root_t roots[PY_MAX_ROOTS]; /* where files may be opened */
    int nroots;
    volatile bool stop;            /* asked to stop */
    volatile bool back;            /* the person pressed back in the script's UI: finish */
    volatile bool finished;        /* the VM task is done with the heap */
    volatile bool closing;         /* the VM is tearing down: no more callbacks */
    volatile bool in_main;         /* running the script itself (not the event loop or REPL) */
    volatile t5_py_state_t state;
    bool ui_used;                  /* t5.ui.screen() was called: keep running for callbacks */
    lv_obj_t *ui_root;             /* the container t5.ui.screen() returned */
};

/* The VM being run (valid on the VM task). */
extern t5_py_t *py_vm;

/* ---- helpers for module code (VM task) ------------------------------------- */
/* Raise OSError(EACCES, msg). */
MP_NORETURN void py_raise_perm(const char *msg);
/* Raise OSError for a t5 error (with its text). */
MP_NORETURN void py_raise_t5(t5_err_t e);
static inline void py_check(t5_err_t e)
{
    if (e != T5_OK) py_raise_t5(e);
}
/* Raise PermissionError unless the owner may use perm (T5_PERM_*). */
void py_need_perm(uint32_t perm);
/* Take the UI lock, or unwind if the VM is being stopped while waiting. */
void py_ui_lock(void);
void py_ui_unlock(void);
/* Sleep ms, running scheduled callbacks; raises on interrupt or stop. */
void py_sleep_ms(uint32_t ms);
/* Wake the VM task (a callback was scheduled, input arrived, stop). */
void py_wake(void);
/* Schedule fn(arg) on the VM task, from any task (the UI task's LVGL
 * callbacks). Drops it if the VM is stopping or the queue is full. */
void py_schedule(mp_obj_t fn, mp_obj_t arg);
/* Path checks: resolve against cwd, refuse anything outside the roots.
 * Returns the absolute path in out or raises. */
void py_resolve_path(const char *in, bool write, char *out, size_t n);
bool py_path_allowed(const char *abs, bool write);

/* ---- per-VM module state -------------------------------------------------------- */
void py_modules_reset(void);           /* VM task, after mp_init */
void py_ui_teardown_vm(void);          /* VM task, before the heap goes: detach if the lock is free */
void py_ui_detach_all(t5_py_t *vm);    /* stopper, under the UI lock: drop every callback into the heap */
void py_net_reset(void);              /* VM task: Stop cancels network waits */
void py_app_lifecycle(int what);       /* 0 suspend, 1 resume, from the app runtime (UI task) */
void py_app_rotate(int degrees);
bool py_ui_wants_events(void);         /* the script left UI callbacks behind */

/* Callables the OS calls back later (dialogs, snackbars, permission
 * answers): a slot number tagged with the VM generation (mod_t5.c). */
uintptr_t py_slot_put(mp_obj_t fn);
mp_obj_t py_slot_take(uintptr_t h);     /* UI task: NULL if stale */

/* fs.c */
void py_fs_setup_roots(t5_py_t *vm);
