/* SPDX-License-Identifier: Apache-2.0
 * Device emulation and raw radio access for Python apps (API level 2,
 * docs/io-access-design.md): the same calls native apps make, with the
 * same permissions and first-use questions.
 *
 *   t5.hid        descriptors, report packing, text to key strokes
 *   t5.usbdev     open(): the Tab5 as a USB HID/MIDI device on USB-C
 *   t5.ble        hid(): the Tab5 as a Bluetooth keyboard/mouse/gamepad
 *   t5.wifi       monitor(): Wi-Fi monitor mode, raw frames, pcap files
 *   t5.usb        open(addr): raw access to a device on USB-A
 *   t5.exclusive  ("usba" | "wifi" | "bt"): whole port or radio
 *
 * Waits are cut into short slices so Ctrl-C and Stop still get through.
 * Exclusive USB-C (a raw device) is native-only for now.
 */
#include <stdio.h>
#include <string.h>
#include "py/mperrno.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "t5_core/excl.h"
#include "t5_core/os.h"
#include "t5_hid/hid.h"
#include "t5_net/ble.h"
#include "t5_net/wifimon.h"
#include "t5_usb/usb.h"
#include "t5_usb/usbdev.h"
#include "bind_perm.h"
#include "py_priv.h"

#define SLICE_MS 100

/* Call fn in slices; returns its result, or T5_ERR_TIMEOUT. */
typedef int (*slice_fn_t)(void *ctx, uint32_t ms);
static int wait_slices(slice_fn_t fn, void *ctx, mp_int_t timeout_ms)
{
    uint32_t left = timeout_ms < 0 ? UINT32_MAX : (uint32_t)timeout_ms;
    for (;;) {
        uint32_t ms = left < SLICE_MS ? left : SLICE_MS;
        int r = fn(ctx, ms);
        if (r != T5_ERR_TIMEOUT) return r;
        if (left != UINT32_MAX) {
            left -= ms;
            if (!left) return T5_ERR_TIMEOUT;
        }
        py_sleep_ms(0);                    /* callbacks; raises on Ctrl-C or Stop */
    }
}

static mp_obj_t bytes_or_none(const uint8_t *b, int n)
{
    return n > 0 ? mp_obj_new_bytes(b, (size_t)n) : mp_const_none;
}

/* __exit__ of the classes that close with close() */
static mp_obj_t io_exit(size_t n, const mp_obj_t *args)
{
    (void)n;
    return mp_call_function_0(mp_load_attr(args[0], MP_QSTR_close));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(io_exit_obj, 4, 4, io_exit);

static mp_obj_t tuple(size_t n, const mp_obj_t *items)
{
    return mp_obj_new_tuple(n, items);
}

static void dict_set(mp_obj_t d, qstr k, mp_obj_t v)
{
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(k), v);
}

/* ==== t5.hid ================================================================= */
static mp_obj_t hid_descriptor(mp_obj_t parts)
{
    uint8_t d[T5_HID_DESC_MAX];
    int n = t5_hid_descriptor((uint32_t)mp_obj_get_int(parts), d, sizeof d);
    if (n < 0) py_raise_t5((t5_err_t)n);
    return mp_obj_new_bytes(d, (size_t)n);
}
static MP_DEFINE_CONST_FUN_OBJ_1(hid_descriptor_obj, hid_descriptor);

static t5_hid_layout_t layout_arg(mp_obj_t o)
{
    static const char *const names[T5_HID_LAYOUT_COUNT] = { "us", "uk", "de", "fr" };
    const char *s = mp_obj_str_get_str(o);
    for (int i = 0; i < T5_HID_LAYOUT_COUNT; i++) {
        if (!strcmp(s, names[i])) return (t5_hid_layout_t)i;
    }
    mp_raise_ValueError(MP_ERROR_TEXT("layout: us, uk, de or fr"));
}

typedef struct {
    mp_obj_t list;
} strokes_t;

static int add_stroke(uint8_t key, uint8_t mods, void *arg)
{
    strokes_t *s = arg;
    mp_obj_t t[2] = { MP_OBJ_NEW_SMALL_INT(key), MP_OBJ_NEW_SMALL_INT(mods) };
    mp_obj_list_append(s->list, tuple(2, t));
    return 0;
}

/* keys(text, layout="us") -> [(key, mods), ...] */
static mp_obj_t hid_keys(size_t n, const mp_obj_t *args)
{
    size_t len;
    const char *text = mp_obj_str_get_data(args[0], &len);
    t5_hid_layout_t l = n > 1 ? layout_arg(args[1]) : T5_HID_LAYOUT_US;
    strokes_t s = { mp_obj_new_list(0, NULL) };
    int done = t5_hid_text(text, l, add_stroke, &s);
    if (done < 0) py_raise_t5((t5_err_t)done);
    if ((size_t)done < len) mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("can't type byte %d on this layout"), done);
    return s.list;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(hid_keys_obj, 1, 2, hid_keys);

/* keyboard(mods, keys) -> 8 bytes */
static mp_obj_t hid_keyboard(mp_obj_t mods, mp_obj_t keys)
{
    t5_hid_keyboard_t k = { .mods = (uint8_t)mp_obj_get_int(mods) };
    size_t n;
    mp_obj_t *items;
    mp_obj_get_array(keys, &n, &items);
    if (n > 6) mp_raise_ValueError(MP_ERROR_TEXT("6 keys at most"));
    for (size_t i = 0; i < n; i++) k.keys[i] = (uint8_t)mp_obj_get_int(items[i]);
    uint8_t out[T5_HID_KEYBOARD_LEN];
    t5_hid_pack_keyboard(&k, out);
    return mp_obj_new_bytes(out, sizeof out);
}
static MP_DEFINE_CONST_FUN_OBJ_2(hid_keyboard_obj, hid_keyboard);

