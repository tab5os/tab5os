/* SPDX-License-Identifier: Apache-2.0
 * Ports service: open/close, claims through the broker, per-mode I/O.
 * Host-compiled; ports_hw.c does the driver work.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "t5_core/event.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_core/res.h"
#include "t5_i2c/t5_i2c.h"
#include "t5_ports/access.h"
#include "ports_hw.h"
#include "ports_priv.h"

static const char *TAG = "t5.ports";

#define MAX_HANDLES     24
#define I2C_CACHE       4       /* t5_i2c device handles kept per mbus.i2c handle */
#define I2C_TIMEOUT_MS  50
#define I2C_WRITE_MAX   128

struct t5_port {
    bool used;
    const port_def_t *def;
    t5_port_mode_t mode;
    uint8_t pins[T5_PORT_MAX_PINS];     /* claimed GPIOs */
    uint8_t npins;
    uint8_t pin;                        /* the GPIO for single-pin modes */
    bool output;
    ports_hw_t hw;
    uint32_t baud;
    uint32_t i2c_hz;
    struct {
        t5_i2c_dev_t *dev;
        uint8_t addr;
        uint32_t last;
    } i2c[I2C_CACHE];
    uint32_t i2c_tick;
    char owner[T5_PORT_OWNER_LEN];
};

static struct t5_port s_handles[MAX_HANDLES];
static t5_mutex_t s_lock;
static t5_ports_access_fn_t s_access;

static const char *const s_mode_names[T5_MODE_COUNT] = {
    [T5_MODE_I2C] = "i2c", [T5_MODE_UART] = "uart", [T5_MODE_SPI] = "spi", [T5_MODE_GPIO] = "gpio",
    [T5_MODE_PWM] = "pwm", [T5_MODE_ADC] = "adc", [T5_MODE_RS485] = "rs485",
};

const char *t5_port_mode_name(t5_port_mode_t mode)
{
    return (mode >= 0 && mode < T5_MODE_COUNT) ? s_mode_names[mode] : "?";
}

t5_port_mode_t t5_port_mode_parse(const char *s)
{
    for (int m = 0; s && m < T5_MODE_COUNT; m++) {
        if (!strcmp(s, s_mode_names[m])) {
            return (t5_port_mode_t)m;
        }
    }
    return T5_MODE_COUNT;
}

void t5_ports_set_access_check(t5_ports_access_fn_t fn)
{
    s_access = fn;
}

t5_err_t t5_ports_init(void)
{
    if (!s_lock) {
        s_lock = t5_mutex_create();
    }
    return s_lock ? T5_OK : T5_ERR_NO_MEM;
}

void t5_ports_reset_for_test(void)
{
    T5_IGNORE(t5_ports_init());   /* idempotent; tests only */
    memset(s_handles, 0, sizeof s_handles);
    s_access = NULL;
    broker_reset();
    profiles_reset();
}

__attribute__((format(printf, 3, 4)))
static void say(char *why, size_t whylen, const char *fmt, ...)
{
    if (why && whylen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(why, whylen, fmt, ap);
        va_end(ap);
    }
}

/* A live handle, or NULL. Caller holds the lock or accepts the race documented in ports.h. */
static t5_port_t *live(t5_port_t *p)
{
    if (!p || p < s_handles || p >= s_handles + MAX_HANDLES || !p->used) {
        return NULL;
    }
    return p;
}

static t5_port_t *live_mode(t5_port_t *p, t5_port_mode_t mode)
{
    p = live(p);
    if (!p) {
        return NULL;
    }
    if (p->mode == mode || (mode == T5_MODE_UART && p->mode == T5_MODE_RS485)) {
        return p;
    }
    return NULL;
}

static void post(t5_event_id_t id, const t5_port_t *p)
{
    t5_ev_port_t ev = { 0 };
    /* Truncated on purpose: the event payload is small. */
    size_t n = strnlen(p->def->name, sizeof ev.port - 1);
    memcpy(ev.port, p->def->name, n);
    n = strnlen(p->owner, sizeof ev.owner - 1);
    memcpy(ev.owner, p->owner, n);
    t5_event_post(id, &ev, sizeof ev);
}

static void res_release(void *handle)
{
    T5_IGNORE(t5_port_close(handle));   /* already closed is fine here */
}

