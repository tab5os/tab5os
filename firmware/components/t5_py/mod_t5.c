/* SPDX-License-Identifier: Apache-2.0
 * The `t5` module (plan 8.3): the root, and the small submodules
 * t5.app, t5.power, t5.time, t5.audio, t5.imu, t5.usb.
 *
 * The scalar functions come from bind_gen.h (tools/py/genbind.py, from the
 * published ABI); this file adds the ones that need handles, structs or
 * callbacks. Permissions are checked here exactly as the native ABI binds
 * them (perm= in api_v1.sym).
 */
#include <string.h>
#include "esp_app_desc.h"
#include "py/mphal.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "t5_app/app.h"
#include "t5_audio/audio.h"
#include "t5_battery/battery.h"
#include "t5_imu/t5_imu.h"
#include "t5_pkg/manifest.h"
#include "t5_power/power.h"
#include "t5_shell/shell.h"
#include "t5_time/t5_time.h"
#include "t5_ui/t5_ui.h"
#include "t5_usb/usb.h"
#include "bind_gen.h"
#include "py_priv.h"

/* Callables the OS calls back on the UI task (lifecycle, permission
 * answers), held by tag (py_tag). */
#define CB_SLOTS 16
MP_REGISTER_ROOT_POINTER(mp_obj_t t5_py_cb[16]);
MP_REGISTER_ROOT_POINTER(mp_obj_t t5_py_on_suspend);
MP_REGISTER_ROOT_POINTER(mp_obj_t t5_py_on_resume);
MP_REGISTER_ROOT_POINTER(mp_obj_t t5_py_on_rotate);

void py_modules_reset(void)
{
    for (int i = 0; i < CB_SLOTS; i++) MP_STATE_VM(t5_py_cb)[i] = mp_const_none;
    MP_STATE_VM(t5_py_on_suspend) = mp_const_none;
    MP_STATE_VM(t5_py_on_resume) = mp_const_none;
    MP_STATE_VM(t5_py_on_rotate) = mp_const_none;
    py_ui_reset();
    py_net_reset();
}

uintptr_t py_slot_put(mp_obj_t fn)
{
    for (int i = 0; i < CB_SLOTS; i++) {
        if (MP_STATE_VM(t5_py_cb)[i] == mp_const_none) {
            MP_STATE_VM(t5_py_cb)[i] = fn;
            return py_tag((uint32_t)i);
        }
    }
    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("too many pending callbacks"));
}

mp_obj_t py_slot_take(uintptr_t h)
{
    int i = py_untag(h);
    if (i < 0 || i >= CB_SLOTS) return MP_OBJ_NULL;
    mp_obj_t fn = MP_STATE_VM(t5_py_cb)[i];
    MP_STATE_VM(t5_py_cb)[i] = mp_const_none;
    return fn == mp_const_none ? MP_OBJ_NULL : fn;
}

static mp_obj_t t5_sleep_ms(mp_obj_t ms)
{
    mp_int_t v = mp_obj_get_int(ms);
    if (v > 0) py_sleep_ms((uint32_t)v);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(t5_sleep_ms_obj, t5_sleep_ms);

static mp_obj_t t5_ticks_ms(void)
{
    return mp_obj_new_int_from_uint(mp_hal_ticks_ms() & 0x3fffffff);
}
static MP_DEFINE_CONST_FUN_OBJ_0(t5_ticks_ms_obj, t5_ticks_ms);

static mp_obj_t t5_os_version(void)
{
    return mp_obj_new_str_from_cstr(esp_app_get_description()->version);
}
static MP_DEFINE_CONST_FUN_OBJ_0(t5_os_version_obj, t5_os_version);

static void *need_app(void)
{
    if (!py_vm || !py_vm->app) mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("only inside an app"));
    return py_vm->app;
}

static mp_obj_t app_running(void)
{
    /* False once the app is exiting or the person pressed back. */
    return mp_obj_new_bool(py_vm && !py_vm->stop && !py_vm->back && (!py_vm->app || t5_app_running(py_vm->app)));
}
static MP_DEFINE_CONST_FUN_OBJ_0(app_running_obj, app_running);

