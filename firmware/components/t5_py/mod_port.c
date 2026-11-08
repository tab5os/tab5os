/* SPDX-License-Identifier: Apache-2.0
 * t5.port (plan 8.3): the named external ports, the same broker native
 * apps use. Needs the ports permission and the port listed in the app's
 * manifest (t5_app_port_open_ex enforces that, like the native ABI).
 *
 *   p = t5.port.open("grove", mode="i2c", hz=400_000)
 *   p.read_reg(0x44, 0x00, 2)
 *   with t5.port.open("grove", mode="gpio", pin=0, output=True) as led:
 *       led.value(1)
 *
 * Long waits (UART reads) are cut into short slices so Ctrl-C, Stop and
 * scheduled UI callbacks still get through.
 */
#include <string.h>
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "t5_app/app.h"
#include "t5_app/wrappers.h"
#include "t5_pkg/manifest.h"
#include "t5_ports/ports.h"
#include "t5_power/rail.h"
#include "py_priv.h"

typedef struct {
    mp_obj_base_t base;
    t5_port_t *p;
    t5_port_mode_t mode;
} py_port_t;

void py_port_reset(void)
{
}

static py_port_t *self_open(mp_obj_t o, t5_port_mode_t need)
{
    py_port_t *self = MP_OBJ_TO_PTR(o);
    if (!self->p) mp_raise_ValueError(MP_ERROR_TEXT("port is closed"));
    if (need != T5_MODE_COUNT && self->mode != need &&
        !(need == T5_MODE_UART && self->mode == T5_MODE_RS485)) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("not available in %s mode"),
                          t5_port_mode_name(self->mode));
    }
    return self;
}

