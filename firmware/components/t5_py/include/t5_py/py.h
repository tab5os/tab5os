/* SPDX-License-Identifier: Apache-2.0
 * MicroPython runtime (plan 8.1-8.3).
 *
 * One VM at a time, on its own task, with a GC heap taken from PSRAM. The
 * VM is recorded as a resource of the app that started it, so a crash or
 * exit stops it and frees the heap; the heap itself is a plain PSRAM block,
 * not counted in the app's heap figures. The VM runs a script file, a
 * string, or an interactive REPL fed with t5_py_input().
 *
 * Containment (plan 8.2): the build has no `machine`, `uctypes`, native or
 * viper emitters, inline assembler, .mpy loading or raw sockets. An app's
 * files follow t5_app/fspolicy.h, as for native apps. Everything else goes
 * through the `t5` module, which checks the app's permissions like the
 * native ABI. tools/py/check_modules.py fails the build if a module outside
 * the allowlist appears.
 *
 * Python apps (runtime = "micropython") are launched by the app runtime
 * through the descriptor t5_py_init() registers. The Terminal (REPL) and the
 * Editor (run a script) use t5_py_start() directly.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

struct _lv_obj_t;
typedef struct t5_py t5_py_t;

typedef enum {
    T5_PY_RUNNING,
    T5_PY_DONE,            /* finished normally */
    T5_PY_EXCEPTION,       /* an uncaught exception (printed to the output) */
    T5_PY_STOPPED,         /* stopped from outside (app exit, Stop button) */
} t5_py_state_t;

/* "running", "done", "exception", "stopped" (the developer API's X-Py-State). */
const char *t5_py_state_name(t5_py_state_t st);

/* Output sink: called on the VM task with stdout bytes (also tracebacks). */
typedef void (*t5_py_out_fn_t)(const char *s, size_t n, void *arg);
/* The VM has finished its script (and, for scripts with UI callbacks, the
 * person closed it or it called t5.app.exit()). Called on the VM task. The
 * owner must still call t5_py_stop() to free it. */
typedef void (*t5_py_done_fn_t)(t5_py_t *vm, t5_py_state_t how, void *arg);

typedef struct {
    const char *path;              /* script to run, or NULL */
    const char *code;              /* or source text (copied) */
    bool repl;                     /* then (or instead) an interactive REPL */
    struct _lv_obj_t *ui_parent;   /* where t5.ui.screen() builds; NULL = no UI */
    size_t heap_bytes;             /* GC heap; 0 = default (2 MB) */
    const char *cwd;               /* relative paths and the first sys.path entry; NULL = the app's data dir */
    t5_py_out_fn_t out;
    t5_py_done_fn_t done;
    void *arg;
} t5_py_cfg_t;

#define T5_PY_HEAP_DEFAULT (2u * 1024u * 1024u)
#define T5_PY_HEAP_MAX (8u * 1024u * 1024u)

/* Register the MicroPython runtime with the app runtime. Before t5_apps_init(). */
t5_err_t t5_py_init(void);

/* Start a VM for the calling task's app (or the system). T5_ERR_BUSY if one
 * is already running. */
t5_err_t t5_py_start(const t5_py_cfg_t *cfg, t5_py_t **out);
/* Interactive input for a REPL VM (characters as typed; "\r" ends a line,
 * "\x03" interrupts). Safe from any task. */
t5_err_t t5_py_input(t5_py_t *vm, const char *s, size_t n);
/* Raise KeyboardInterrupt in the running code. */
void t5_py_interrupt(t5_py_t *vm);
/* Stop (if still running) and free. Any task, including the UI task while
 * it holds the UI lock. Waits up to timeout_ms for the code to unwind, then
 * kills the task. vm is invalid afterwards. */
t5_err_t t5_py_stop(t5_py_t *vm, uint32_t timeout_ms);
/* Wait up to timeout_ms (T5_PY_FOREVER: no limit) for the VM to finish:
 * true once it has (its done callback has run). By its owner, before
 * t5_py_stop(). */
#define T5_PY_FOREVER UINT32_MAX
bool t5_py_wait(t5_py_t *vm, uint32_t timeout_ms);
/* A VM exists (running or finished but not yet freed). */
bool t5_py_busy(void);

/* ---- console view (UI task, or under the UI lock) ------------------------
 * A scrolling monospace text view for program output, shared by Python apps
 * (before they build their own UI), the Editor's output pane and the
 * Terminal. Writes are safe from any task; the view catches up on the UI
 * task. Keeps the last 16 KB. */
struct _lv_obj_t *t5_py_console_create(struct _lv_obj_t *parent);
void t5_py_console_write(struct _lv_obj_t *con, const char *s, size_t n);
void t5_py_console_clear(struct _lv_obj_t *con);

/* ---- for tests and the console ------------------------------------------- */
/* Run source text headless (system context), collect output, wait for it.
 * Returns the end state; out gets the output (truncated to n). */
t5_py_state_t t5_py_exec_capture(const char *code, char *out, size_t n, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