/* Which pins a mode needs. Returns T5_OK and fills pins, or an error and why. */
static t5_err_t mode_pins(const port_def_t *d, t5_port_mode_t mode, const void *cfg, uint8_t *pins,
                          uint8_t *n, char *why, size_t whylen)
{
    *n = 0;
    switch (mode) {
    case T5_MODE_I2C:
        if (!d->shared) {
            pins[(*n)++] = d->gpio[d->i2c[0]];
            pins[(*n)++] = d->gpio[d->i2c[1]];
        }
        return T5_OK;
    case T5_MODE_UART:
    case T5_MODE_RS485:
        for (int i = 0; i < 3; i++) {
            if (d->uart[i] >= 0) {
                pins[(*n)++] = d->gpio[d->uart[i]];
            }
        }
        return T5_OK;
    case T5_MODE_SPI: {
        for (int i = 0; i < 3; i++) {
            pins[(*n)++] = d->gpio[d->spi[i]];
        }
        int cs = cfg ? ((const t5_port_spi_cfg_t *)cfg)->cs_pin_index : -1;
        if (cs >= 0) {
            bool role = cs == d->spi[0] || cs == d->spi[1] || cs == d->spi[2];
            if (cs >= d->npins || role) {
                say(why, whylen, "%s: pin %d can't be CS", d->name, cs);
                return T5_ERR_INVALID_ARG;
            }
            pins[(*n)++] = d->gpio[cs];
        }
        return T5_OK;
    }
    case T5_MODE_GPIO:
    case T5_MODE_PWM:
    case T5_MODE_ADC: {
        /* pin_index is the first member of all three cfg structs. */
        uint8_t idx = cfg ? *(const uint8_t *)cfg : 0;
        if (idx >= d->npins) {
            say(why, whylen, "%s has no pin %u", d->name, idx);
            return T5_ERR_INVALID_ARG;
        }
        uint8_t g = d->gpio[idx];
        if (mode == T5_MODE_ADC && !ports_pin_adc(g, NULL, NULL)) {
            say(why, whylen, "%s: %s has no ADC channel", d->name, d->label[idx]);
            return T5_ERR_NOT_SUPPORTED;
        }
        if (ports_pin_hands_off(g) &&
            (mode != T5_MODE_GPIO || (cfg && ((const t5_port_gpio_cfg_t *)cfg)->output))) {
            say(why, whylen, "%s: G%u is %s, input only", d->name, g, ports_pin_risk(g));
            return T5_ERR_DENIED;
        }
        pins[(*n)++] = g;
        return T5_OK;
    }
    default:
        return T5_ERR_INVALID_ARG;
    }
}

static t5_err_t hw_open(t5_port_t *p, const void *cfg)
{
    const port_def_t *d = p->def;
    switch (p->mode) {
    case T5_MODE_I2C: {
        const t5_port_i2c_cfg_t *c = cfg;
        p->i2c_hz = (c && c->hz) ? c->hz : 100000;
        if (d->shared) {
            return T5_OK;   /* devices open lazily on t5_i2c */
        }
        return ports_hw_i2c_open(d->gpio[d->i2c[0]], d->gpio[d->i2c[1]], p->i2c_hz, &p->hw);
    }
    case T5_MODE_UART:
    case T5_MODE_RS485: {
        const t5_port_uart_cfg_t *c = cfg;
        ports_hw_uart_cfg_t h = {
            .tx = d->gpio[d->uart[0]], .rx = d->gpio[d->uart[1]],
            .rts = (p->mode == T5_MODE_RS485 && d->uart[2] >= 0) ? d->gpio[d->uart[2]] : -1,
            .baud = (c && c->baud) ? c->baud : 115200,
            .data_bits = (c && c->data_bits) ? c->data_bits : 8,
            .parity = c ? c->parity : 0,
            .stop_bits = (c && c->stop_bits) ? c->stop_bits : 1,
            .rx_buf = (c && c->rx_buf) ? c->rx_buf : 1024,
        };
        if (h.data_bits < 5 || h.data_bits > 8 || h.parity > 2 || h.stop_bits > 2) {
            return T5_ERR_INVALID_ARG;
        }
        p->baud = h.baud;
        return ports_hw_uart_open(&h, &p->hw);
    }
    case T5_MODE_SPI: {
        const t5_port_spi_cfg_t *c = cfg;
        int cs = (c && c->cs_pin_index >= 0) ? d->gpio[c->cs_pin_index] : -1;
        if (c && c->mode > 3) {
            return T5_ERR_INVALID_ARG;
        }
        return ports_hw_spi_open(d->gpio[d->spi[0]], d->gpio[d->spi[1]], d->gpio[d->spi[2]], cs,
                                 (c && c->hz) ? c->hz : 1000000, c ? c->mode : 0, &p->hw);
    }
    case T5_MODE_GPIO: {
        const t5_port_gpio_cfg_t *c = cfg;
        t5_port_gpio_cfg_t def = { 0 };
        if (!c) {
            c = &def;
        }
        p->output = c->output;
        if (ports_pin_hands_off(p->pin)) return T5_OK;
        return ports_hw_gpio_config(p->pin, c->output, c->pull_up, c->pull_down, c->open_drain);
    }
    case T5_MODE_PWM: {
        const t5_port_pwm_cfg_t *c = cfg;
        uint32_t hz = (c && c->hz) ? c->hz : 1000;
        uint8_t bits = (c && c->resolution_bits) ? c->resolution_bits : 10;
        if (bits > 14) {
            return T5_ERR_INVALID_ARG;
        }
        return ports_hw_pwm_open(p->pin, hz, bits, &p->hw);
    }
    case T5_MODE_ADC: {
        int unit, ch;
        ports_pin_adc(p->pin, &unit, &ch);
        return ports_hw_adc_open(p->pin, unit, ch, &p->hw);
    }
    default:
        return T5_ERR_INVALID_ARG;
    }
}