static mp_obj_t port_close(mp_obj_t o)
{
    py_port_t *self = MP_OBJ_TO_PTR(o);
    if (self->p) T5_IGNORE(t5_port_close(self->p));   /* finaliser: nothing to report to */
    self->p = NULL;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(port_close_obj, port_close);

static mp_obj_t port_exit(size_t n, const mp_obj_t *args)
{
    (void)n;
    return port_close(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(port_exit_obj, 4, 4, port_exit);

static void port_print(const mp_print_t *print, mp_obj_t o, mp_print_kind_t kind)
{
    (void)kind;
    py_port_t *self = MP_OBJ_TO_PTR(o);
    if (!self->p) mp_printf(print, "<Port closed>");
    else mp_printf(print, "<Port %s %s>", t5_port_name(self->p), t5_port_mode_name(self->mode));
}

/* ---- I2C ------------------------------------------------------------------------------- */
static uint8_t addr_arg(mp_obj_t a)
{
    mp_int_t v = mp_obj_get_int(a);
    if (v < 0 || v > 0x7f) mp_raise_ValueError(MP_ERROR_TEXT("7-bit address"));
    return (uint8_t)v;
}

static mp_obj_t port_read_reg(size_t n, const mp_obj_t *args)
{
    (void)n;
    py_port_t *self = self_open(args[0], T5_MODE_I2C);
    uint8_t addr = addr_arg(args[1]);
    uint8_t reg = (uint8_t)mp_obj_get_int(args[2]);
    mp_int_t len = mp_obj_get_int(args[3]);
    if (len < 0 || len > 4096) mp_raise_ValueError(MP_ERROR_TEXT("length"));
    vstr_t v;
    vstr_init_len(&v, (size_t)len);
    t5_err_t e = t5_port_i2c_read_reg(self->p, addr, reg, (uint8_t *)v.buf, (size_t)len);
    if (e != T5_OK) {
        vstr_clear(&v);
        py_raise_t5(e);
    }
    return mp_obj_new_bytes_from_vstr(&v);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(port_read_reg_obj, 4, 4, port_read_reg);

static mp_obj_t port_write_reg(size_t n, const mp_obj_t *args)
{
    (void)n;
    py_port_t *self = self_open(args[0], T5_MODE_I2C);
    uint8_t addr = addr_arg(args[1]);
    uint8_t reg = (uint8_t)mp_obj_get_int(args[2]);
    mp_buffer_info_t b;
    uint8_t one;
    if (mp_obj_is_int(args[3])) {
        one = (uint8_t)mp_obj_get_int(args[3]);
        b.buf = &one;
        b.len = 1;
    } else {
        mp_get_buffer_raise(args[3], &b, MP_BUFFER_READ);
    }
    py_check(t5_port_i2c_write_reg(self->p, addr, reg, b.buf, b.len));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(port_write_reg_obj, 4, 4, port_write_reg);

static mp_obj_t port_xfer(size_t n, const mp_obj_t *args)
{
    py_port_t *self = self_open(args[0], T5_MODE_I2C);
    uint8_t addr = addr_arg(args[1]);
    mp_buffer_info_t tx = { .buf = NULL, .len = 0 };
    if (n > 2 && args[2] != mp_const_none) mp_get_buffer_raise(args[2], &tx, MP_BUFFER_READ);
    mp_int_t rxlen = n > 3 ? mp_obj_get_int(args[3]) : 0;
    if (rxlen < 0 || rxlen > 4096) mp_raise_ValueError(MP_ERROR_TEXT("length"));
    vstr_t v;
    vstr_init_len(&v, (size_t)rxlen);
    t5_err_t e = t5_port_i2c_xfer(self->p, addr, tx.buf, tx.len, (uint8_t *)v.buf, (size_t)rxlen);
    if (e != T5_OK) {
        vstr_clear(&v);
        py_raise_t5(e);
    }
    return mp_obj_new_bytes_from_vstr(&v);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(port_xfer_obj, 2, 4, port_xfer);

static mp_obj_t port_probe(mp_obj_t o, mp_obj_t a)
{
    py_port_t *self = self_open(o, T5_MODE_I2C);
    t5_err_t e = t5_port_i2c_probe(self->p, addr_arg(a));
    if (e == T5_ERR_NACK) return mp_const_false;
    py_check(e);
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_2(port_probe_obj, port_probe);

static mp_obj_t port_scan(mp_obj_t o)
{
    py_port_t *self = self_open(o, T5_MODE_I2C);
    uint8_t found[112];
    int k = t5_port_i2c_scan(self->p, found, sizeof found);
    if (k < 0) py_raise_t5((t5_err_t)k);
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (int i = 0; i < k && i < (int)sizeof found; i++) mp_obj_list_append(list, MP_OBJ_NEW_SMALL_INT(found[i]));
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_1(port_scan_obj, port_scan);

/* ---- UART / RS-485 ------------------------------------------------------------------------ */
static mp_obj_t port_write(mp_obj_t o, mp_obj_t data)
{
    py_port_t *self = self_open(o, T5_MODE_UART);
    mp_buffer_info_t b;
    mp_get_buffer_raise(data, &b, MP_BUFFER_READ);
    int r = t5_port_uart_write(self->p, b.buf, b.len);
    if (r < 0) py_raise_t5((t5_err_t)r);
    return MP_OBJ_NEW_SMALL_INT(r);
}
static MP_DEFINE_CONST_FUN_OBJ_2(port_write_obj, port_write);

/* read(n, timeout_ms=100): up to n bytes, returning early once nothing more
 * arrives for 20 ms after the first byte. */
static mp_obj_t port_read(size_t n_args, const mp_obj_t *args)
{
    py_port_t *self = self_open(args[0], T5_MODE_UART);
    mp_int_t want = mp_obj_get_int(args[1]);
    mp_int_t timeout = n_args > 2 ? mp_obj_get_int(args[2]) : 100;
    if (want < 0 || want > 65536) mp_raise_ValueError(MP_ERROR_TEXT("length"));
    vstr_t v;
    vstr_init(&v, (size_t)want ? (size_t)want : 1);
    uint32_t t0 = mp_hal_ticks_ms();
    while ((mp_int_t)v.len < want) {
        uint32_t el = mp_hal_ticks_ms() - t0;
        if (!v.len && (mp_int_t)el >= timeout) break;
        uint32_t slice = v.len ? 20 : ((mp_int_t)(timeout - el) < 50 ? (uint32_t)(timeout - el) : 50);
        int r = t5_port_uart_read(self->p, v.buf + v.len, (size_t)want - v.len, slice);
        if (r < 0 && r != T5_ERR_TIMEOUT) {
            vstr_clear(&v);
            py_raise_t5((t5_err_t)r);
        }
        if (r > 0) v.len += (size_t)r;
        else if (v.len) break;
        mp_event_handle_nowait();
    }
    return mp_obj_new_bytes_from_vstr(&v);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(port_read_obj, 2, 3, port_read);

static mp_obj_t port_flush(mp_obj_t o)
{
    py_check(t5_port_uart_flush(self_open(o, T5_MODE_UART)->p));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(port_flush_obj, port_flush);

static mp_obj_t port_wait_tx(size_t n, const mp_obj_t *args)
{
    py_port_t *self = self_open(args[0], T5_MODE_UART);
    uint32_t ms = n > 1 ? (uint32_t)mp_obj_get_int(args[1]) : 1000;
    py_check(t5_port_uart_wait_tx(self->p, ms));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(port_wait_tx_obj, 1, 2, port_wait_tx);

/* ---- SPI ------------------------------------------------------------------------------------ */
static mp_obj_t port_transfer(mp_obj_t o, mp_obj_t data)
{
    py_port_t *self = self_open(o, T5_MODE_SPI);
    mp_buffer_info_t b = { 0 };
    size_t len;
    if (mp_obj_is_int(data)) {
        len = (size_t)mp_obj_get_int(data);
    } else {
        mp_get_buffer_raise(data, &b, MP_BUFFER_READ);
        len = b.len;
    }
    if (len > 65536) mp_raise_ValueError(MP_ERROR_TEXT("length"));
    vstr_t v;
    vstr_init_len(&v, len);
    t5_err_t e = t5_port_spi_transfer(self->p, b.buf, v.buf, len);
    if (e != T5_OK) {
        vstr_clear(&v);
        py_raise_t5(e);
    }
    return mp_obj_new_bytes_from_vstr(&v);
}
static MP_DEFINE_CONST_FUN_OBJ_2(port_transfer_obj, port_transfer);

/* ---- GPIO, PWM, ADC --------------------------------------------------------------------------- */
static mp_obj_t port_value(size_t n, const mp_obj_t *args)
{
    py_port_t *self = self_open(args[0], T5_MODE_GPIO);
    if (n > 1) {
        py_check(t5_port_gpio_set(self->p, mp_obj_is_true(args[1])));
        return mp_const_none;
    }
    bool level = false;
    py_check(t5_port_gpio_get(self->p, &level));
    return MP_OBJ_NEW_SMALL_INT(level ? 1 : 0);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(port_value_obj, 1, 2, port_value);

static mp_obj_t port_duty(mp_obj_t o, mp_obj_t permille)
{
    mp_int_t d = mp_obj_get_int(permille);
    if (d < 0 || d > 1000) mp_raise_ValueError(MP_ERROR_TEXT("duty is 0..1000"));
    py_check(t5_port_pwm_set(self_open(o, T5_MODE_PWM)->p, (uint32_t)d));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(port_duty_obj, port_duty);

static mp_obj_t port_read_mv(mp_obj_t o)
{
    int mv = 0;
    py_check(t5_port_adc_read_mv(self_open(o, T5_MODE_ADC)->p, &mv));
    return MP_OBJ_NEW_SMALL_INT(mv);
}
static MP_DEFINE_CONST_FUN_OBJ_1(port_read_mv_obj, port_read_mv);

static const mp_rom_map_elem_t port_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&port_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&port_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&port_exit_obj) },
    { MP_ROM_QSTR(MP_QSTR_read_reg), MP_ROM_PTR(&port_read_reg_obj) },
    { MP_ROM_QSTR(MP_QSTR_write_reg), MP_ROM_PTR(&port_write_reg_obj) },
    { MP_ROM_QSTR(MP_QSTR_xfer), MP_ROM_PTR(&port_xfer_obj) },
    { MP_ROM_QSTR(MP_QSTR_probe), MP_ROM_PTR(&port_probe_obj) },
    { MP_ROM_QSTR(MP_QSTR_scan), MP_ROM_PTR(&port_scan_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&port_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&port_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&port_flush_obj) },
    { MP_ROM_QSTR(MP_QSTR_wait_tx), MP_ROM_PTR(&port_wait_tx_obj) },
    { MP_ROM_QSTR(MP_QSTR_transfer), MP_ROM_PTR(&port_transfer_obj) },
    { MP_ROM_QSTR(MP_QSTR_value), MP_ROM_PTR(&port_value_obj) },
    { MP_ROM_QSTR(MP_QSTR_duty), MP_ROM_PTR(&port_duty_obj) },
    { MP_ROM_QSTR(MP_QSTR_read_mv), MP_ROM_PTR(&port_read_mv_obj) },
};
static MP_DEFINE_CONST_DICT(port_locals, port_locals_table);
static MP_DEFINE_CONST_OBJ_TYPE(py_type_port, MP_QSTR_Port, MP_TYPE_FLAG_NONE, print, port_print, locals_dict,
                                &port_locals);

static mp_obj_t port_open(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
    enum { A_name, A_mode, A_hz, A_baud, A_bits, A_parity, A_stop, A_rxbuf, A_spi_mode, A_cs, A_pin, A_output,
           A_pull_up, A_pull_down, A_open_drain, A_risky };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_name, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_mode, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_QSTR(MP_QSTR_i2c) } },
        { MP_QSTR_hz, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_baud, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 115200 } },
        { MP_QSTR_bits, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 8 } },
        { MP_QSTR_parity, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_stop, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 1 } },
        { MP_QSTR_rxbuf, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 1024 } },
        { MP_QSTR_spi_mode, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_cs, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = -1 } },
        { MP_QSTR_pin, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_output, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_pull_up, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_pull_down, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_open_drain, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_risky, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    py_need_perm(T5_PERM_PORTS);
    const char *name = mp_obj_str_get_str(a[A_name].u_obj);
    t5_port_mode_t mode = t5_port_mode_parse(mp_obj_str_get_str(a[A_mode].u_obj));
    if (mode == T5_MODE_COUNT) mp_raise_ValueError(MP_ERROR_TEXT("mode is i2c, uart, rs485, spi, gpio, pwm or adc"));
    union {
        t5_port_i2c_cfg_t i2c;
        t5_port_uart_cfg_t uart;
        t5_port_spi_cfg_t spi;
        t5_port_gpio_cfg_t gpio;
        t5_port_pwm_cfg_t pwm;
        t5_port_adc_cfg_t adc;
    } cfg;
    memset(&cfg, 0, sizeof cfg);
    switch (mode) {
    case T5_MODE_I2C: cfg.i2c.hz = a[A_hz].u_int ? (uint32_t)a[A_hz].u_int : 100000; break;
    case T5_MODE_UART:
    case T5_MODE_RS485:
        cfg.uart.baud = (uint32_t)a[A_baud].u_int;
        cfg.uart.data_bits = (uint8_t)a[A_bits].u_int;
        cfg.uart.parity = (uint8_t)a[A_parity].u_int;
        cfg.uart.stop_bits = (uint8_t)a[A_stop].u_int;
        cfg.uart.rx_buf = (size_t)a[A_rxbuf].u_int;
        break;
    case T5_MODE_SPI:
        cfg.spi.hz = a[A_hz].u_int ? (uint32_t)a[A_hz].u_int : 1000000;
        cfg.spi.mode = (uint8_t)a[A_spi_mode].u_int;
        cfg.spi.cs_pin_index = (int)a[A_cs].u_int;
        break;
    case T5_MODE_GPIO:
        cfg.gpio.pin_index = (uint8_t)a[A_pin].u_int;
        cfg.gpio.output = a[A_output].u_bool;
        cfg.gpio.pull_up = a[A_pull_up].u_bool;
        cfg.gpio.pull_down = a[A_pull_down].u_bool;
        cfg.gpio.open_drain = a[A_open_drain].u_bool;
        break;
    case T5_MODE_PWM:
        cfg.pwm.pin_index = (uint8_t)a[A_pin].u_int;
        cfg.pwm.hz = a[A_hz].u_int ? (uint32_t)a[A_hz].u_int : 1000;
        cfg.pwm.resolution_bits = 10;
        break;
    case T5_MODE_ADC: cfg.adc.pin_index = (uint8_t)a[A_pin].u_int; break;
    default: break;
    }
    py_port_t *o = mp_obj_malloc_with_finaliser(py_port_t, &py_type_port);
    o->p = NULL;
    o->mode = mode;
    char why[T5_PORT_WHY_LEN] = "";
    uint32_t flags = a[A_risky].u_bool ? T5_PORT_FLAG_RISKY : 0;
    t5_err_t e = py_vm && py_vm->app ? t5_app_port_open_ex(name, mode, &cfg, flags, &o->p, why, sizeof why)
                                     : t5_port_open_ex(name, mode, &cfg, flags, &o->p, why, sizeof why);
    if (e != T5_OK) {
        if (why[0]) {
            mp_obj_t args[2] = { MP_OBJ_NEW_SMALL_INT(e == T5_ERR_BUSY ? MP_EBUSY : e == T5_ERR_DENIED ? MP_EACCES : MP_EIO),
                                 mp_obj_new_str_from_cstr(why) };
            nlr_raise(mp_obj_exception_make_new(&mp_type_OSError, 2, 0, args));
        }
        py_raise_t5(e);
    }
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(port_open_obj, 1, port_open);

static mp_obj_t port_names(void)
{
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (int i = 0; i < t5_ports_count(); i++) {
        t5_port_info_t info;
        if (t5_ports_info(i, &info) == T5_OK) mp_obj_list_append(list, mp_obj_new_str_from_cstr(info.name));
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_0(port_names_obj, port_names);

/* ---- switched rails: t5.port.rail("ext5v") ------------------------------------------------- */
typedef struct {
    mp_obj_base_t base;
    t5_rail_hold_t *hold;
} py_rail_t;

static mp_obj_t rail_release(mp_obj_t o)
{
    py_rail_t *self = MP_OBJ_TO_PTR(o);
    if (self->hold) T5_IGNORE(t5_rail_release(self->hold));   /* only fails for a bad handle */
    self->hold = NULL;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(rail_release_obj, rail_release);

static mp_obj_t rail_exit(size_t n, const mp_obj_t *args)
{
    (void)n;
    return rail_release(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(rail_exit_obj, 4, 4, rail_exit);

static const mp_rom_map_elem_t rail_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_release), MP_ROM_PTR(&rail_release_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&rail_release_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&rail_exit_obj) },
};
static MP_DEFINE_CONST_DICT(rail_locals, rail_locals_table);
static MP_DEFINE_CONST_OBJ_TYPE(py_type_rail, MP_QSTR_Rail, MP_TYPE_FLAG_NONE, locals_dict, &rail_locals);

static mp_obj_t port_rail(mp_obj_t name)
{
    py_need_perm(T5_PERM_PORTS);
    const char *n = mp_obj_str_get_str(name);
    int rail = -1;
    for (int i = 0; i < T5_RAIL_COUNT; i++) {
        if (!strcmp(n, t5_rail_name((t5_rail_t)i))) rail = i;
    }
    /* Apps may switch the external 5 V only. */
    if (rail != T5_RAIL_EXT5V) mp_raise_ValueError(MP_ERROR_TEXT("apps can hold the ext5v rail only"));
    py_rail_t *o = mp_obj_malloc_with_finaliser(py_rail_t, &py_type_rail);
    o->hold = NULL;
    const char *owner = py_vm && py_vm->app ? t5_app_id(py_vm->app) : "python";
    py_check(t5_rail_acquire((t5_rail_t)rail, owner, 0, &o->hold));
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_1(port_rail_obj, port_rail);

static const mp_rom_map_elem_t port_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_port) },
    { MP_ROM_QSTR(MP_QSTR_open), MP_ROM_PTR(&port_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_names), MP_ROM_PTR(&port_names_obj) },
    { MP_ROM_QSTR(MP_QSTR_rail), MP_ROM_PTR(&port_rail_obj) },
};
static MP_DEFINE_CONST_DICT(port_globals, port_globals_table);
const mp_obj_module_t py_mod_port = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&port_globals };