/* mouse(buttons, dx, dy, wheel=0, pan=0) -> 7 bytes */
static mp_obj_t hid_mouse(size_t n, const mp_obj_t *a)
{
    t5_hid_mouse_t m = { .buttons = (uint8_t)mp_obj_get_int(a[0]), .dx = (int16_t)mp_obj_get_int(a[1]),
                         .dy = (int16_t)mp_obj_get_int(a[2]), .wheel = n > 3 ? (int8_t)mp_obj_get_int(a[3]) : 0,
                         .pan = n > 4 ? (int8_t)mp_obj_get_int(a[4]) : 0 };
    uint8_t out[T5_HID_MOUSE_LEN];
    t5_hid_pack_mouse(&m, out);
    return mp_obj_new_bytes(out, sizeof out);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(hid_mouse_obj, 3, 5, hid_mouse);

static mp_obj_t hid_consumer(mp_obj_t usage)
{
    uint8_t out[T5_HID_CONSUMER_LEN];
    t5_hid_pack_consumer((uint16_t)mp_obj_get_int(usage), out);
    return mp_obj_new_bytes(out, sizeof out);
}
static MP_DEFINE_CONST_FUN_OBJ_1(hid_consumer_obj, hid_consumer);

/* gamepad(buttons, lx, ly, rx, ry, lt=0, rt=0, hat=8) -> 9 bytes */
static mp_obj_t hid_gamepad(size_t n, const mp_obj_t *a)
{
    t5_hid_gamepad_t g = { .buttons = (uint16_t)mp_obj_get_int(a[0]), .lx = (int8_t)mp_obj_get_int(a[1]),
                           .ly = (int8_t)mp_obj_get_int(a[2]), .rx = (int8_t)mp_obj_get_int(a[3]),
                           .ry = (int8_t)mp_obj_get_int(a[4]), .lt = n > 5 ? (uint8_t)mp_obj_get_int(a[5]) : 0,
                           .rt = n > 6 ? (uint8_t)mp_obj_get_int(a[6]) : 0,
                           .hat = n > 7 ? (uint8_t)mp_obj_get_int(a[7]) : T5_HID_HAT_NONE };
    uint8_t out[T5_HID_GAMEPAD_LEN];
    t5_hid_pack_gamepad(&g, out);
    return mp_obj_new_bytes(out, sizeof out);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(hid_gamepad_obj, 5, 8, hid_gamepad);

static mp_obj_t hid_absolute(mp_obj_t buttons, mp_obj_t x, mp_obj_t y)
{
    t5_hid_absolute_t a = { .buttons = (uint8_t)mp_obj_get_int(buttons), .x = (uint16_t)mp_obj_get_int(x),
                            .y = (uint16_t)mp_obj_get_int(y) };
    uint8_t out[T5_HID_ABSOLUTE_LEN];
    t5_hid_pack_absolute(&a, out);
    return mp_obj_new_bytes(out, sizeof out);
}
static MP_DEFINE_CONST_FUN_OBJ_3(hid_absolute_obj, hid_absolute);

static const mp_rom_map_elem_t hid_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_hid) },
    { MP_ROM_QSTR(MP_QSTR_descriptor), MP_ROM_PTR(&hid_descriptor_obj) },
    { MP_ROM_QSTR(MP_QSTR_keys), MP_ROM_PTR(&hid_keys_obj) },
    { MP_ROM_QSTR(MP_QSTR_keyboard), MP_ROM_PTR(&hid_keyboard_obj) },
    { MP_ROM_QSTR(MP_QSTR_mouse), MP_ROM_PTR(&hid_mouse_obj) },
    { MP_ROM_QSTR(MP_QSTR_consumer), MP_ROM_PTR(&hid_consumer_obj) },
    { MP_ROM_QSTR(MP_QSTR_gamepad), MP_ROM_PTR(&hid_gamepad_obj) },
    { MP_ROM_QSTR(MP_QSTR_absolute), MP_ROM_PTR(&hid_absolute_obj) },
    { MP_ROM_QSTR(MP_QSTR_KEYBOARD), MP_ROM_INT(T5_HID_PART_KEYBOARD) },
    { MP_ROM_QSTR(MP_QSTR_MOUSE), MP_ROM_INT(T5_HID_PART_MOUSE) },
    { MP_ROM_QSTR(MP_QSTR_CONSUMER), MP_ROM_INT(T5_HID_PART_CONSUMER) },
    { MP_ROM_QSTR(MP_QSTR_GAMEPAD), MP_ROM_INT(T5_HID_PART_GAMEPAD) },
    { MP_ROM_QSTR(MP_QSTR_ABSOLUTE), MP_ROM_INT(T5_HID_PART_ABSOLUTE) },
    { MP_ROM_QSTR(MP_QSTR_ALL), MP_ROM_INT(T5_HID_PART_ALL) },
    { MP_ROM_QSTR(MP_QSTR_NO_IDS), MP_ROM_INT(T5_HID_NO_IDS) },
};
static MP_DEFINE_CONST_DICT(hid_globals, hid_globals_table);
const mp_obj_module_t py_mod_hid = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&hid_globals };

/* ==== t5.usbdev ============================================================== */
typedef struct {
    mp_obj_base_t base;
    t5_usbdev_t *d;
} py_usbdev_t;

static t5_usbdev_t *usbdev_of(mp_obj_t o)
{
    py_usbdev_t *self = MP_OBJ_TO_PTR(o);
    if (!self->d) mp_raise_ValueError(MP_ERROR_TEXT("closed"));
    return self->d;
}