static void hw_close(t5_port_t *p)
{
    switch (p->mode) {
    case T5_MODE_I2C:
        if (p->def->shared) {
            for (int i = 0; i < I2C_CACHE; i++) {
                if (p->i2c[i].dev) {
                    t5_i2c_close(p->i2c[i].dev);
                    p->i2c[i].dev = NULL;
                }
            }
        } else if (p->hw) {
            ports_hw_i2c_close(p->hw);
        }
        break;
    case T5_MODE_UART:
    case T5_MODE_RS485:
        if (p->hw) ports_hw_uart_close(p->hw);
        break;
    case T5_MODE_SPI:
        if (p->hw) ports_hw_spi_close(p->hw);
        break;
    case T5_MODE_PWM:
        if (p->hw) ports_hw_pwm_close(p->hw);
        break;
    case T5_MODE_ADC:
        if (p->hw) ports_hw_adc_close(p->hw);
        break;
    default:
        break;
    }
    p->hw = NULL;
    for (int i = 0; i < p->npins; i++) {
        if (!ports_pin_hands_off(p->pins[i])) ports_hw_pin_release(p->pins[i]);
    }
}

t5_err_t t5_port_open_ex(const char *name, t5_port_mode_t mode, const void *cfg, uint32_t flags,
                         t5_port_t **out, char *why, size_t whylen)
{
    if (why && whylen) {
        why[0] = '\0';
    }
    if (!name || !out || mode < 0 || mode >= T5_MODE_COUNT) {
        return T5_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (s_access && !s_access(name, why, whylen)) return T5_ERR_DENIED;
    T5_RETURN_ON_ERR(t5_ports_init());
    const port_def_t *d = ports_table_find(name);
    if (!d) {
        say(why, whylen, "no port named %s", name);
        return T5_ERR_NOT_FOUND;
    }
    if (!(ports_modes(d) & PM(mode))) {
        say(why, whylen, "%s has no %s mode", d->name, t5_port_mode_name(mode));
        return T5_ERR_NOT_SUPPORTED;
    }
    uint8_t pins[T5_PORT_MAX_PINS];
    uint8_t n;
    T5_RETURN_ON_ERR(mode_pins(d, mode, cfg, pins, &n, why, whylen));

    const char *owner = t5_caller_id();
    t5_mutex_lock(s_lock);
    t5_port_t *p = NULL;
    for (int i = 0; i < MAX_HANDLES; i++) {
        if (!s_handles[i].used) {
            p = &s_handles[i];
            break;
        }
    }
    if (!p) {
        t5_mutex_unlock(s_lock);
        say(why, whylen, "too many open ports");
        return T5_ERR_NO_MEM;
    }
    memset(p, 0, sizeof *p);
    if (n) {
        t5_err_t err = broker_claim(pins, n, flags, owner, d->name, mode, p, why, whylen);
        if (err != T5_OK) {
            t5_mutex_unlock(s_lock);
            return err;
        }
    }
    p->used = true;
    p->def = d;
    p->mode = mode;
    memcpy(p->pins, pins, n);
    p->npins = n;
    p->pin = n ? pins[0] : 0;
    snprintf(p->owner, sizeof p->owner, "%s", owner);

    t5_err_t err = hw_open(p, cfg);
    if (err != T5_OK) {
        T5_LOGW(TAG, "%s %s: driver: %s", d->name, t5_port_mode_name(mode), t5_err_str(err));
        hw_close(p);
        broker_release(p);
        memset(p, 0, sizeof *p);
        t5_mutex_unlock(s_lock);
        say(why, whylen, "%s: driver failed (%s)", d->name, t5_err_str(err));
        return err;
    }
    T5_LOGI(TAG, "%s opened as %s by %s", d->name, t5_port_mode_name(mode), p->owner);
    t5_res_add(T5_RES_PORT, p, res_release);
    post(T5_EV_PORT_CLAIMED, p);
    t5_mutex_unlock(s_lock);
    *out = p;
    return T5_OK;
}

t5_err_t t5_port_open(const char *name, t5_port_mode_t mode, const void *cfg, t5_port_t **out)
{
    return t5_port_open_ex(name, mode, cfg, 0, out, NULL, 0);
}

t5_err_t t5_port_close(t5_port_t *p)
{
    if (!p) {
        return T5_OK;
    }
    if (p < s_handles || p >= s_handles + MAX_HANDLES) {
        return T5_ERR_INVALID_ARG;
    }
    T5_RETURN_ON_ERR(t5_ports_init());
    t5_mutex_lock(s_lock);
    if (!p->used) {
        t5_mutex_unlock(s_lock);
        return T5_OK;
    }
    hw_close(p);
    broker_release(p);
    T5_LOGI(TAG, "%s released by %s", p->def->name, p->owner);
    post(T5_EV_PORT_RELEASED, p);
    t5_res_remove(p);
    memset(p, 0, sizeof *p);
    t5_mutex_unlock(s_lock);
    return T5_OK;
}

const char *t5_port_name(const t5_port_t *p)
{
    return live((t5_port_t *)p) ? p->def->name : NULL;
}

t5_port_mode_t t5_port_mode(const t5_port_t *p)
{
    return live((t5_port_t *)p) ? p->mode : T5_MODE_COUNT;
}

/* ---- I2C ----------------------------------------------------------------- */
static bool addr_ok(uint8_t addr)
{
    return addr >= 0x08 && addr <= 0x77;
}

/* t5_i2c handle for addr on the shared bus, opened on first use. */
static t5_err_t shared_dev(t5_port_t *p, uint8_t addr, t5_i2c_dev_t **out)
{
    if (t5_i2c_addr_reserved(addr)) {
        return T5_ERR_DENIED;
    }
    t5_mutex_lock(s_lock);
    int slot = -1, oldest = 0;
    for (int i = 0; i < I2C_CACHE; i++) {
        if (p->i2c[i].dev && p->i2c[i].addr == addr) {
            slot = i;
            break;
        }
        if (!p->i2c[i].dev || (p->i2c[oldest].dev && p->i2c[i].last < p->i2c[oldest].last)) {
            oldest = i;
        }
    }
    t5_err_t err = T5_OK;
    if (slot < 0) {
        slot = oldest;
        if (p->i2c[slot].dev) {
            t5_i2c_close(p->i2c[slot].dev);
            p->i2c[slot].dev = NULL;
        }
        err = t5_i2c_open(addr, p->i2c_hz, "mbus.i2c", &p->i2c[slot].dev);
        p->i2c[slot].addr = addr;
    }
    if (err == T5_OK) {
        p->i2c[slot].last = ++p->i2c_tick;
        *out = p->i2c[slot].dev;
    }
    t5_mutex_unlock(s_lock);
    return err;
}

t5_err_t t5_port_i2c_xfer(t5_port_t *p, uint8_t addr, const uint8_t *tx, size_t txlen, uint8_t *rx,
                          size_t rxlen)
{
    p = live_mode(p, T5_MODE_I2C);
    if (!p || !addr_ok(addr) || (txlen && !tx) || (rxlen && !rx) || (!txlen && !rxlen)) {
        return T5_ERR_INVALID_ARG;
    }
    if (p->def->shared) {
        t5_i2c_dev_t *dev = NULL;
        T5_RETURN_ON_ERR(shared_dev(p, addr, &dev));
        return t5_i2c_xfer(dev, tx, txlen, rx, rxlen);
    }
    return ports_hw_i2c_xfer(p->hw, addr, tx, txlen, rx, rxlen, I2C_TIMEOUT_MS);
}

t5_err_t t5_port_i2c_read_reg(t5_port_t *p, uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    if (!len) {
        return T5_ERR_INVALID_ARG;
    }
    return t5_port_i2c_xfer(p, addr, &reg, 1, buf, len);
}

t5_err_t t5_port_i2c_write_reg(t5_port_t *p, uint8_t addr, uint8_t reg, const uint8_t *buf, size_t len)
{
    uint8_t tmp[1 + I2C_WRITE_MAX];
    if (len > I2C_WRITE_MAX || (len && !buf)) {
        return T5_ERR_INVALID_ARG;
    }
    tmp[0] = reg;
    if (len) {
        memcpy(tmp + 1, buf, len);
    }
    return t5_port_i2c_xfer(p, addr, tmp, len + 1, NULL, 0);
}

t5_err_t t5_port_i2c_probe(t5_port_t *p, uint8_t addr)
{
    p = live_mode(p, T5_MODE_I2C);
    if (!p || !addr_ok(addr)) {
        return T5_ERR_INVALID_ARG;
    }
    if (p->def->shared) {
        return t5_i2c_addr_reserved(addr) ? T5_ERR_DENIED : t5_i2c_probe(addr);
    }
    return ports_hw_i2c_probe(p->hw, addr, I2C_TIMEOUT_MS);
}

int t5_port_i2c_scan(t5_port_t *p, uint8_t *found, size_t max)
{
    p = live_mode(p, T5_MODE_I2C);
    if (!p) {
        return T5_ERR_INVALID_ARG;
    }
    int n = 0;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        if (p->def->shared && t5_i2c_addr_reserved(a)) {
            continue;
        }
        if (t5_port_i2c_probe(p, a) == T5_OK) {
            if (found && (size_t)n < max) {
                found[n] = a;
            }
            n++;
        }
    }
    return n;
}