static mp_obj_t app_state_save(mp_obj_t data)
{
    void *app = need_app();
    mp_buffer_info_t b;
    mp_get_buffer_raise(data, &b, MP_BUFFER_READ);
    py_check(t5_app_state_save(app, b.buf, b.len));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(app_state_save_obj, app_state_save);

static mp_obj_t app_state_load(void)
{
    void *app = need_app();
    vstr_t v;
    vstr_init_len(&v, T5_APP_STATE_MAX);
    size_t len = 0;
    t5_err_t e = t5_app_state_load(app, v.buf, T5_APP_STATE_MAX, &len);
    if (e == T5_ERR_NOT_FOUND) {
        vstr_clear(&v);
        return mp_const_none;
    }
    if (e != T5_OK) {
        vstr_clear(&v);
        py_raise_t5(e);
    }
    v.len = len;
    return mp_obj_new_bytes_from_vstr(&v);
}
static MP_DEFINE_CONST_FUN_OBJ_0(app_state_load_obj, app_state_load);

static uint32_t perm_arg(mp_obj_t name)
{
    uint32_t p = t5_perm_from_name(mp_obj_str_get_str(name));
    if (!p) mp_raise_ValueError(MP_ERROR_TEXT("unknown permission"));
    return p;
}

static mp_obj_t app_has_permission(mp_obj_t name)
{
    uint32_t p = perm_arg(name);
    return mp_obj_new_bool(!py_vm->app || t5_app_has_permission(py_vm->app, p));
}
static MP_DEFINE_CONST_FUN_OBJ_1(app_has_permission_obj, app_has_permission);

static void perm_answer(t5_app_t *app, uint32_t perm, bool granted, void *user)
{
    (void)app;
    (void)perm;
    mp_obj_t fn = py_slot_take((uintptr_t)user);
    if (fn != MP_OBJ_NULL) py_schedule(fn, mp_obj_new_bool(granted));
}

static mp_obj_t app_request_permission(mp_obj_t name, mp_obj_t fn)
{
    void *app = need_app();
    uint32_t p = perm_arg(name);
    if (t5_app_has_permission(app, p)) {
        py_schedule(fn, mp_const_true);
        return mp_const_none;
    }
    uintptr_t h = py_slot_put(fn);
    t5_err_t e = t5_app_request_permission(app, p, perm_answer, (void *)h);
    if (e != T5_OK) {
        py_slot_take(h);
        py_raise_t5(e);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(app_request_permission_obj, app_request_permission);

static mp_obj_t app_on_suspend(mp_obj_t fn)
{
    MP_STATE_VM(t5_py_on_suspend) = fn;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(app_on_suspend_obj, app_on_suspend);

static mp_obj_t app_on_resume(mp_obj_t fn)
{
    MP_STATE_VM(t5_py_on_resume) = fn;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(app_on_resume_obj, app_on_resume);

static mp_obj_t app_on_rotate(mp_obj_t fn)
{
    MP_STATE_VM(t5_py_on_rotate) = fn;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(app_on_rotate_obj, app_on_rotate);

/* From the app runtime, on the UI task. */
void py_app_lifecycle(int what)
{
    if (!py_vm || py_vm->finished) return;
    mp_obj_t fn = what ? MP_STATE_VM(t5_py_on_resume) : MP_STATE_VM(t5_py_on_suspend);
    if (fn != mp_const_none) py_schedule(fn, mp_const_none);
}

void py_app_rotate(int degrees)
{
    if (!py_vm || py_vm->finished) return;
    mp_obj_t fn = MP_STATE_VM(t5_py_on_rotate);
    if (fn != mp_const_none) py_schedule(fn, MP_OBJ_NEW_SMALL_INT(degrees));
}

static const mp_rom_map_elem_t app_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_app) },
    T5_GEN_APP_GLOBALS
    { MP_ROM_QSTR(MP_QSTR_running), MP_ROM_PTR(&app_running_obj) },
    { MP_ROM_QSTR(MP_QSTR_state_save), MP_ROM_PTR(&app_state_save_obj) },
    { MP_ROM_QSTR(MP_QSTR_state_load), MP_ROM_PTR(&app_state_load_obj) },
    { MP_ROM_QSTR(MP_QSTR_has_permission), MP_ROM_PTR(&app_has_permission_obj) },
    { MP_ROM_QSTR(MP_QSTR_request_permission), MP_ROM_PTR(&app_request_permission_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_suspend), MP_ROM_PTR(&app_on_suspend_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_resume), MP_ROM_PTR(&app_on_resume_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_rotate), MP_ROM_PTR(&app_on_rotate_obj) },
};
static MP_DEFINE_CONST_DICT(app_globals, app_globals_table);
static const mp_obj_module_t mod_app = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&app_globals };

static mp_obj_t power_battery(void)
{
    t5_battery_status_t b;
    t5_battery_status(&b);
    mp_obj_t d = mp_obj_new_dict(8);
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_percent), MP_OBJ_NEW_SMALL_INT(b.percent));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_mv), MP_OBJ_NEW_SMALL_INT(b.mv));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_ma), mp_obj_new_float((mp_float_t)b.ma10 / 10));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_mw), mp_obj_new_int(b.mw));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_minutes_left), mp_obj_new_int(b.minutes_left));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_usb), mp_obj_new_bool(b.usb));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_charge), t5g_enum_out_t5_charge_state_t(b.charge));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_cycles), MP_OBJ_NEW_SMALL_INT(b.cycles));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(power_battery_obj, power_battery);