static mp_obj_t usbdev_close(mp_obj_t o)
{
    py_usbdev_t *self = MP_OBJ_TO_PTR(o);
    if (self->d) T5_IGNORE(t5_usbdev_close(self->d));
    self->d = NULL;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(usbdev_close_obj, usbdev_close);

/* send(hid, data, report_id=0, timeout=100) */
static mp_obj_t usbdev_send(size_t n, const mp_obj_t *a)
{
    t5_usbdev_t *d = usbdev_of(a[0]);
    mp_buffer_info_t b;
    mp_get_buffer_raise(a[2], &b, MP_BUFFER_READ);
    uint8_t rid = n > 3 ? (uint8_t)mp_obj_get_int(a[3]) : 0;
    uint32_t t = n > 4 ? (uint32_t)mp_obj_get_int(a[4]) : 100;
    py_check(t5_usbdev_hid_send(d, (int)mp_obj_get_int(a[1]), rid, b.buf, b.len, t));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(usbdev_send_obj, 3, 5, usbdev_send);

static mp_obj_t usbdev_midi(mp_obj_t o, mp_obj_t msg)
{
    mp_buffer_info_t b;
    mp_get_buffer_raise(msg, &b, MP_BUFFER_READ);
    py_check(t5_usbdev_midi_send(usbdev_of(o), b.buf, b.len));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(usbdev_midi_obj, usbdev_midi);

static mp_obj_t usbdev_mounted(mp_obj_t o)
{
    return mp_obj_new_bool(t5_usbdev_mounted(usbdev_of(o)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(usbdev_mounted_obj, usbdev_mounted);

typedef struct {
    t5_usbdev_t *d;
    t5_usbdev_ev_t ev;
    uint8_t buf[64];
} usbdev_wait_t;

static int usbdev_slice(void *ctx, uint32_t ms)
{
    usbdev_wait_t *w = ctx;
    return t5_usbdev_next(w->d, &w->ev, w->buf, sizeof w->buf, ms);
}

/* event(timeout=0) -> None | ("mounted",) | ("unmounted",) | ("suspended", wake)
 *   | ("resumed",) | ("output", hid, report_id, bytes) | ("protocol", hid, boot)
 *   | ("midi", bytes) */
static mp_obj_t usbdev_event(size_t n, const mp_obj_t *a)
{
    usbdev_wait_t w = { .d = usbdev_of(a[0]) };
    int r = wait_slices(usbdev_slice, &w, n > 1 ? mp_obj_get_int(a[1]) : 0);
    if (r == T5_ERR_TIMEOUT) return mp_const_none;
    if (r < 0) py_raise_t5((t5_err_t)r);
    mp_obj_t t[4];
    switch (w.ev.type) {
    case T5_USBDEV_MOUNTED: t[0] = MP_OBJ_NEW_QSTR(MP_QSTR_mounted); return tuple(1, t);
    case T5_USBDEV_UNMOUNTED: t[0] = MP_OBJ_NEW_QSTR(MP_QSTR_unmounted); return tuple(1, t);
    case T5_USBDEV_SUSPENDED:
        t[0] = MP_OBJ_NEW_QSTR(MP_QSTR_suspended);
        t[1] = mp_obj_new_bool(w.ev.remote_wakeup);
        return tuple(2, t);
    case T5_USBDEV_RESUMED: t[0] = MP_OBJ_NEW_QSTR(MP_QSTR_resumed); return tuple(1, t);
    case T5_USBDEV_HID_OUTPUT:
        t[0] = MP_OBJ_NEW_QSTR(MP_QSTR_output);
        t[1] = MP_OBJ_NEW_SMALL_INT(w.ev.hid);
        t[2] = MP_OBJ_NEW_SMALL_INT(w.ev.report_id);
        t[3] = mp_obj_new_bytes(w.buf, (size_t)r);
        return tuple(4, t);
    case T5_USBDEV_HID_PROTOCOL:
        t[0] = MP_OBJ_NEW_QSTR(MP_QSTR_protocol);
        t[1] = MP_OBJ_NEW_SMALL_INT(w.ev.hid);
        t[2] = mp_obj_new_bool(w.ev.boot_protocol);
        return tuple(3, t);
    case T5_USBDEV_MIDI_IN:
        t[0] = MP_OBJ_NEW_QSTR(MP_QSTR_midi);
        t[1] = mp_obj_new_bytes(w.buf, (size_t)r);
        return tuple(2, t);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(usbdev_event_obj, 1, 2, usbdev_event);

static const mp_rom_map_elem_t usbdev_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&usbdev_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_midi), MP_ROM_PTR(&usbdev_midi_obj) },
    { MP_ROM_QSTR(MP_QSTR_mounted), MP_ROM_PTR(&usbdev_mounted_obj) },
    { MP_ROM_QSTR(MP_QSTR_event), MP_ROM_PTR(&usbdev_event_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&usbdev_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&usbdev_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&io_exit_obj) },
};
static MP_DEFINE_CONST_DICT(usbdev_locals, usbdev_locals_table);
static MP_DEFINE_CONST_OBJ_TYPE(py_type_usbdev, MP_QSTR_UsbDevice, MP_TYPE_FLAG_NONE, locals_dict, &usbdev_locals);

/* open(hid=[descriptor, ...], midi=False, product=None, boot=None) */
static mp_obj_t usbdev_open(size_t n, const mp_obj_t *pos, mp_map_t *kw)
{
    enum { A_HID, A_MIDI, A_PRODUCT, A_BOOT };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_hid, MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_midi, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_product, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_boot, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
    };
    mp_arg_val_t v[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n, pos, kw, MP_ARRAY_SIZE(allowed), allowed, v);
    py_need_perm(T5G_PERM_t5_usbdev_open);
    t5_usbdev_cfg_t cfg = { .size = sizeof cfg, .midi = v[A_MIDI].u_bool };
    if (v[A_PRODUCT].u_obj != mp_const_none) cfg.product = mp_obj_str_get_str(v[A_PRODUCT].u_obj);
    if (v[A_HID].u_obj != mp_const_none) {
        size_t k;
        mp_obj_t *items;
        mp_obj_get_array(v[A_HID].u_obj, &k, &items);
        if (k > T5_USBDEV_MAX_HID) mp_raise_ValueError(MP_ERROR_TEXT("3 HID interfaces at most"));
        cfg.hid_count = (uint8_t)k;
        for (size_t i = 0; i < k; i++) {
            mp_buffer_info_t b;
            mp_get_buffer_raise(items[i], &b, MP_BUFFER_READ);
            cfg.hid[i].report_desc = b.buf;
            cfg.hid[i].desc_len = (uint16_t)b.len;
        }
    }
    if (v[A_BOOT].u_obj != mp_const_none && cfg.hid_count) {
        const char *b = mp_obj_str_get_str(v[A_BOOT].u_obj);
        cfg.hid[0].boot = !strcmp(b, "keyboard") ? T5_USBDEV_BOOT_KEYBOARD : !strcmp(b, "mouse") ? T5_USBDEV_BOOT_MOUSE : 0;
    }
    py_usbdev_t *o = mp_obj_malloc_with_finaliser(py_usbdev_t, &py_type_usbdev);
    o->d = NULL;
    py_check(t5_usbdev_open(&cfg, &o->d));
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(usbdev_open_obj, 0, usbdev_open);

static const mp_rom_map_elem_t usbdev_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_usbdev) },
    { MP_ROM_QSTR(MP_QSTR_open), MP_ROM_PTR(&usbdev_open_obj) },
};
static MP_DEFINE_CONST_DICT(usbdev_globals, usbdev_globals_table);
const mp_obj_module_t py_mod_usbdev = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&usbdev_globals };

/* ==== t5.ble.hid ============================================================= */
typedef struct {
    mp_obj_base_t base;
    t5_ble_hid_t *h;
} py_blehid_t;

static t5_ble_hid_t *blehid_of(mp_obj_t o)
{
    py_blehid_t *self = MP_OBJ_TO_PTR(o);
    if (!self->h) mp_raise_ValueError(MP_ERROR_TEXT("closed"));
    return self->h;
}

static mp_obj_t blehid_close(mp_obj_t o)
{
    py_blehid_t *self = MP_OBJ_TO_PTR(o);
    if (self->h) T5_IGNORE(t5_ble_hid_close(self->h));
    self->h = NULL;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(blehid_close_obj, blehid_close);

/* send(part, report, timeout=100) */
static mp_obj_t blehid_send(size_t n, const mp_obj_t *a)
{
    mp_buffer_info_t b;
    mp_get_buffer_raise(a[2], &b, MP_BUFFER_READ);
    uint32_t t = n > 3 ? (uint32_t)mp_obj_get_int(a[3]) : 100;
    py_check(t5_ble_hid_send(blehid_of(a[0]), (uint32_t)mp_obj_get_int(a[1]), b.buf, b.len, t));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(blehid_send_obj, 3, 4, blehid_send);

static mp_obj_t blehid_connected(mp_obj_t o)
{
    return mp_obj_new_bool(t5_ble_hid_connected(blehid_of(o)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(blehid_connected_obj, blehid_connected);

typedef struct {
    t5_ble_hid_t *h;
    t5_ble_hid_ev_t ev;
    uint8_t buf[8];
} blehid_wait_t;

static int blehid_slice(void *ctx, uint32_t ms)
{
    blehid_wait_t *w = ctx;
    return t5_ble_hid_next(w->h, &w->ev, w->buf, sizeof w->buf, ms);
}

/* event(timeout=0) -> None | ("connected", addr) | ("disconnected", addr)
 *   | ("output", report_id, bytes) | ("suspend", on) */
static mp_obj_t blehid_event(size_t n, const mp_obj_t *a)
{
    blehid_wait_t w = { .h = blehid_of(a[0]) };
    int r = wait_slices(blehid_slice, &w, n > 1 ? mp_obj_get_int(a[1]) : 0);
    if (r == T5_ERR_TIMEOUT) return mp_const_none;
    if (r < 0) py_raise_t5((t5_err_t)r);
    char addr[18];
    t5_ble_format_addr(w.ev.addr, addr);
    mp_obj_t t[3];
    switch (w.ev.type) {
    case T5_BLE_HID_CONNECTED:
    case T5_BLE_HID_DISCONNECTED:
        t[0] = MP_OBJ_NEW_QSTR(w.ev.type == T5_BLE_HID_CONNECTED ? MP_QSTR_connected : MP_QSTR_disconnected);
        t[1] = mp_obj_new_str(addr, strlen(addr));
        return tuple(2, t);
    case T5_BLE_HID_OUTPUT:
        t[0] = MP_OBJ_NEW_QSTR(MP_QSTR_output);
        t[1] = MP_OBJ_NEW_SMALL_INT(w.ev.report_id);
        t[2] = mp_obj_new_bytes(w.buf, (size_t)r);
        return tuple(3, t);
    case T5_BLE_HID_SUSPEND:
        t[0] = MP_OBJ_NEW_QSTR(MP_QSTR_suspend);
        t[1] = mp_obj_new_bool(w.ev.on);
        return tuple(2, t);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(blehid_event_obj, 1, 2, blehid_event);

static const mp_rom_map_elem_t blehid_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&blehid_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_connected), MP_ROM_PTR(&blehid_connected_obj) },
    { MP_ROM_QSTR(MP_QSTR_event), MP_ROM_PTR(&blehid_event_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&blehid_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&blehid_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&io_exit_obj) },
};
static MP_DEFINE_CONST_DICT(blehid_locals, blehid_locals_table);
static MP_DEFINE_CONST_OBJ_TYPE(py_type_blehid, MP_QSTR_BleHid, MP_TYPE_FLAG_NONE, locals_dict, &blehid_locals);

/* hid(name=None, appearance=0) */
static mp_obj_t ble_hid(size_t n, const mp_obj_t *a)
{
    py_need_perm(T5G_PERM_t5_ble_hid_open);
    t5_ble_hid_cfg_t cfg = { .size = sizeof cfg };
    if (n > 0 && a[0] != mp_const_none) cfg.name = mp_obj_str_get_str(a[0]);
    if (n > 1) cfg.appearance = (uint16_t)mp_obj_get_int(a[1]);
    py_blehid_t *o = mp_obj_malloc_with_finaliser(py_blehid_t, &py_type_blehid);
    o->h = NULL;
    py_check(t5_ble_hid_open(&cfg, &o->h));
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ble_hid_obj, 0, 2, ble_hid);

static const mp_rom_map_elem_t ble_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_ble) },
    { MP_ROM_QSTR(MP_QSTR_hid), MP_ROM_PTR(&ble_hid_obj) },
    { MP_ROM_QSTR(MP_QSTR_KEYBOARD), MP_ROM_INT(T5_BLE_APPEARANCE_KEYBOARD) },
    { MP_ROM_QSTR(MP_QSTR_MOUSE), MP_ROM_INT(T5_BLE_APPEARANCE_MOUSE) },
    { MP_ROM_QSTR(MP_QSTR_GAMEPAD), MP_ROM_INT(T5_BLE_APPEARANCE_GAMEPAD) },
};
static MP_DEFINE_CONST_DICT(ble_globals, ble_globals_table);
const mp_obj_module_t py_mod_ble = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&ble_globals };

/* ==== t5.wifi.monitor ======================================================== */
typedef struct {
    mp_obj_base_t base;
    t5_wifimon_t *m;
} py_wifimon_t;

static t5_wifimon_t *wifimon_of(mp_obj_t o)
{
    py_wifimon_t *self = MP_OBJ_TO_PTR(o);
    if (!self->m) mp_raise_ValueError(MP_ERROR_TEXT("closed"));
    return self->m;
}

static mp_obj_t wifimon_close(mp_obj_t o)
{
    py_wifimon_t *self = MP_OBJ_TO_PTR(o);
    if (self->m) T5_IGNORE(t5_wifimon_close(self->m));
    self->m = NULL;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(wifimon_close_obj, wifimon_close);

typedef struct {
    t5_wifimon_t *m;
    t5_wifimon_frame_t f;
    uint8_t buf[1400];
} wifimon_wait_t;

static int wifimon_slice(void *ctx, uint32_t ms)
{
    wifimon_wait_t *w = ctx;
    return t5_wifimon_next(w->m, &w->f, w->buf, sizeof w->buf, ms);
}

static mp_obj_t frame_meta(const t5_wifimon_frame_t *f)
{
    mp_obj_t d = mp_obj_new_dict(8);
    dict_set(d, MP_QSTR_rssi, MP_OBJ_NEW_SMALL_INT(f->rssi));
    dict_set(d, MP_QSTR_noise, MP_OBJ_NEW_SMALL_INT(f->noise_floor));
    dict_set(d, MP_QSTR_channel, MP_OBJ_NEW_SMALL_INT(f->channel));
    dict_set(d, MP_QSTR_time_us, mp_obj_new_int_from_uint(f->time_us));
    dict_set(d, MP_QSTR_len, MP_OBJ_NEW_SMALL_INT(f->len));
    dict_set(d, MP_QSTR_type, MP_OBJ_NEW_SMALL_INT(f->type));
    dict_set(d, MP_QSTR_rate, MP_OBJ_NEW_SMALL_INT(f->rate));
    dict_set(d, MP_QSTR_fcs_bad, mp_obj_new_bool(f->fcs_bad));
    return d;
}

/* frame(timeout=100) -> None | (meta, bytes) */
static mp_obj_t wifimon_frame(size_t n, const mp_obj_t *a)
{
    wifimon_wait_t *w = m_new_obj(wifimon_wait_t);
    w->m = wifimon_of(a[0]);
    int r = wait_slices(wifimon_slice, w, n > 1 ? mp_obj_get_int(a[1]) : 100);
    if (r == T5_ERR_TIMEOUT) return mp_const_none;
    if (r < 0) py_raise_t5((t5_err_t)r);
    mp_obj_t t[2] = { frame_meta(&w->f), mp_obj_new_bytes(w->buf, (size_t)r) };
    return tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(wifimon_frame_obj, 1, 2, wifimon_frame);

static mp_obj_t wifimon_set_channel(mp_obj_t o, mp_obj_t ch)
{
    py_check(t5_wifimon_set_channel(wifimon_of(o), (uint8_t)mp_obj_get_int(ch)));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(wifimon_set_channel_obj, wifimon_set_channel);

static mp_obj_t wifimon_stats(mp_obj_t o)
{
    t5_wifimon_stats_t st;
    t5_wifimon_stats(wifimon_of(o), &st);
    mp_obj_t d = mp_obj_new_dict(7);
    dict_set(d, MP_QSTR_frames, mp_obj_new_int_from_uint(st.frames));
    dict_set(d, MP_QSTR_dropped_radio, mp_obj_new_int_from_uint(st.dropped_radio));
    dict_set(d, MP_QSTR_dropped_link, mp_obj_new_int_from_uint(st.dropped_link));
    dict_set(d, MP_QSTR_dropped_queue, mp_obj_new_int_from_uint(st.dropped_queue));
    dict_set(d, MP_QSTR_sent, mp_obj_new_int_from_uint(st.sent));
    dict_set(d, MP_QSTR_refused, mp_obj_new_int_from_uint(st.refused));
    dict_set(d, MP_QSTR_channel, MP_OBJ_NEW_SMALL_INT(st.channel));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(wifimon_stats_obj, wifimon_stats);

/* send(frame, sys_seq=True) */
static mp_obj_t wifimon_send(size_t n, const mp_obj_t *a)
{
    py_need_perm(T5G_PERM_t5_wifimon_send);
    mp_buffer_info_t b;
    mp_get_buffer_raise(a[1], &b, MP_BUFFER_READ);
    py_check(t5_wifimon_send(wifimon_of(a[0]), b.buf, b.len, n > 2 ? mp_obj_is_true(a[2]) : true));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(wifimon_send_obj, 2, 3, wifimon_send);

/* pcap(path, seconds) -> frames written: a capture Wireshark opens. */
static mp_obj_t wifimon_pcap(mp_obj_t o, mp_obj_t path_in, mp_obj_t secs)
{
    t5_wifimon_t *m = wifimon_of(o);
    char path[T5_FS_PATH_MAX];
    py_resolve_path(mp_obj_str_get_str(path_in), true, path, sizeof path);
    mp_int_t s = mp_obj_get_int(secs);
    if (s < 1 || s > 3600) mp_raise_ValueError(MP_ERROR_TEXT("1..3600 seconds"));
    FILE *f = fopen(path, "wb");
    if (!f) mp_raise_OSError(MP_EIO);
    uint8_t hdr[24];
    fwrite(hdr, 1, t5_wifimon_pcap_header(hdr), f);
    wifimon_wait_t *w = m_new_obj(wifimon_wait_t);
    w->m = m;
    static uint8_t rec[16 + 16 + 1400];
    uint32_t count = 0, until = t5_time_ms() + (uint32_t)s * 1000u;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        while ((int32_t)(until - t5_time_ms()) > 0) {
            int r = wait_slices(wifimon_slice, w, SLICE_MS);
            if (r == T5_ERR_TIMEOUT) continue;
            if (r < 0) break;
            size_t k = t5_wifimon_pcap_record(&w->f, w->buf, (size_t)r, rec, sizeof rec);
            if (k && fwrite(rec, 1, k, f) == k) count++;
        }
        nlr_pop();
    } else {
        fclose(f);                         /* Ctrl-C or Stop: keep what was written */
        nlr_jump(nlr.ret_val);
    }
    fclose(f);
    return mp_obj_new_int_from_uint(count);
}
static MP_DEFINE_CONST_FUN_OBJ_3(wifimon_pcap_obj, wifimon_pcap);

static const mp_rom_map_elem_t wifimon_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_frame), MP_ROM_PTR(&wifimon_frame_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_channel), MP_ROM_PTR(&wifimon_set_channel_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&wifimon_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&wifimon_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_pcap), MP_ROM_PTR(&wifimon_pcap_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&wifimon_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&wifimon_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&io_exit_obj) },
};
static MP_DEFINE_CONST_DICT(wifimon_locals, wifimon_locals_table);
static MP_DEFINE_CONST_OBJ_TYPE(py_type_wifimon, MP_QSTR_Monitor, MP_TYPE_FLAG_NONE, locals_dict, &wifimon_locals);

/* monitor(channel=0, types=MGMT, snaplen=256, exclusive=False, tx_per_s=0) */
static mp_obj_t wifi_monitor(size_t n, const mp_obj_t *pos, mp_map_t *kw)
{
    enum { A_CH, A_TYPES, A_SNAP, A_EXCL, A_TX };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_channel, MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_types, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = T5_WIFIMON_MGMT } },
        { MP_QSTR_snaplen, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 256 } },
        { MP_QSTR_exclusive, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_tx_per_s, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
    };
    mp_arg_val_t v[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n, pos, kw, MP_ARRAY_SIZE(allowed), allowed, v);
    py_need_perm(T5G_PERM_t5_wifimon_open);
    t5_wifimon_cfg_t cfg = { .size = sizeof cfg, .channel = (uint8_t)v[A_CH].u_int, .types = (uint8_t)v[A_TYPES].u_int,
                             .snaplen = (uint16_t)v[A_SNAP].u_int, .exclusive = v[A_EXCL].u_bool,
                             .tx_per_s = (uint16_t)v[A_TX].u_int };
    py_wifimon_t *o = mp_obj_malloc_with_finaliser(py_wifimon_t, &py_type_wifimon);
    o->m = NULL;
    py_check(t5_wifimon_open(&cfg, &o->m));
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(wifi_monitor_obj, 0, wifi_monitor);

