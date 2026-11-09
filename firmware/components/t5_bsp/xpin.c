/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include "t5_bsp/xpin.h"
#include "t5_bsp/xpin_test.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_i2c/t5_i2c.h"
#include "t5_pins/table.h"

static const char *TAG = "t5.xpin";

enum {
    REG_ID = 0x01,       /* bit1: reset interrupt (set after power-on, cleared on read). Never write bit0. */
    REG_DIR = 0x03,      /* 1 = output */
    REG_OUT = 0x05,
    REG_OUT_HIZ = 0x07,  /* 1 = output high impedance */
    REG_PULL_EN = 0x0B,
    REG_PULL_UP = 0x0D,  /* 1 = up, 0 = down */
    REG_IN = 0x0F,
    REG_INT_MASK = 0x11, /* 1 = masked */
};

#define ID_RESET_FLAG 0x02

typedef struct {
    uint8_t addr;
    t5_i2c_dev_t *dev;
    uint8_t dir, out, hiz, pull_en, pull_up;
    bool valid;
} exp_t;

static exp_t s_exp[2] = { { .addr = T5_I2C_ADDR_EXP0 }, { .addr = T5_I2C_ADDR_EXP1 } };
static uint32_t s_reapplied;
static bool s_inited;

static exp_t *find(t5_xpin_t pin)
{
    uint8_t a = T5_XPIN_ADDR(pin);
    for (int i = 0; i < 2; i++) {
        if (s_exp[i].addr == a && s_exp[i].valid) {
            return &s_exp[i];
        }
    }
    return NULL;
}

static t5_err_t write_all(exp_t *e)
{
    /* Output level first, then drive enable, then direction: the same order a
     * single-pin change uses, so a re-apply never glitches a line. */
    T5_RETURN_ON_ERR(t5_i2c_write_u8(e->dev, REG_OUT, e->out));
    T5_RETURN_ON_ERR(t5_i2c_write_u8(e->dev, REG_PULL_UP, e->pull_up));
    T5_RETURN_ON_ERR(t5_i2c_write_u8(e->dev, REG_PULL_EN, e->pull_en));
    T5_RETURN_ON_ERR(t5_i2c_write_u8(e->dev, REG_OUT_HIZ, e->hiz));
    T5_RETURN_ON_ERR(t5_i2c_write_u8(e->dev, REG_DIR, e->dir));
    return t5_i2c_write_u8(e->dev, REG_INT_MASK, 0xFF);
}

static t5_err_t adopt(exp_t *e)
{
    uint8_t id;
    T5_RETURN_ON_ERR(t5_i2c_read_u8(e->dev, REG_ID, &id));
    if ((id >> 5) != 0x5) {
        T5_LOGE(TAG, "0x%02x: unexpected ID 0x%02x", e->addr, id);
        return T5_ERR_NOT_FOUND;
    }
    T5_RETURN_ON_ERR(t5_i2c_read_u8(e->dev, REG_DIR, &e->dir));
    T5_RETURN_ON_ERR(t5_i2c_read_u8(e->dev, REG_OUT, &e->out));
    T5_RETURN_ON_ERR(t5_i2c_read_u8(e->dev, REG_OUT_HIZ, &e->hiz));
    T5_RETURN_ON_ERR(t5_i2c_read_u8(e->dev, REG_PULL_EN, &e->pull_en));
    T5_RETURN_ON_ERR(t5_i2c_read_u8(e->dev, REG_PULL_UP, &e->pull_up));
    T5_LOGI(TAG, "0x%02x adopted%s: dir %02x out %02x hiz %02x pull %02x/%02x", e->addr,
            (id & ID_RESET_FLAG) ? " (fresh power-on)" : "", e->dir, e->out, e->hiz, e->pull_en, e->pull_up);
    return T5_OK;
}

static void on_bus_recovered(void *arg)
{
    (void)arg;
    t5_xpin_reapply();
}

t5_err_t t5_xpin_init(void)
{
    if (s_inited) {
        return T5_OK;
    }
    for (int i = 0; i < 2; i++) {
        exp_t *e = &s_exp[i];
        if (!e->dev) {
            T5_RETURN_ON_ERR(t5_i2c_open(e->addr, T5_SYS_I2C_HZ, i ? "exp1" : "exp0", &e->dev));
        }
        T5_RETURN_ON_ERR(adopt(e));
        e->valid = true;
    }
    T5_RETURN_ON_ERR(t5_i2c_on_recover(on_bus_recovered, NULL));
    s_inited = true;
    return T5_OK;
}

t5_err_t t5_xpin_reapply(void)
{
    t5_err_t err = T5_OK;
    for (int i = 0; i < 2; i++) {
        if (s_exp[i].valid) {
            t5_err_t e = write_all(&s_exp[i]);
            if (e != T5_OK) {
                err = e;
            }
        }
    }
    s_reapplied++;
    return err;
}

uint32_t t5_xpin_reapply_count(void)
{
    return s_reapplied;
}

static t5_err_t set_bit(exp_t *e, uint8_t reg, uint8_t *shadow, uint8_t bit, bool on)
{
    uint8_t v = on ? (uint8_t)(*shadow | bit) : (uint8_t)(*shadow & ~bit);
    if (v == *shadow) {
        return T5_OK;
    }
    T5_RETURN_ON_ERR(t5_i2c_write_u8(e->dev, reg, v));
    *shadow = v;
    return T5_OK;
}

