/* SPDX-License-Identifier: Apache-2.0
 * The MicroPython VM (plan 8.1): one at a time, on its own task, with a GC
 * heap in PSRAM.
 *
 * Lifetime: t5_py_start() makes the task; it runs the script and/or the
 * REPL, then (if the script built UI with callbacks) an event loop until it
 * is stopped. Whoever started it calls t5_py_stop(), which aborts the code
 * if it is still running (an abort Python can't catch), waits for the task
 * to unwind, detaches LVGL callbacks that point into the heap, and frees
 * everything. If the owning app exits without doing so, the tracker does it
 * (the VM is recorded as a T5_RES_OTHER resource of the app).
 *
 * The VM task itself is not given to the tracker: a forced task delete must
 * go through t5_py_stop(), which knows about the heap and the callbacks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "py/compile.h"
#include "py/cstack.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/nlr.h"
#include "py/repl.h"
#include "py/runtime.h"
#include "py/stackctrl.h"
#include "py/stream.h"
#include "shared/runtime/gchelper.h"
#include "shared/runtime/pyexec.h"
#include "t5_app/app.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_core/res.h"
#include "t5_storage/paths.h"
#include "t5_ui/t5_ui.h"
#include "py_priv.h"

static const char *TAG = "t5.py";

#define VM_STACK_BYTES (24 * 1024)   /* internal RAM: tasks that write flash need it */
#define VM_PRIO 3
#define REPL_IN_BYTES 512

t5_py_t *py_vm;                        /* the VM (one at a time) */
static SemaphoreHandle_t s_lock;       /* guards py_vm across start/stop */
static EventGroupHandle_t s_events;
#define VM_ENDED BIT0                  /* the VM task is done with the VM (set after finished) */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

SemaphoreHandle_t py_mutex_once(SemaphoreHandle_t *slot)
{
    if (*slot) return *slot;
    SemaphoreHandle_t l = xSemaphoreCreateMutex();
    portENTER_CRITICAL(&s_mux);
    if (!*slot) {
        *slot = l;
        l = NULL;
    }
    portEXIT_CRITICAL(&s_mux);
    if (l) vSemaphoreDelete(l);
    return *slot;
}

const char *t5_py_state_name(t5_py_state_t st)
{
    switch (st) {
    case T5_PY_RUNNING: return "running";
    case T5_PY_DONE: return "done";
    case T5_PY_EXCEPTION: return "exception";
    case T5_PY_STOPPED: return "stopped";
    default: return "?";
    }
}

uint32_t t5_py_atomic_begin(void)
{
    portENTER_CRITICAL(&s_mux);
    return 0;
}

void t5_py_atomic_end(uint32_t state)
{
    (void)state;
    portEXIT_CRITICAL(&s_mux);
}

uint32_t t5_py_random_seed(void)
{
    return esp_random();
}

void py_wake(void)
{
    t5_py_t *vm = py_vm;
    if (vm && vm->task && !vm->finished) xTaskNotifyGive(vm->task);
}

void t5_py_wait_event(uint32_t timeout_ms)
{
    /* timeout_ms is (mp_uint_t)-1 for "indefinitely": wake at least every
     * 100 ms anyway, so a stop request is never missed. */
    uint32_t cap = 100;
    if (mp_hal_ticks_ms() - py_net_poll_tick < 500) cap = 10;
    uint32_t ms = timeout_ms > cap ? cap : timeout_ms;
    if (py_vm && !py_vm->stop) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms ? ms : 1));
}

void mp_hal_delay_ms(mp_uint_t ms)
{
    py_sleep_ms(ms);
}

void py_sleep_ms(uint32_t ms)
{
    uint32_t t0 = mp_hal_ticks_ms();
    for (;;) {
        uint32_t el = mp_hal_ticks_ms() - t0;
        if (el >= ms) break;
        mp_event_wait_ms(ms - el);       /* runs callbacks, raises KeyboardInterrupt or aborts */
    }
    mp_event_handle_nowait();
}

void mp_hal_delay_us(mp_uint_t us)
{
    if (us >= 1000) {
        py_sleep_ms(us / 1000);
        us %= 1000;
    }
    esp_rom_delay_us(us);
}

uint64_t mp_hal_time_ns(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000000ull + (uint64_t)tv.tv_usec * 1000ull;
}