/* A wake lock as an object: t5.power.lock("cpu"), with/release(). */
typedef struct {
    mp_obj_base_t base;
    t5_wakelock_t *lock;
} py_lock_t;

static mp_obj_t lock_release(mp_obj_t self_in)
{
    py_lock_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->lock) t5_wakelock_release(self->lock);
    self->lock = NULL;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(lock_release_obj, lock_release);

static mp_obj_t lock_exit(size_t n, const mp_obj_t *args)
{
    (void)n;
    return lock_release(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(lock_exit_obj, 4, 4, lock_exit);

static const mp_rom_map_elem_t lock_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_release), MP_ROM_PTR(&lock_release_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&lock_release_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&lock_exit_obj) },
};
static MP_DEFINE_CONST_DICT(lock_locals, lock_locals_table);
static MP_DEFINE_CONST_OBJ_TYPE(py_type_lock, MP_QSTR_WakeLock, MP_TYPE_FLAG_NONE, locals_dict, &lock_locals);

static mp_obj_t power_lock(mp_obj_t kind)
{
    const char *k = mp_obj_str_get_str(kind);
    int t = -1;
    for (int i = 0; i < T5_LOCK_TYPE_COUNT; i++) {
        if (!strcmp(k, t5_lock_type_name((t5_lock_type_t)i))) t = i;
    }
    if (t < 0) mp_raise_ValueError(MP_ERROR_TEXT("lock is one of screen, cpu, audio, net, rail"));
    py_lock_t *o = mp_obj_malloc_with_finaliser(py_lock_t, &py_type_lock);
    o->lock = NULL;
    const char *owner = py_vm && py_vm->app ? t5_app_id(py_vm->app) : "python";
    o->lock = t5_wakelock_acquire((t5_lock_type_t)t, owner);
    if (!o->lock) py_raise_t5(T5_ERR_NO_MEM);
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_1(power_lock_obj, power_lock);

static const mp_rom_map_elem_t power_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_power) },
    T5_GEN_POWER_GLOBALS
    { MP_ROM_QSTR(MP_QSTR_battery), MP_ROM_PTR(&power_battery_obj) },
    { MP_ROM_QSTR(MP_QSTR_lock), MP_ROM_PTR(&power_lock_obj) },
};
static MP_DEFINE_CONST_DICT(power_globals, power_globals_table);
static const mp_obj_module_t mod_power = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&power_globals };