static const mp_rom_map_elem_t wifi_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_wifi) },
    { MP_ROM_QSTR(MP_QSTR_monitor), MP_ROM_PTR(&wifi_monitor_obj) },
    { MP_ROM_QSTR(MP_QSTR_MGMT), MP_ROM_INT(T5_WIFIMON_MGMT) },
    { MP_ROM_QSTR(MP_QSTR_CTRL), MP_ROM_INT(T5_WIFIMON_CTRL) },
    { MP_ROM_QSTR(MP_QSTR_DATA), MP_ROM_INT(T5_WIFIMON_DATA) },
    { MP_ROM_QSTR(MP_QSTR_MISC), MP_ROM_INT(T5_WIFIMON_MISC) },
};
static MP_DEFINE_CONST_DICT(wifi_globals, wifi_globals_table);
const mp_obj_module_t py_mod_wifi = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&wifi_globals };

/* ==== t5.usb.open: raw USB-A ================================================= */
typedef struct {
    mp_obj_base_t base;
    t5_usb_handle_t *h;
} py_usbh_t;

static t5_usb_handle_t *usbh_of(mp_obj_t o)
{
    py_usbh_t *self = MP_OBJ_TO_PTR(o);
    if (!self->h) mp_raise_ValueError(MP_ERROR_TEXT("closed"));
    return self->h;
}