/* ---- UART ---------------------------------------------------------------- */
int t5_port_uart_write(t5_port_t *p, const void *data, size_t n)
{
    p = live_mode(p, T5_MODE_UART);
    if (!p || (n && !data)) {
        return T5_ERR_INVALID_ARG;
    }
    return n ? ports_hw_uart_write(p->hw, data, n) : 0;
}

int t5_port_uart_read(t5_port_t *p, void *data, size_t n, uint32_t timeout_ms)
{
    p = live_mode(p, T5_MODE_UART);
    if (!p || (n && !data)) {
        return T5_ERR_INVALID_ARG;
    }
    return n ? ports_hw_uart_read(p->hw, data, n, timeout_ms) : 0;
}

t5_err_t t5_port_uart_flush(t5_port_t *p)
{
    p = live_mode(p, T5_MODE_UART);
    return p ? ports_hw_uart_flush(p->hw) : T5_ERR_INVALID_ARG;
}

t5_err_t t5_port_uart_wait_tx(t5_port_t *p, uint32_t timeout_ms)
{
    p = live_mode(p, T5_MODE_UART);
    return p ? ports_hw_uart_wait_tx(p->hw, timeout_ms) : T5_ERR_INVALID_ARG;
}

uint32_t t5_port_uart_baud(const t5_port_t *p)
{
    const t5_port_t *q = live_mode((t5_port_t *)p, T5_MODE_UART);
    return q ? q->baud : 0;
}