void mp_hal_set_interrupt_char(int c)
{
    (void)c;      /* Ctrl-C is always an interrupt (t5_py_input) */
}

int mp_hal_stdin_rx_chr(void)
{
    /* Only the REPL reads input, and it is event driven (t5_py_input). */
    for (;;) {
        uint8_t c;
        if (py_vm && py_vm->in && xStreamBufferReceive(py_vm->in, &c, 1, pdMS_TO_TICKS(50)) == 1) return c;
        mp_event_handle_nowait();
    }
}

uintptr_t mp_hal_stdio_poll(uintptr_t poll_flags)
{
    uintptr_t r = poll_flags & MP_STREAM_POLL_WR;
    if ((poll_flags & MP_STREAM_POLL_RD) && py_vm && py_vm->in && xStreamBufferBytesAvailable(py_vm->in)) {
        r |= MP_STREAM_POLL_RD;
    }
    return r;
}

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len)
{
    t5_py_t *vm = py_vm;
    if (vm && vm->cfg.out && len) vm->cfg.out(str, len, vm->cfg.arg);
    return len;
}

void gc_collect(void)
{
    gc_collect_start();
    gc_helper_collect_regs_and_stack();
    gc_collect_end();
}

MP_NORETURN void nlr_jump_fail(void *val)
{
    T5_LOGE(TAG, "uncaught NLR %p", val);
    abort();
}

int py_errno(t5_err_t e, py_errno_ctx_t ctx)
{
    bool sock = ctx == PY_ERRNO_SOCKET;
    switch (e) {
    case T5_ERR_BUSY: return sock ? MP_EADDRINUSE : MP_EBUSY;
    case T5_ERR_STATE: return sock ? MP_ENOTCONN : MP_EIO;
    case T5_ERR_DENIED: return MP_EACCES;
    case T5_ERR_TIMEOUT: return MP_ETIMEDOUT;
    case T5_ERR_NO_MEM: return MP_ENOMEM;
    case T5_ERR_INVALID_ARG: return MP_EINVAL;
    case T5_ERR_NOT_FOUND: return MP_ENOENT;
    case T5_ERR_NOT_SUPPORTED: return MP_EOPNOTSUPP;
    case T5_ERR_NACK: return sock ? MP_ECONNRESET : MP_ENODEV;
    default: return sock ? MP_ECONNRESET : MP_EIO;
    }
}

MP_NORETURN void py_raise_perm(const char *msg)
{
    /* MicroPython has no PermissionError: OSError(EACCES, msg). */
    mp_obj_t args[2] = { MP_OBJ_NEW_SMALL_INT(MP_EACCES), mp_obj_new_str_from_cstr(msg) };
    nlr_raise(mp_obj_exception_make_new(&mp_type_OSError, 2, 0, args));
}

MP_NORETURN void py_raise_t5(t5_err_t e)
{
    if (e == T5_ERR_DENIED) {
        py_raise_perm(t5_err_str(e));
    }
    mp_obj_t args[2] = { MP_OBJ_NEW_SMALL_INT(py_errno(e, PY_ERRNO_GENERIC)), mp_obj_new_str_from_cstr(t5_err_str(e)) };
    nlr_raise(mp_obj_exception_make_new(&mp_type_OSError, 2, 0, args));
}

void py_need_perm(uint32_t perm)
{
    t5_py_t *vm = py_vm;
    if (!vm || !vm->app) return;                  /* system code may use everything */
    if (!t5_app_has_permission(vm->app, perm)) {
        char msg[64];
        snprintf(msg, sizeof msg, "needs the %s permission in manifest.toml", t5_perm_name(perm) ? t5_perm_name(perm) : "?");
        py_raise_perm(msg);
    }
}

/* Unwind the VM: an abort Python can't catch, else SystemExit. */
static MP_NORETURN void py_unwind(void)
{
    if (nlr_get_abort()) nlr_jump_abort();
    mp_raise_type(&mp_type_SystemExit);
}

void py_ui_lock(void)
{
    while (!t5_ui_lock_timeout(20)) {
        if (py_vm && py_vm->stop) py_unwind();
    }
    if (py_vm && py_vm->stop) {
        t5_ui_unlock();
        py_unwind();
    }
}

void py_ui_unlock(void)
{
    t5_ui_unlock();
}