static mp_obj_t usbh_close(mp_obj_t o)
{
    py_usbh_t *self = MP_OBJ_TO_PTR(o);
    if (self->h) T5_IGNORE(t5_usb_close(self->h));
    self->h = NULL;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(usbh_close_obj, usbh_close);

static mp_obj_t desc_bytes(int (*fn)(t5_usb_handle_t *, void *, size_t), t5_usb_handle_t *h)
{
    int need = fn(h, NULL, 0);
    if (need < 0) py_raise_t5((t5_err_t)need);
    vstr_t v;
    vstr_init_len(&v, (size_t)need);
    int got = fn(h, v.buf, (size_t)need);
    if (got < 0) {
        vstr_clear(&v);
        py_raise_t5((t5_err_t)got);
    }
    return mp_obj_new_bytes_from_vstr(&v);
}

static mp_obj_t usbh_device_desc(mp_obj_t o)
{
    return desc_bytes(t5_usb_device_desc, usbh_of(o));
}
static MP_DEFINE_CONST_FUN_OBJ_1(usbh_device_desc_obj, usbh_device_desc);

static mp_obj_t usbh_config_desc(mp_obj_t o)
{
    return desc_bytes(t5_usb_config_desc, usbh_of(o));
}
static MP_DEFINE_CONST_FUN_OBJ_1(usbh_config_desc_obj, usbh_config_desc);

static mp_obj_t usbh_string(mp_obj_t o, mp_obj_t idx)
{
    char s[128];
    int n = t5_usb_string(usbh_of(o), (uint8_t)mp_obj_get_int(idx), s, sizeof s);
    if (n < 0) py_raise_t5((t5_err_t)n);
    return mp_obj_new_str(s, (size_t)n);
}
static MP_DEFINE_CONST_FUN_OBJ_2(usbh_string_obj, usbh_string);

/* claim(iface, alt=0, take_over=False) */
static mp_obj_t usbh_claim(size_t n, const mp_obj_t *a)
{
    uint8_t alt = n > 2 ? (uint8_t)mp_obj_get_int(a[2]) : 0;
    uint32_t flags = n > 3 && mp_obj_is_true(a[3]) ? T5_USB_CLAIM_TAKE_OVER : 0;
    py_check(t5_usb_claim(usbh_of(a[0]), (uint8_t)mp_obj_get_int(a[1]), alt, flags));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(usbh_claim_obj, 2, 4, usbh_claim);

static mp_obj_t usbh_release(mp_obj_t o, mp_obj_t iface)
{
    py_check(t5_usb_release(usbh_of(o), (uint8_t)mp_obj_get_int(iface)));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(usbh_release_obj, usbh_release);

/* control(bm_request_type, b_request, w_value, w_index, data_or_length=0, timeout=1000)
 * -> bytes read (IN) or the count written (OUT) */
static mp_obj_t usbh_control(size_t n, const mp_obj_t *a)
{
    t5_usb_handle_t *h = usbh_of(a[0]);
    uint8_t bm = (uint8_t)mp_obj_get_int(a[1]);
    uint32_t t = n > 6 ? (uint32_t)mp_obj_get_int(a[6]) : 1000;
    if (bm & 0x80) {
        mp_int_t len = n > 5 ? mp_obj_get_int(a[5]) : 0;
        if (len < 0 || len > T5_USB_XFER_MAX) mp_raise_ValueError(MP_ERROR_TEXT("length"));
        vstr_t v;
        vstr_init_len(&v, (size_t)len);
        int r = t5_usb_control(h, bm, (uint8_t)mp_obj_get_int(a[2]), (uint16_t)mp_obj_get_int(a[3]),
                               (uint16_t)mp_obj_get_int(a[4]), v.buf, (uint16_t)len, t);
        if (r < 0) {
            vstr_clear(&v);
            py_raise_t5((t5_err_t)r);
        }
        v.len = (size_t)r;
        return mp_obj_new_bytes_from_vstr(&v);
    }
    mp_buffer_info_t b = { .buf = NULL, .len = 0 };
    if (n > 5 && a[5] != mp_const_none && !mp_obj_is_int(a[5])) mp_get_buffer_raise(a[5], &b, MP_BUFFER_READ);
    int r = t5_usb_control(h, bm, (uint8_t)mp_obj_get_int(a[2]), (uint16_t)mp_obj_get_int(a[3]),
                           (uint16_t)mp_obj_get_int(a[4]), b.buf, (uint16_t)b.len, t);
    if (r < 0) py_raise_t5((t5_err_t)r);
    return MP_OBJ_NEW_SMALL_INT(r);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(usbh_control_obj, 5, 7, usbh_control);

/* read(ep, n, timeout=1000) -> bytes */
static mp_obj_t usbh_read(size_t n, const mp_obj_t *a)
{
    mp_int_t len = mp_obj_get_int(a[2]);
    if (len < 1 || len > T5_USB_XFER_MAX) mp_raise_ValueError(MP_ERROR_TEXT("length"));
    vstr_t v;
    vstr_init_len(&v, (size_t)len);
    int r = t5_usb_transfer(usbh_of(a[0]), (uint8_t)(mp_obj_get_int(a[1]) | 0x80), v.buf, (size_t)len,
                            n > 3 ? (uint32_t)mp_obj_get_int(a[3]) : 1000);
    if (r < 0) {
        vstr_clear(&v);
        py_raise_t5((t5_err_t)r);
    }
    v.len = (size_t)r;
    return mp_obj_new_bytes_from_vstr(&v);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(usbh_read_obj, 3, 4, usbh_read);

/* write(ep, data, timeout=1000) -> count */
static mp_obj_t usbh_write(size_t n, const mp_obj_t *a)
{
    mp_buffer_info_t b;
    mp_get_buffer_raise(a[2], &b, MP_BUFFER_READ);
    int r = t5_usb_transfer(usbh_of(a[0]), (uint8_t)(mp_obj_get_int(a[1]) & 0x7f), b.buf, b.len,
                            n > 3 ? (uint32_t)mp_obj_get_int(a[3]) : 1000);
    if (r < 0) py_raise_t5((t5_err_t)r);
    return MP_OBJ_NEW_SMALL_INT(r);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(usbh_write_obj, 3, 4, usbh_write);

/* stream(ep, depth=4): keep IN transfers queued; next(ep, timeout) reads them. */
static mp_obj_t usbh_stream(size_t n, const mp_obj_t *a)
{
    py_check(t5_usb_stream_start(usbh_of(a[0]), (uint8_t)(mp_obj_get_int(a[1]) | 0x80),
                                 n > 2 ? (uint8_t)mp_obj_get_int(a[2]) : 4));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(usbh_stream_obj, 2, 3, usbh_stream);

typedef struct {
    t5_usb_handle_t *h;
    uint8_t ep;
    uint8_t buf[512];
} usbh_wait_t;

static int usbh_slice(void *ctx, uint32_t ms)
{
    usbh_wait_t *w = ctx;
    return t5_usb_stream_next(w->h, w->ep, w->buf, sizeof w->buf, ms);
}

static mp_obj_t usbh_next(size_t n, const mp_obj_t *a)
{
    usbh_wait_t *w = m_new_obj(usbh_wait_t);
    w->h = usbh_of(a[0]);
    w->ep = (uint8_t)(mp_obj_get_int(a[1]) | 0x80);
    int r = wait_slices(usbh_slice, w, n > 2 ? mp_obj_get_int(a[2]) : 100);
    if (r == T5_ERR_TIMEOUT) return mp_const_none;
    if (r < 0) py_raise_t5((t5_err_t)r);
    return mp_obj_new_bytes(w->buf, (size_t)r);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(usbh_next_obj, 2, 3, usbh_next);

static mp_obj_t usbh_up(mp_obj_t o)
{
    return mp_obj_new_bool(t5_usb_handle_up(usbh_of(o)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(usbh_up_obj, usbh_up);

static const mp_rom_map_elem_t usbh_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_device_desc), MP_ROM_PTR(&usbh_device_desc_obj) },
    { MP_ROM_QSTR(MP_QSTR_config_desc), MP_ROM_PTR(&usbh_config_desc_obj) },
    { MP_ROM_QSTR(MP_QSTR_string), MP_ROM_PTR(&usbh_string_obj) },
    { MP_ROM_QSTR(MP_QSTR_claim), MP_ROM_PTR(&usbh_claim_obj) },
    { MP_ROM_QSTR(MP_QSTR_release), MP_ROM_PTR(&usbh_release_obj) },
    { MP_ROM_QSTR(MP_QSTR_control), MP_ROM_PTR(&usbh_control_obj) },
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&usbh_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&usbh_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_stream), MP_ROM_PTR(&usbh_stream_obj) },
    { MP_ROM_QSTR(MP_QSTR_next), MP_ROM_PTR(&usbh_next_obj) },
    { MP_ROM_QSTR(MP_QSTR_up), MP_ROM_PTR(&usbh_up_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&usbh_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&usbh_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&io_exit_obj) },
};
static MP_DEFINE_CONST_DICT(usbh_locals, usbh_locals_table);
static MP_DEFINE_CONST_OBJ_TYPE(py_type_usbh, MP_QSTR_UsbHandle, MP_TYPE_FLAG_NONE, locals_dict, &usbh_locals);

static mp_obj_t usb_open(mp_obj_t addr)
{
    py_need_perm(T5G_PERM_t5_usb_open);
    py_usbh_t *o = mp_obj_malloc_with_finaliser(py_usbh_t, &py_type_usbh);
    o->h = NULL;
    py_check(t5_usb_open((uint8_t)mp_obj_get_int(addr), &o->h));
    return MP_OBJ_FROM_PTR(o);
}
MP_DEFINE_CONST_FUN_OBJ_1(py_usb_open_obj, usb_open);

/* t5.usb.devices() -> [(addr, vid, pid, class, product), ...] */
static mp_obj_t usb_devices(void)
{
    t5_usb_dev_t d[T5_USB_MAX_DEVICES];
    int n = t5_usb_devices(d, T5_USB_MAX_DEVICES);
    mp_obj_t l = mp_obj_new_list(0, NULL);
    for (int i = 0; i < n; i++) {
        const char *cls = t5_usb_class_name((t5_usb_class_t)d[i].cls);
        mp_obj_t t[5] = { MP_OBJ_NEW_SMALL_INT(d[i].addr), MP_OBJ_NEW_SMALL_INT(d[i].vid), MP_OBJ_NEW_SMALL_INT(d[i].pid),
                          mp_obj_new_str(cls, strlen(cls)), mp_obj_new_str(d[i].product, strlen(d[i].product)) };
        mp_obj_list_append(l, tuple(5, t));
    }
    return l;
}
MP_DEFINE_CONST_FUN_OBJ_0(py_usb_devices_obj, usb_devices);

/* ==== t5.exclusive =========================================================== */
typedef struct {
    mp_obj_base_t base;
    t5_excl_t *h;
    t5_excl_kind_t kind;
} py_excl_t;

static t5_excl_t *excl_of(mp_obj_t o, t5_excl_kind_t want)
{
    py_excl_t *self = MP_OBJ_TO_PTR(o);
    if (!self->h) mp_raise_ValueError(MP_ERROR_TEXT("released"));
    if (self->kind != want) mp_raise_ValueError(MP_ERROR_TEXT("not for this kind of claim"));
    return self->h;
}

static mp_obj_t excl_release(mp_obj_t o)
{
    py_excl_t *self = MP_OBJ_TO_PTR(o);
    if (self->h) T5_IGNORE(t5_excl_release(self->h));
    self->h = NULL;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(excl_release_obj, excl_release);

static mp_obj_t excl_exit(size_t n, const mp_obj_t *args)
{
    (void)n;
    return excl_release(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(excl_exit_obj, 4, 4, excl_exit);

static mp_obj_t excl_active(mp_obj_t o)
{
    py_excl_t *self = MP_OBJ_TO_PTR(o);
    return mp_obj_new_bool(self->h && t5_excl_active(self->h));
}
static MP_DEFINE_CONST_FUN_OBJ_1(excl_active_obj, excl_active);

static mp_obj_t excl_hci_send(mp_obj_t o, mp_obj_t frame)
{
    mp_buffer_info_t b;
    mp_get_buffer_raise(frame, &b, MP_BUFFER_READ);
    py_check(t5_bt_hci_send(excl_of(o, T5_EXCL_BT), b.buf, b.len));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(excl_hci_send_obj, excl_hci_send);

typedef struct {
    t5_excl_t *h;
    uint8_t buf[1100];
} hci_wait_t;

static int hci_slice(void *ctx, uint32_t ms)
{
    hci_wait_t *w = ctx;
    return t5_bt_hci_next(w->h, w->buf, sizeof w->buf, ms);
}

static mp_obj_t excl_hci_next(size_t n, const mp_obj_t *a)
{
    hci_wait_t *w = m_new_obj(hci_wait_t);
    w->h = excl_of(a[0], T5_EXCL_BT);
    int r = wait_slices(hci_slice, w, n > 1 ? mp_obj_get_int(a[1]) : 100);
    if (r == T5_ERR_TIMEOUT) return mp_const_none;
    if (r < 0) py_raise_t5((t5_err_t)r);
    return bytes_or_none(w->buf, r);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(excl_hci_next_obj, 1, 2, excl_hci_next);

static mp_obj_t excl_usba_power(mp_obj_t o, mp_obj_t on)
{
    py_check(t5_usba_power(excl_of(o, T5_EXCL_USBA), mp_obj_is_true(on)));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(excl_usba_power_obj, excl_usba_power);

/* scan(channel=0, passive=False, hidden=False) -> [dict, ...] */
static mp_obj_t excl_scan(size_t n, const mp_obj_t *a)
{
    t5_wifi_scan_cfg_t cfg = { .size = sizeof cfg, .channel = n > 1 ? (uint8_t)mp_obj_get_int(a[1]) : 0,
                               .passive = n > 2 && mp_obj_is_true(a[2]), .show_hidden = n > 3 && mp_obj_is_true(a[3]) };
    t5_wifi_ap_t *aps = m_new(t5_wifi_ap_t, 32);
    int k = t5_wifi_excl_scan(excl_of(a[0], T5_EXCL_WIFI), &cfg, aps, 32);
    if (k < 0) py_raise_t5((t5_err_t)k);
    mp_obj_t l = mp_obj_new_list(0, NULL);
    for (int i = 0; i < k; i++) {
        mp_obj_t d = mp_obj_new_dict(5);
        char bssid[18];
        snprintf(bssid, sizeof bssid, "%02x:%02x:%02x:%02x:%02x:%02x", aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2],
                 aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5]);
        dict_set(d, MP_QSTR_ssid, mp_obj_new_str(aps[i].ssid, strnlen(aps[i].ssid, 32)));
        dict_set(d, MP_QSTR_bssid, mp_obj_new_str(bssid, strlen(bssid)));
        dict_set(d, MP_QSTR_rssi, MP_OBJ_NEW_SMALL_INT(aps[i].rssi));
        dict_set(d, MP_QSTR_channel, MP_OBJ_NEW_SMALL_INT(aps[i].channel));
        dict_set(d, MP_QSTR_auth, MP_OBJ_NEW_SMALL_INT(aps[i].auth));
        mp_obj_list_append(l, d);
    }
    return l;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(excl_scan_obj, 1, 4, excl_scan);

/* connect(ssid, password=None, timeout=15000) */
static mp_obj_t excl_connect(size_t n, const mp_obj_t *a)
{
    const char *pass = n > 2 && a[2] != mp_const_none ? mp_obj_str_get_str(a[2]) : NULL;
    py_check(t5_wifi_excl_connect(excl_of(a[0], T5_EXCL_WIFI), mp_obj_str_get_str(a[1]), pass,
                                  n > 3 ? (uint32_t)mp_obj_get_int(a[3]) : 15000));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(excl_connect_obj, 2, 4, excl_connect);

static mp_obj_t excl_disconnect(mp_obj_t o)
{
    py_check(t5_wifi_excl_disconnect(excl_of(o, T5_EXCL_WIFI)));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(excl_disconnect_obj, excl_disconnect);

/* ap(ssid, password=None, channel=0, max_clients=0, hidden=False); ap(None) stops it */
static mp_obj_t excl_ap(size_t n, const mp_obj_t *a)
{
    t5_excl_t *h = excl_of(a[0], T5_EXCL_WIFI);
    if (a[1] == mp_const_none) {
        py_check(t5_wifi_excl_ap_stop(h));
        return mp_const_none;
    }
    t5_wifi_ap_cfg_t cfg = { .size = sizeof cfg, .ssid = mp_obj_str_get_str(a[1]),
                             .pass = n > 2 && a[2] != mp_const_none ? mp_obj_str_get_str(a[2]) : NULL,
                             .channel = n > 3 ? (uint8_t)mp_obj_get_int(a[3]) : 0,
                             .max_clients = n > 4 ? (uint8_t)mp_obj_get_int(a[4]) : 0,
                             .hidden = n > 5 && mp_obj_is_true(a[5]) };
    py_check(t5_wifi_excl_ap_start(h, &cfg));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(excl_ap_obj, 2, 6, excl_ap);

static mp_obj_t excl_ap_clients(mp_obj_t o)
{
    uint8_t mac[8][6];
    int k = t5_wifi_excl_ap_clients(excl_of(o, T5_EXCL_WIFI), mac, 8);
    if (k < 0) py_raise_t5((t5_err_t)k);
    mp_obj_t l = mp_obj_new_list(0, NULL);
    for (int i = 0; i < k; i++) mp_obj_list_append(l, mp_obj_new_bytes(mac[i], 6));
    return l;
}
static MP_DEFINE_CONST_FUN_OBJ_1(excl_ap_clients_obj, excl_ap_clients);

/* radio(country=None, tx_power=0, protocols=0, bandwidth=0) -> dict now */
static mp_obj_t excl_radio(size_t n, const mp_obj_t *pos, mp_map_t *kw)
{
    enum { A_SELF, A_CC, A_PW, A_PROTO, A_BW };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_country, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_tx_power, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_protocols, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_bandwidth, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
    };
    mp_arg_val_t v[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n, pos, kw, MP_ARRAY_SIZE(allowed), allowed, v);
    t5_wifi_radio_t set = { .size = sizeof set, .tx_power_dbm = (int8_t)v[A_PW].u_int,
                            .protocols = (uint8_t)v[A_PROTO].u_int, .bandwidth = (uint8_t)v[A_BW].u_int };
    if (v[A_CC].u_obj != mp_const_none) snprintf(set.country, sizeof set.country, "%s", mp_obj_str_get_str(v[A_CC].u_obj));
    t5_wifi_radio_t get = { .size = sizeof get };
    py_check(t5_wifi_excl_radio(excl_of(v[A_SELF].u_obj, T5_EXCL_WIFI), &set, &get));
    mp_obj_t d = mp_obj_new_dict(4);
    dict_set(d, MP_QSTR_country, mp_obj_new_str(get.country, strlen(get.country)));
    dict_set(d, MP_QSTR_tx_power, MP_OBJ_NEW_SMALL_INT(get.tx_power_dbm));
    dict_set(d, MP_QSTR_protocols, MP_OBJ_NEW_SMALL_INT(get.protocols));
    dict_set(d, MP_QSTR_bandwidth, MP_OBJ_NEW_SMALL_INT(get.bandwidth));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(excl_radio_obj, 1, excl_radio);

/* mac(new=None) -> bytes */
static mp_obj_t excl_mac(size_t n, const mp_obj_t *a)
{
    t5_excl_t *h = excl_of(a[0], T5_EXCL_WIFI);
    uint8_t mac[6];
    if (n > 1 && a[1] != mp_const_none) {
        mp_buffer_info_t b;
        mp_get_buffer_raise(a[1], &b, MP_BUFFER_READ);
        if (b.len != 6) mp_raise_ValueError(MP_ERROR_TEXT("6 bytes"));
        memcpy(mac, b.buf, 6);
        py_check(t5_wifi_excl_mac(h, mac, true));
    }
    py_check(t5_wifi_excl_mac(h, mac, false));
    return mp_obj_new_bytes(mac, 6);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(excl_mac_obj, 1, 2, excl_mac);

static const mp_rom_map_elem_t excl_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_active), MP_ROM_PTR(&excl_active_obj) },
    { MP_ROM_QSTR(MP_QSTR_release), MP_ROM_PTR(&excl_release_obj) },
    { MP_ROM_QSTR(MP_QSTR_hci_send), MP_ROM_PTR(&excl_hci_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_hci_next), MP_ROM_PTR(&excl_hci_next_obj) },
    { MP_ROM_QSTR(MP_QSTR_power), MP_ROM_PTR(&excl_usba_power_obj) },
    { MP_ROM_QSTR(MP_QSTR_scan), MP_ROM_PTR(&excl_scan_obj) },
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&excl_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&excl_disconnect_obj) },
    { MP_ROM_QSTR(MP_QSTR_ap), MP_ROM_PTR(&excl_ap_obj) },
    { MP_ROM_QSTR(MP_QSTR_ap_clients), MP_ROM_PTR(&excl_ap_clients_obj) },
    { MP_ROM_QSTR(MP_QSTR_radio), MP_ROM_PTR(&excl_radio_obj) },
    { MP_ROM_QSTR(MP_QSTR_mac), MP_ROM_PTR(&excl_mac_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&excl_release_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&excl_exit_obj) },
};
static MP_DEFINE_CONST_DICT(excl_locals, excl_locals_table);
static MP_DEFINE_CONST_OBJ_TYPE(py_type_excl, MP_QSTR_Exclusive, MP_TYPE_FLAG_NONE, locals_dict, &excl_locals);

/* exclusive("usba" | "wifi" | "bt"): asks the person the first time. */
static mp_obj_t t5_exclusive(mp_obj_t what)
{
    const char *w = mp_obj_str_get_str(what);
    t5_excl_kind_t k;
    uint32_t perm;
    if (!strcmp(w, "usba")) {
        k = T5_EXCL_USBA;
        perm = T5G_PERM_t5_usba_power;
    } else if (!strcmp(w, "wifi")) {
        k = T5_EXCL_WIFI;
        perm = T5G_PERM_t5_wifi_excl_scan;
    } else if (!strcmp(w, "bt")) {
        k = T5_EXCL_BT;
        perm = T5G_PERM_t5_bt_hci_send;
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("usba, wifi or bt"));
    }
    py_need_perm(perm);
    py_excl_t *o = mp_obj_malloc_with_finaliser(py_excl_t, &py_type_excl);
    o->h = NULL;
    o->kind = k;
    py_check(t5_excl_claim(k, &o->h));
    return MP_OBJ_FROM_PTR(o);
}
MP_DEFINE_CONST_FUN_OBJ_1(py_exclusive_obj, t5_exclusive);