/* ---- SPI, GPIO, PWM, ADC ------------------------------------------------- */
t5_err_t t5_port_spi_transfer(t5_port_t *p, const void *tx, void *rx, size_t n)
{
    p = live_mode(p, T5_MODE_SPI);
    if (!p || (!tx && !rx)) {
        return T5_ERR_INVALID_ARG;
    }
    return n ? ports_hw_spi_transfer(p->hw, tx, rx, n) : T5_OK;
}

t5_err_t t5_port_gpio_set(t5_port_t *p, bool level)
{
    p = live_mode(p, T5_MODE_GPIO);
    if (!p) {
        return T5_ERR_INVALID_ARG;
    }
    if (!p->output) {
        return T5_ERR_STATE;
    }
    return ports_hw_gpio_set(p->pin, level);
}

t5_err_t t5_port_gpio_get(t5_port_t *p, bool *level)
{
    p = live_mode(p, T5_MODE_GPIO);
    if (!p || !level) {
        return T5_ERR_INVALID_ARG;
    }
    int v = ports_hw_gpio_get(p->pin);
    if (v < 0) {
        return T5_ERR_IO;
    }
    *level = v != 0;
    return T5_OK;
}

t5_err_t t5_port_pwm_set(t5_port_t *p, uint32_t duty_permille)
{
    p = live_mode(p, T5_MODE_PWM);
    if (!p || duty_permille > 1000) {
        return T5_ERR_INVALID_ARG;
    }
    return ports_hw_pwm_set(p->hw, duty_permille);
}