void py_schedule(mp_obj_t fn, mp_obj_t arg)
{
    t5_py_t *vm = py_vm;
    if (!vm || vm->stop || vm->closing || fn == mp_const_none) return;
    if (!mp_sched_schedule(fn, arg)) T5_LOGW(TAG, "callback queue full, one dropped");
    py_wake();
}

/* Run scheduled callbacks until stopped (scripts that built a UI). */
static void event_loop(t5_py_t *vm)
{
    nlr_buf_t nlr;
    while (!vm->stop) {
        if (nlr_push(&nlr) == 0) {
            nlr_set_abort(&nlr);
            while (!vm->stop && py_ui_wants_events()) mp_event_wait_ms(100);
            nlr_pop();
            nlr_set_abort(NULL);
            break;
        }
        nlr_set_abort(NULL);
        if (nlr.ret_val == NULL) break;         /* aborted: stop */
        /* An exception in a callback: print it and carry on, like a REPL. */
        mp_obj_print_exception(&mp_plat_print, MP_OBJ_FROM_PTR(nlr.ret_val));
    }
}

static void repl_loop(t5_py_t *vm)
{
    pyexec_event_repl_init();
    while (!vm->stop) {
        uint8_t buf[64];
        size_t n = xStreamBufferReceive(vm->in, buf, sizeof buf, pdMS_TO_TICKS(50));
        for (size_t i = 0; i < n && !vm->stop; i++) {
            int r = pyexec_event_repl_process_char(buf[i]);
            if (r & PYEXEC_FORCED_EXIT) return;    /* Ctrl-D: the owner restarts it */
        }
        /* Callbacks scheduled by widgets made at the prompt. */
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            nlr_set_abort(&nlr);
            mp_event_handle_nowait();
            nlr_pop();
        } else if (nlr.ret_val) {
            mp_obj_print_exception(&mp_plat_print, MP_OBJ_FROM_PTR(nlr.ret_val));
        }
        nlr_set_abort(NULL);
    }
}

static void vm_task(void *arg)
{
    t5_py_t *vm = arg;
    t5_app_ctx_set(vm->app);        /* services see the app; the tracker doesn't own this task */
    volatile int top = 0;
    mp_cstack_init_with_top((void *)&top, VM_STACK_BYTES - 2048);
    gc_init(vm->heap, vm->heap + vm->heap_size);
    mp_init();
    /* sys.path: the script's directory, then lib/ under the working dir. */
    mp_obj_list_init(MP_OBJ_TO_PTR(mp_sys_path), 0);
    if (vm->path) {
        char dir[T5_FS_PATH_MAX];
        snprintf(dir, sizeof dir, "%s", vm->path);
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir) *slash = 0;
        mp_obj_list_append(mp_sys_path, mp_obj_new_str_from_cstr(dir));
    } else {
        mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR_));
    }
    char lib[T5_FS_PATH_MAX + 8];
    snprintf(lib, sizeof lib, "%s/lib", vm->cwd);
    mp_obj_list_append(mp_sys_path, mp_obj_new_str_from_cstr(lib));
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR__dot_frozen));   /* the frozen stdlib, last */
    mp_obj_list_init(MP_OBJ_TO_PTR(mp_sys_argv), 0);
    if (vm->path) mp_obj_list_append(mp_sys_argv, mp_obj_new_str_from_cstr(vm->path));
    py_modules_reset();

    t5_py_state_t how = T5_PY_DONE;
    int r = PYEXEC_NORMAL_EXIT;
    vm->in_main = true;
    if (vm->code) {               /* with a path too, the path only sets sys.path */
        vstr_t v;
        vstr_init_fixed_buf(&v, strlen(vm->code) + 1, vm->code);
        v.len = strlen(vm->code);
        r = pyexec_vstr(&v, true);
    } else if (vm->path) {
        r = pyexec_file(vm->path);
    }
    vm->in_main = false;
    if (vm->stop || r == PYEXEC_ABORT) how = T5_PY_STOPPED;
    else if (vm->back) how = T5_PY_DONE;                 /* back interrupted the script on purpose */
    else if (r & PYEXEC_UNHANDLED_EXCEPTION || r == PYEXEC_KEYBOARD_INTERRUPT) how = T5_PY_EXCEPTION;
    if (how != T5_PY_STOPPED) {
        if (vm->cfg.repl) repl_loop(vm);
        else if (how == T5_PY_DONE) event_loop(vm);
        if (vm->stop) how = T5_PY_STOPPED;
    }
    py_ui_teardown_vm();
    /* Finalisers close files and ports the script left open. */
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        gc_sweep_all();
        nlr_pop();
    }
    mp_deinit();
    vm->state = how;
    if (vm->cfg.done) vm->cfg.done(vm, how, vm->cfg.arg);
    vm->finished = true;            /* last touch of vm: the stopper may free it now */
    xEventGroupSetBits(s_events, VM_ENDED);
    vTaskDelete(NULL);
}