static mp_obj_t time_local(void)
{
    struct tm tm;
    if (!t5_time_local(&tm)) return mp_const_none;
    mp_obj_t t[8] = {
        MP_OBJ_NEW_SMALL_INT(tm.tm_year + 1900), MP_OBJ_NEW_SMALL_INT(tm.tm_mon + 1), MP_OBJ_NEW_SMALL_INT(tm.tm_mday),
        MP_OBJ_NEW_SMALL_INT(tm.tm_hour),        MP_OBJ_NEW_SMALL_INT(tm.tm_min),     MP_OBJ_NEW_SMALL_INT(tm.tm_sec),
        MP_OBJ_NEW_SMALL_INT(tm.tm_wday),        MP_OBJ_NEW_SMALL_INT(tm.tm_yday + 1),
    };
    return mp_obj_new_tuple(8, t);
}
static MP_DEFINE_CONST_FUN_OBJ_0(time_local_obj, time_local);

static const mp_rom_map_elem_t time_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_time) },
    T5_GEN_TIME_GLOBALS
    { MP_ROM_QSTR(MP_QSTR_local), MP_ROM_PTR(&time_local_obj) },
    { MP_ROM_QSTR(MP_QSTR_ticks_ms), MP_ROM_PTR(&t5_ticks_ms_obj) },
    { MP_ROM_QSTR(MP_QSTR_sleep_ms), MP_ROM_PTR(&t5_sleep_ms_obj) },
};
static MP_DEFINE_CONST_DICT(time_globals, time_globals_table);
static const mp_obj_module_t mod_time = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&time_globals };

typedef struct {
    mp_obj_base_t base;
    t5_audio_out_t *out;
} py_audio_out_t;

static mp_obj_t aout_write(mp_obj_t self_in, mp_obj_t data)
{
    py_audio_out_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->out) mp_raise_ValueError(MP_ERROR_TEXT("closed"));
    mp_buffer_info_t b;
    mp_get_buffer_raise(data, &b, MP_BUFFER_READ);
    size_t done = 0;
    while (done < b.len) {
        int n = t5_audio_out_write(self->out, (const uint8_t *)b.buf + done, b.len - done, 50);
        if (n < 0 && n != T5_ERR_TIMEOUT) py_raise_t5((t5_err_t)n);
        if (n > 0) done += (size_t)n;
        mp_event_handle_nowait();          /* stays interruptible while the queue is full */
    }
    return MP_OBJ_NEW_SMALL_INT(done);
}
static MP_DEFINE_CONST_FUN_OBJ_2(aout_write_obj, aout_write);

static mp_obj_t aout_close(mp_obj_t self_in)
{
    py_audio_out_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->out) t5_audio_out_close(self->out);
    self->out = NULL;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(aout_close_obj, aout_close);

static mp_obj_t aout_exit(size_t n, const mp_obj_t *args)
{
    (void)n;
    return aout_close(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(aout_exit_obj, 4, 4, aout_exit);

static const mp_rom_map_elem_t aout_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&aout_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&aout_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&aout_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&aout_exit_obj) },
};
static MP_DEFINE_CONST_DICT(aout_locals, aout_locals_table);
static MP_DEFINE_CONST_OBJ_TYPE(py_type_audio_out, MP_QSTR_AudioOut, MP_TYPE_FLAG_NONE, locals_dict, &aout_locals);

static mp_obj_t audio_out(size_t n_args, const mp_obj_t *args)
{
    t5_audio_out_cfg_t cfg = { .rate = 16000, .channels = 1 };
    if (n_args > 0) cfg.rate = (uint32_t)mp_obj_get_int(args[0]);
    if (n_args > 1) cfg.channels = (uint8_t)mp_obj_get_int(args[1]);
    py_audio_out_t *o = mp_obj_malloc_with_finaliser(py_audio_out_t, &py_type_audio_out);
    o->out = NULL;
    py_check(t5_audio_out_open(&cfg, &o->out));
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audio_out_obj, 0, 2, audio_out);

static const mp_rom_map_elem_t audio_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audio) },
    T5_GEN_AUDIO_GLOBALS
    { MP_ROM_QSTR(MP_QSTR_out), MP_ROM_PTR(&audio_out_obj) },
};
static MP_DEFINE_CONST_DICT(audio_globals, audio_globals_table);
static const mp_obj_module_t mod_audio = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&audio_globals };