t5_err_t t5_port_adc_read_mv(t5_port_t *p, int *mv)
{
    p = live_mode(p, T5_MODE_ADC);
    if (!p || !mv) {
        return T5_ERR_INVALID_ARG;
    }
    return ports_hw_adc_read_mv(p->hw, mv);
}

/* ---- Enumeration --------------------------------------------------------- */
int t5_ports_count(void)
{
    return ports_table_count();
}

static void fill_info(const port_def_t *d, t5_port_info_t *out)
{
    memset(out, 0, sizeof *out);
    out->name = d->name;
    out->desc = d->desc;
    out->npins = d->npins;
    out->modes = ports_modes(d);
    out->shared = d->shared;
    for (int i = 0; i < d->npins; i++) {
        out->gpio[i] = d->gpio[i];
        out->label[i] = d->label[i];
        if (ports_pin_risky(d->gpio[i])) out->risky_mask |= 1u << i;
        if (ports_pin_adc(d->gpio[i], NULL, NULL)) out->adc_mask |= 1u << i;
    }
}

t5_err_t t5_ports_info(int i, t5_port_info_t *out)
{
    const port_def_t *d = ports_table_get(i);
    if (!d || !out) {
        return d ? T5_ERR_INVALID_ARG : T5_ERR_NOT_FOUND;
    }
    fill_info(d, out);
    return T5_OK;
}

t5_err_t t5_ports_find(const char *name, t5_port_info_t *out)
{
    const port_def_t *d = ports_table_find(name);
    if (!d || !out) {
        return d ? T5_ERR_INVALID_ARG : T5_ERR_NOT_FOUND;
    }
    fill_info(d, out);
    return T5_OK;
}

/* The first port (table order) using gpio, or NULL. */
static const port_def_t *port_of(uint8_t gpio)
{
    for (int i = 0; i < ports_table_count(); i++) {
        const port_def_t *d = ports_table_get(i);
        for (int j = 0; j < d->npins; j++) {
            if (d->gpio[j] == gpio) {
                return d;
            }
        }
    }
    return NULL;
}

int t5_ports_pins(uint8_t *gpio, int max)
{
    int n = 0;
    for (int g = 0; g < PORTS_GPIO_MAX; g++) {
        if (port_of((uint8_t)g)) {
            if (gpio && n < max) {
                gpio[n] = (uint8_t)g;
            }
            n++;
        }
    }
    return n;
}

t5_err_t t5_ports_pin_info(uint8_t gpio, t5_pin_info_t *out)
{
    const port_def_t *d = gpio < PORTS_GPIO_MAX ? port_of(gpio) : NULL;
    if (!out) {
        return T5_ERR_INVALID_ARG;
    }
    if (!d) {
        return T5_ERR_NOT_FOUND;
    }
    T5_RETURN_ON_ERR(t5_ports_init());
    memset(out, 0, sizeof *out);
    out->gpio = gpio;
    out->risky = ports_pin_risky(gpio);
    t5_mutex_lock(s_lock);
    const broker_pin_t *b = broker_pin(gpio);
    if (b->claimed) {
        out->claimed = true;
        snprintf(out->owner, sizeof out->owner, "%s", b->owner);
        out->port = b->port;
        out->mode = b->mode;
    } else if (d->shared) {
        snprintf(out->owner, sizeof out->owner, "system");
        out->port = d->name;
        out->mode = T5_MODE_I2C;
    }
    t5_mutex_unlock(s_lock);
    out->level = ports_hw_gpio_get(gpio);
    return T5_OK;
}