static void vm_free(t5_py_t *vm)
{
    if (vm->in) vStreamBufferDelete(vm->in);
    heap_caps_free(vm->heap);
    free(vm->path);
    free(vm->code);
    free(vm);
}

/* Stop, wait, detach, free. The VM lock is held. */
static void teardown(t5_py_t *vm, uint32_t timeout_ms)
{
    if (!vm->finished) {
        vm->stop = true;
        mp_sched_vm_abort();
        /* Every wait on the VM task is bounded (t5_py_wait_event), so one
         * wake-up is enough even if a wait other than the event loop's
         * takes it. */
        xTaskNotifyGive(vm->task);
        xEventGroupWaitBits(s_events, VM_ENDED, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
        if (!vm->finished) {
            T5_LOGW(TAG, "VM didn't stop within %u ms: killing its task", (unsigned)timeout_ms);
            vTaskDelete(vm->task);
            vm->finished = true;
            xEventGroupSetBits(s_events, VM_ENDED);
        }
    }
    /* Widgets may outlive the VM (they belong to the app's screen): make sure
     * none of them can call into the heap we are about to free. */
    bool on_ui = t5_ui_on_ui_task();
    if (!on_ui) t5_ui_lock();
    py_ui_detach_all(vm);
    /* The script's UI goes with it (the Editor and Terminal keep running). */
    if (vm->ui_root) lv_obj_delete(vm->ui_root);
    vm->ui_root = NULL;
    if (!on_ui) t5_ui_unlock();
    if (py_vm == vm) py_vm = NULL;
    vm_free(vm);
}

static void tracked_release(void *h)
{
    /* The owning app exited (or crashed) without stopping its VM. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (py_vm == h) teardown(h, 300);
    xSemaphoreGive(s_lock);
}

t5_err_t t5_py_start(const t5_py_cfg_t *cfg, t5_py_t **out)
{
    if (!cfg || !out || (!cfg->path && !cfg->code && !cfg->repl)) return T5_ERR_INVALID_ARG;
    xSemaphoreTake(py_mutex_once(&s_lock), portMAX_DELAY);
    if (!s_events) s_events = xEventGroupCreate();
    if (py_vm || !s_events) {
        xSemaphoreGive(s_lock);
        return py_vm ? T5_ERR_BUSY : T5_ERR_NO_MEM;
    }
    xEventGroupClearBits(s_events, VM_ENDED);
    t5_py_t *vm = calloc(1, sizeof *vm);
    t5_err_t e = vm ? T5_OK : T5_ERR_NO_MEM;
    if (e == T5_OK) {
        vm->cfg = *cfg;
        vm->app = t5_app_self();
        vm->path = cfg->path ? strdup(cfg->path) : NULL;
        vm->code = cfg->code ? strdup(cfg->code) : NULL;
        vm->cfg.path = vm->path;
        vm->cfg.code = vm->code;
        if ((cfg->path && !vm->path) || (cfg->code && !vm->code)) e = T5_ERR_NO_MEM;
    }
    if (e == T5_OK) {
        size_t hb = cfg->heap_bytes ? cfg->heap_bytes : T5_PY_HEAP_DEFAULT;
        if (hb < 64 * 1024) hb = 64 * 1024;
        if (hb > T5_PY_HEAP_MAX) hb = T5_PY_HEAP_MAX;
        vm->heap_size = hb;
        vm->heap = heap_caps_malloc(hb, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        vm->in = xStreamBufferCreate(REPL_IN_BYTES, 1);
        if (!vm->heap || !vm->in) e = T5_ERR_NO_MEM;
    }
    if (e == T5_OK) {
        if (cfg->cwd) snprintf(vm->cwd, sizeof vm->cwd, "%s", cfg->cwd);
        else if (vm->app) snprintf(vm->cwd, sizeof vm->cwd, "%s", t5_app_data_dir(vm->app));
        else snprintf(vm->cwd, sizeof vm->cwd, "%s", T5_USER_DIR);
        vm->state = T5_PY_RUNNING;
        py_vm = vm;
        /* Created without the app context so the tracker doesn't own the
         * task; vm_task sets the context itself. */
        void *saved = t5_app_ctx_get();
        t5_app_ctx_set(NULL);
        BaseType_t ok = xTaskCreatePinnedToCore(vm_task, "t5_py", VM_STACK_BYTES, vm, VM_PRIO, &vm->task, 0);
        t5_app_ctx_set(saved);
        if (ok != pdPASS) {
            py_vm = NULL;
            e = T5_ERR_NO_MEM;
        }
    }
    if (e != T5_OK) {
        if (vm) vm_free(vm);
        xSemaphoreGive(s_lock);
        return e;
    }
    t5_res_add(T5_RES_OTHER, vm, tracked_release);
    xSemaphoreGive(s_lock);
    T5_LOGI(TAG, "VM up for %s: %u KB heap, %s", vm->app ? t5_app_id(vm->app) : "system",
            (unsigned)(vm->heap_size / 1024), vm->path ? vm->path : (vm->code ? "<code>" : "REPL"));
    *out = vm;
    return T5_OK;
}