static mp_obj_t imu_accel(void)
{
    t5_imu_accel_t a;
    py_check(t5_imu_read_accel(&a));
    mp_obj_t t[3] = { MP_OBJ_NEW_SMALL_INT(a.x), MP_OBJ_NEW_SMALL_INT(a.y), MP_OBJ_NEW_SMALL_INT(a.z) };
    return mp_obj_new_tuple(3, t);
}
static MP_DEFINE_CONST_FUN_OBJ_0(imu_accel_obj, imu_accel);

static const mp_rom_map_elem_t imu_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_imu) },
    T5_GEN_IMU_GLOBALS
    { MP_ROM_QSTR(MP_QSTR_accel), MP_ROM_PTR(&imu_accel_obj) },
};
static MP_DEFINE_CONST_DICT(imu_globals, imu_globals_table);
static const mp_obj_module_t mod_imu = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&imu_globals };

static const mp_rom_map_elem_t usb_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_usb) },
    T5_GEN_USB_GLOBALS
};
static MP_DEFINE_CONST_DICT(usb_globals, usb_globals_table);
static const mp_obj_module_t mod_usb = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&usb_globals };

extern const mp_obj_module_t py_mod_port, py_mod_ui, py_mod_net;

static const mp_rom_map_elem_t t5_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_t5) },
    { MP_ROM_QSTR(MP_QSTR_api), MP_ROM_INT(T5_API_LEVEL) },
    { MP_ROM_QSTR(MP_QSTR_version), MP_ROM_PTR(&t5_os_version_obj) },
    { MP_ROM_QSTR(MP_QSTR_sleep_ms), MP_ROM_PTR(&t5_sleep_ms_obj) },
    { MP_ROM_QSTR(MP_QSTR_ticks_ms), MP_ROM_PTR(&t5_ticks_ms_obj) },
    { MP_ROM_QSTR(MP_QSTR_app), MP_ROM_PTR(&mod_app) },
    { MP_ROM_QSTR(MP_QSTR_power), MP_ROM_PTR(&mod_power) },
    { MP_ROM_QSTR(MP_QSTR_time), MP_ROM_PTR(&mod_time) },
    { MP_ROM_QSTR(MP_QSTR_audio), MP_ROM_PTR(&mod_audio) },
    { MP_ROM_QSTR(MP_QSTR_imu), MP_ROM_PTR(&mod_imu) },
    { MP_ROM_QSTR(MP_QSTR_usb), MP_ROM_PTR(&mod_usb) },
    { MP_ROM_QSTR(MP_QSTR_port), MP_ROM_PTR(&py_mod_port) },
    { MP_ROM_QSTR(MP_QSTR_ui), MP_ROM_PTR(&py_mod_ui) },
    { MP_ROM_QSTR(MP_QSTR_net), MP_ROM_PTR(&py_mod_net) },
};
static MP_DEFINE_CONST_DICT(t5_globals, t5_globals_table);

const mp_obj_module_t py_mod_t5 = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&t5_globals };
MP_REGISTER_MODULE(MP_QSTR_t5, py_mod_t5);

const char t5_py_help_text[] =
    "Tab5 OS MicroPython. Hardware is reached through the t5 module:\n"
    "  t5.port   grove, mbus.*, ext, rs485, stamp: I2C, UART, SPI, GPIO, PWM, ADC\n"
    "  t5.ui     screen(), labels, buttons, switches, sliders, charts, dialogs\n"
    "  t5.net    get(), post(), request(), websocket(), tcp(), udp(), listen(), poll()\n"
    "  t5.power  battery(), state(), lock()\n"
    "  t5.time   now(), local(), valid()\n"
    "  t5.audio  tone(), play(), out()\n"
    "  t5.app    running(), exit(), data_dir(), state_save(), on_suspend()\n"
    "  t5.imu    accel()\n"
    "Files: open() works in the app's own folders, and in shared folders\n"
    "with shared_read, shared_write or all_files. help('modules') lists\n"
    "every module.\n";