t5_err_t t5_xpin_output(t5_xpin_t pin, bool level)
{
    exp_t *e = find(pin);
    if (!e) {
        return T5_ERR_INVALID_ARG;
    }
    uint8_t b = (uint8_t)(1u << T5_XPIN_NUM(pin));
    T5_RETURN_ON_ERR(t5_i2c_lock());
    t5_err_t err = set_bit(e, REG_OUT, &e->out, b, level);
    if (err == T5_OK) err = set_bit(e, REG_OUT_HIZ, &e->hiz, b, false);
    if (err == T5_OK) err = set_bit(e, REG_DIR, &e->dir, b, true);
    t5_i2c_unlock();
    return err;
}

t5_err_t t5_xpin_input(t5_xpin_t pin, t5_xpin_pull_t pull)
{
    exp_t *e = find(pin);
    if (!e) {
        return T5_ERR_INVALID_ARG;
    }
    uint8_t b = (uint8_t)(1u << T5_XPIN_NUM(pin));
    T5_RETURN_ON_ERR(t5_i2c_lock());
    /* Pull first so the line is held while the driver lets go. */
    t5_err_t err = T5_OK;
    if (pull != T5_XPIN_PULL_NONE) {
        err = set_bit(e, REG_PULL_UP, &e->pull_up, b, pull == T5_XPIN_PULL_UP);
    }
    if (err == T5_OK) err = set_bit(e, REG_PULL_EN, &e->pull_en, b, pull != T5_XPIN_PULL_NONE);
    if (err == T5_OK) err = set_bit(e, REG_DIR, &e->dir, b, false);
    t5_i2c_unlock();
    return err;
}

t5_err_t t5_xpin_pulse(t5_xpin_t pin, bool level, uint32_t ms)
{
    T5_RETURN_ON_ERR(t5_xpin_output(pin, level));
    t5_delay_ms(ms);
    return t5_xpin_output(pin, !level);
}

t5_err_t t5_xpin_read(t5_xpin_t pin, bool *level)
{
    exp_t *e = find(pin);
    if (!e || !level) {
        return T5_ERR_INVALID_ARG;
    }
    /* The input-status register reads 0 for pins set as outputs (seen on Unit A),
     * so for an output read the chip's output register instead. Both are live
     * reads: a chip that lost its config shows up here. */
    bool is_out = e->dir & (1u << T5_XPIN_NUM(pin));
    uint8_t v;
    T5_RETURN_ON_ERR(t5_i2c_read_u8(e->dev, is_out ? REG_OUT : REG_IN, &v));
    *level = (v >> T5_XPIN_NUM(pin)) & 1;
    return T5_OK;
}

t5_xpin_mode_t t5_xpin_mode(t5_xpin_t pin)
{
    exp_t *e = find(pin);
    uint8_t b = (uint8_t)(1u << T5_XPIN_NUM(pin));
    if (!e || !(e->dir & b)) {
        return T5_XPIN_MODE_INPUT;
    }
    return (e->hiz & b) ? T5_XPIN_MODE_OUTPUT_HIZ : T5_XPIN_MODE_OUTPUT;
}

bool t5_xpin_out_level(t5_xpin_t pin)
{
    exp_t *e = find(pin);
    return e && (e->out >> T5_XPIN_NUM(pin)) & 1;
}

t5_xpin_pull_t t5_xpin_pull(t5_xpin_t pin)
{
    exp_t *e = find(pin);
    uint8_t b = (uint8_t)(1u << T5_XPIN_NUM(pin));
    if (!e || !(e->pull_en & b)) {
        return T5_XPIN_PULL_NONE;
    }
    return (e->pull_up & b) ? T5_XPIN_PULL_UP : T5_XPIN_PULL_DOWN;
}

t5_err_t t5_xpin_apply_boot_defaults(void)
{
    /* Charging stays on: losing CHG_EN on a crash loop would flatten the pack. */
    T5_RETURN_ON_ERR(t5_xpin_output(T5_XP_CHG_EN, true));
    /* Fast charge off (nCHG_QC_EN high) until the battery service applies the setting. */
    T5_RETURN_ON_ERR(t5_xpin_output(T5_XP_NCHG_QC_EN, true));
    T5_RETURN_ON_ERR(t5_xpin_input(T5_XP_CHG_STAT, T5_XPIN_PULL_NONE));
    /* Never driven high here. Output-low is its idle state. */
    T5_RETURN_ON_ERR(t5_xpin_output(T5_XP_PWROFF_PULSE, false));
    /* Switchable rails off until a service acquires them. */
    for (int i = 0; i < T5_PINS_RAIL_COUNT; i++) T5_RETURN_ON_ERR(t5_xpin_output(t5_pins_rail_en[i], false));
    T5_RETURN_ON_ERR(t5_xpin_output(T5_XP_RF_ANT_SEL, false));   /* internal antenna */
    T5_RETURN_ON_ERR(t5_xpin_output(T5_XP_CAM_RST, false));      /* camera held in reset */
    return t5_xpin_input(T5_XP_HP_DET, T5_XPIN_PULL_NONE);
}

void t5_xpin_reset_for_test(void)
{
    for (int i = 0; i < 2; i++) {
        s_exp[i].dev = NULL;
        s_exp[i].valid = false;
    }
    s_reapplied = 0;
    s_inited = false;
}