t5_err_t t5_py_input(t5_py_t *vm, const char *s, size_t n)
{
    if (!vm || vm != py_vm || vm->finished) return T5_ERR_STATE;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == 0x03) {             /* Ctrl-C */
            mp_sched_keyboard_interrupt();
            py_wake();
            continue;
        }
        if (xStreamBufferSend(vm->in, &s[i], 1, pdMS_TO_TICKS(100)) != 1) return T5_ERR_TIMEOUT;
    }
    return T5_OK;
}

void t5_py_interrupt(t5_py_t *vm)
{
    if (!vm || vm != py_vm || vm->finished) return;
    mp_sched_keyboard_interrupt();
    py_wake();
}

t5_err_t t5_py_stop(t5_py_t *vm, uint32_t timeout_ms)
{
    if (!vm) return T5_ERR_INVALID_ARG;
    xSemaphoreTake(py_mutex_once(&s_lock), portMAX_DELAY);
    if (vm != py_vm) {
        xSemaphoreGive(s_lock);
        return T5_ERR_STATE;
    }
    t5_res_remove_app(vm->app, vm);
    teardown(vm, timeout_ms);
    xSemaphoreGive(s_lock);
    return T5_OK;
}

bool t5_py_wait(t5_py_t *vm, uint32_t timeout_ms)
{
    if (!vm || vm->finished) return vm != NULL;
    TickType_t t = timeout_ms == T5_PY_FOREVER ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    xEventGroupWaitBits(s_events, VM_ENDED, pdFALSE, pdTRUE, t);
    return vm->finished;
}

bool t5_py_busy(void)
{
    return py_vm != NULL;
}

typedef struct {
    char *buf;
    size_t n, len;
} capture_t;

static void cap_out(const char *s, size_t n, void *arg)
{
    capture_t *c = arg;
    if (!c->buf || !c->n) return;
    size_t room = c->n - 1 - c->len;
    if (n > room) n = room;
    memcpy(c->buf + c->len, s, n);
    c->len += n;
    c->buf[c->len] = 0;
}

t5_py_state_t t5_py_exec_capture(const char *code, char *out, size_t n, uint32_t timeout_ms)
{
    capture_t c = { out, n, 0 };
    if (out && n) out[0] = 0;
    t5_py_cfg_t cfg = { .code = code, .out = cap_out, .arg = &c, .heap_bytes = 512 * 1024 };
    t5_py_t *vm;
    if (t5_py_start(&cfg, &vm) != T5_OK) {
        if (out && n) snprintf(out, n, "VM busy or out of memory\n");
        return T5_PY_STOPPED;
    }
    t5_py_state_t st = t5_py_wait(vm, timeout_ms) ? vm->state : T5_PY_STOPPED;
    t5_py_stop(vm, 500);
    return st;
}
