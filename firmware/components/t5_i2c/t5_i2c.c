/* SPDX-License-Identifier: Apache-2.0
 * t5_i2c logic: locking, retry with recovery, fault isolation, stats, trace.
 * Talks to hardware only through t5_i2c_hw.h.
 */
#include <stdlib.h>
#include <string.h>
#include "t5_core/event.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_i2c/t5_i2c.h"
#include "t5_i2c_hw.h"
#include "t5_pins/pins.h"

static const char *TAG = "t5.i2c";

#define LOCK_TIMEOUT_MS 1000
#define MAX_RECOVER_CBS 8
#define STACK_TX 64

struct t5_i2c_dev {
    bool used;
    uint8_t addr;
    const char *name;
    t5_i2c_hw_dev_t hw;
    uint32_t xfers, errors, nacks, timeouts, consecutive;
    bool faulted;
    int32_t last_err;
    uint32_t last_attempt_ms;
};

static struct t5_i2c_dev s_devs[T5_I2C_MAX_DEVICES];
static t5_mutex_t s_bus;
static bool s_ready;
static bool s_in_recovery;
static t5_i2c_bus_stats_t s_stats;

static struct { t5_i2c_recover_cb_t cb; void *arg; } s_recover_cbs[MAX_RECOVER_CBS];

static bool s_trace_on;
static t5_i2c_trace_t s_trace[T5_I2C_TRACE_LEN];
static uint32_t s_trace_head;   /* total entries ever written */

static const uint8_t k_reserved[] = {
    T5_I2C_ADDR_ES8388, T5_I2C_ADDR_GT911, T5_I2C_ADDR_UNKNOWN28, T5_I2C_ADDR_RX8130,
    T5_I2C_ADDR_SC2356, T5_I2C_ADDR_ES7210, T5_I2C_ADDR_INA226, T5_I2C_ADDR_EXP0,
    T5_I2C_ADDR_EXP1, T5_I2C_ADDR_ST712X, T5_I2C_ADDR_GT911_ALT, T5_I2C_ADDR_BMI270,
};

bool t5_i2c_addr_reserved(uint8_t addr)
{
    for (size_t i = 0; i < sizeof k_reserved; i++) {
        if (k_reserved[i] == addr) {
            return true;
        }
    }
    return false;
}

t5_err_t t5_i2c_init(void)
{
    if (s_ready) {
        return T5_OK;
    }
    if (!s_bus) {
        s_bus = t5_mutex_create();
        if (!s_bus) {
            return T5_ERR_NO_MEM;
        }
    }
    T5_RETURN_ON_ERR(t5_i2c_hw_init());
    s_ready = true;
    return T5_OK;
}

t5_err_t t5_i2c_lock(uint32_t timeout_ms)
{
    if (!s_ready) {
        return T5_ERR_STATE;
    }
    if (!t5_mutex_lock_timeout(s_bus, timeout_ms)) {
        s_stats.lock_timeouts++;
        return T5_ERR_BUSY;
    }
    return T5_OK;
}

void t5_i2c_unlock(void)
{
    t5_mutex_unlock(s_bus);
}

t5_err_t t5_i2c_open(uint8_t addr, uint32_t hz, const char *name, t5_i2c_dev_t **out)
{
    if (!out || addr < 0x08 || addr > 0x77) {
        return T5_ERR_INVALID_ARG;
    }
    T5_RETURN_ON_ERR(t5_i2c_lock(LOCK_TIMEOUT_MS));
    t5_err_t err = T5_ERR_NO_MEM;
    for (int i = 0; i < T5_I2C_MAX_DEVICES; i++) {
        if (s_devs[i].used) {
            continue;
        }
        t5_i2c_hw_dev_t hw;
        err = t5_i2c_hw_add(addr, hz ? hz : 400000, &hw);
        if (err == T5_OK) {
            s_devs[i] = (struct t5_i2c_dev){ .used = true, .addr = addr, .name = name ? name : "?", .hw = hw };
            *out = &s_devs[i];
        }
        break;
    }
    t5_i2c_unlock();
    return err;
}

void t5_i2c_close(t5_i2c_dev_t *dev)
{
    if (!dev || t5_i2c_lock(LOCK_TIMEOUT_MS) != T5_OK) {
        return;
    }
    t5_i2c_hw_remove(dev->hw);
    dev->used = false;
    t5_i2c_unlock();
}

uint8_t t5_i2c_addr(const t5_i2c_dev_t *dev)
{
    return dev ? dev->addr : 0;
}

t5_err_t t5_i2c_on_recover(t5_i2c_recover_cb_t cb, void *arg)
{
    for (int i = 0; i < MAX_RECOVER_CBS; i++) {
        if (!s_recover_cbs[i].cb) {
            s_recover_cbs[i].cb = cb;
            s_recover_cbs[i].arg = arg;
            return T5_OK;
        }
    }
    return T5_ERR_NO_MEM;
}

/* Bus lock held. */
static t5_err_t recover_locked(void)
{
    if (s_in_recovery) {
        return T5_ERR_STATE;
    }
    s_in_recovery = true;
    t5_err_t err = t5_i2c_hw_recover();
    s_stats.recoveries++;
    if (err != T5_OK) {
        s_stats.recovery_failures++;
        T5_LOGE(TAG, "bus recovery failed: %s", t5_err_str(err));
    } else {
        T5_LOGW(TAG, "bus recovered (#%u)", (unsigned)s_stats.recoveries);
    }
    for (int i = 0; i < MAX_RECOVER_CBS; i++) {
        if (s_recover_cbs[i].cb) {
            s_recover_cbs[i].cb(s_recover_cbs[i].arg);
        }
    }
    t5_event_post(T5_EV_I2C_BUS_RECOVERED, NULL, 0);
    s_in_recovery = false;
    return err;
}

t5_err_t t5_i2c_recover(void)
{
    T5_RETURN_ON_ERR(t5_i2c_lock(LOCK_TIMEOUT_MS));
    t5_err_t err = recover_locked();
    t5_i2c_unlock();
    return err;
}

static void trace_add(uint8_t addr, const uint8_t *tx, size_t txlen, size_t rxlen, t5_err_t err)
{
    if (!s_trace_on) {
        return;
    }
    t5_i2c_trace_t *e = &s_trace[s_trace_head % T5_I2C_TRACE_LEN];
    e->t_ms = t5_time_ms();
    e->addr = addr;
    e->txlen = txlen > 255 ? 255 : (uint8_t)txlen;
    e->rxlen = rxlen > 255 ? 255 : (uint8_t)rxlen;
    e->first = txlen ? tx[0] : 0;
    e->err = (int8_t)err;
    s_trace_head++;
}

static void account(struct t5_i2c_dev *d, t5_err_t err)
{
    d->xfers++;
    s_stats.xfers++;
    if (err == T5_OK) {
        d->consecutive = 0;
        if (d->faulted) {
            d->faulted = false;
            T5_LOGI(TAG, "0x%02x (%s) answering again", d->addr, d->name);
            t5_ev_i2c_t ev = { d->addr, T5_OK, d->errors };
            t5_event_post(T5_EV_I2C_DEVICE_OK, &ev, sizeof ev);
        }
        return;
    }
    d->errors++;
    s_stats.errors++;
    d->consecutive++;
    d->last_err = err;
    if (err == T5_ERR_NACK) d->nacks++;
    if (err == T5_ERR_TIMEOUT) d->timeouts++;
    if (!d->faulted && d->consecutive >= T5_I2C_FAULT_THRESHOLD) {
        d->faulted = true;
        T5_LOGE(TAG, "0x%02x (%s) faulted after %u failures: %s", d->addr, d->name,
                (unsigned)d->consecutive, t5_err_str(err));
        t5_ev_i2c_t ev = { d->addr, err, d->errors };
        t5_event_post(T5_EV_I2C_DEVICE_FAULT, &ev, sizeof ev);
    }
}

t5_err_t t5_i2c_xfer(t5_i2c_dev_t *dev, const uint8_t *tx, size_t txlen, uint8_t *rx, size_t rxlen)
{
    if (!dev || !dev->used || (txlen && !tx) || (rxlen && !rx) || (!txlen && !rxlen)) {
        return T5_ERR_INVALID_ARG;
    }
    T5_RETURN_ON_ERR(t5_i2c_lock(LOCK_TIMEOUT_MS));
    uint32_t now = t5_time_ms();
    if (dev->faulted && now - dev->last_attempt_ms < T5_I2C_FAULT_RETRY_MS) {
        t5_i2c_unlock();
        return T5_ERR_IO;   /* fail fast: don't stall the bus on a dead chip */
    }
    dev->last_attempt_ms = now;
    t5_err_t err = t5_i2c_hw_xfer(dev->hw, tx, txlen, rx, rxlen, T5_I2C_TIMEOUT_MS);
    if ((err == T5_ERR_TIMEOUT || err == T5_ERR_IO) && !s_in_recovery) {
        recover_locked();
        err = t5_i2c_hw_xfer(dev->hw, tx, txlen, rx, rxlen, T5_I2C_TIMEOUT_MS);
    }
    account(dev, err);
    trace_add(dev->addr, tx, txlen, rxlen, err);
    t5_i2c_unlock();
    return err;
}

t5_err_t t5_i2c_read(t5_i2c_dev_t *dev, uint8_t reg, uint8_t *buf, size_t len)
{
    return t5_i2c_xfer(dev, &reg, 1, buf, len);
}

static t5_err_t write_prefixed(t5_i2c_dev_t *dev, const uint8_t *prefix, size_t plen,
                               const uint8_t *buf, size_t len)
{
    uint8_t stack[STACK_TX];
    uint8_t *tx = plen + len <= sizeof stack ? stack : malloc(plen + len);
    if (!tx) {
        return T5_ERR_NO_MEM;
    }
    memcpy(tx, prefix, plen);
    if (len) {
        memcpy(tx + plen, buf, len);
    }
    t5_err_t err = t5_i2c_xfer(dev, tx, plen + len, NULL, 0);
    if (tx != stack) {
        free(tx);
    }
    return err;
}

t5_err_t t5_i2c_write(t5_i2c_dev_t *dev, uint8_t reg, const uint8_t *buf, size_t len)
{
    return write_prefixed(dev, &reg, 1, buf, len);
}

t5_err_t t5_i2c_read_u8(t5_i2c_dev_t *dev, uint8_t reg, uint8_t *val)
{
    return t5_i2c_read(dev, reg, val, 1);
}

t5_err_t t5_i2c_write_u8(t5_i2c_dev_t *dev, uint8_t reg, uint8_t val)
{
    return t5_i2c_write(dev, reg, &val, 1);
}

t5_err_t t5_i2c_update_u8(t5_i2c_dev_t *dev, uint8_t reg, uint8_t mask, uint8_t val)
{
    T5_RETURN_ON_ERR(t5_i2c_lock(LOCK_TIMEOUT_MS));
    uint8_t v;
    t5_err_t err = t5_i2c_read_u8(dev, reg, &v);
    if (err == T5_OK) {
        err = t5_i2c_write_u8(dev, reg, (uint8_t)((v & ~mask) | (val & mask)));
    }
    t5_i2c_unlock();
    return err;
}

t5_err_t t5_i2c_read16(t5_i2c_dev_t *dev, uint16_t reg, uint8_t *buf, size_t len)
{
    const uint8_t r[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
    return t5_i2c_xfer(dev, r, 2, buf, len);
}

t5_err_t t5_i2c_write16(t5_i2c_dev_t *dev, uint16_t reg, const uint8_t *buf, size_t len)
{
    const uint8_t r[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
    return write_prefixed(dev, r, 2, buf, len);
}

t5_err_t t5_i2c_probe(uint8_t addr)
{
    T5_RETURN_ON_ERR(t5_i2c_lock(LOCK_TIMEOUT_MS));
    t5_err_t err = t5_i2c_hw_probe(addr, T5_I2C_TIMEOUT_MS);
    if (err == T5_ERR_TIMEOUT && !s_in_recovery) {
        recover_locked();
        err = t5_i2c_hw_probe(addr, T5_I2C_TIMEOUT_MS);
    }
    t5_i2c_unlock();
    return err;
}

void t5_i2c_bus_stats(t5_i2c_bus_stats_t *out)
{
    if (out) {
        *out = s_stats;
    }
}

int t5_i2c_dev_stats(t5_i2c_dev_stats_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < T5_I2C_MAX_DEVICES; i++) {
        struct t5_i2c_dev *d = &s_devs[i];
        if (!d->used) {
            continue;
        }
        if (out && n < max) {
            out[n] = (t5_i2c_dev_stats_t){
                .addr = d->addr, .name = d->name, .xfers = d->xfers, .errors = d->errors,
                .nacks = d->nacks, .timeouts = d->timeouts, .consecutive = d->consecutive,
                .faulted = d->faulted, .last_err = d->last_err,
            };
        }
        n++;
    }
    return n;
}

void t5_i2c_clear_fault(t5_i2c_dev_t *dev)
{
    if (dev) {
        dev->faulted = false;
        dev->consecutive = 0;
    }
}

void t5_i2c_trace_enable(bool on)
{
    s_trace_on = on;
}

int t5_i2c_trace_read(t5_i2c_trace_t *out, int max)
{
    uint32_t head = s_trace_head;
    uint32_t avail = head < T5_I2C_TRACE_LEN ? head : T5_I2C_TRACE_LEN;
    uint32_t n = avail < (uint32_t)max ? avail : (uint32_t)max;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = s_trace[(head - n + i) % T5_I2C_TRACE_LEN];
    }
    return (int)n;
}

/* Host tests only: forget all state. */
void t5_i2c_reset_for_test(void)
{
    memset(s_devs, 0, sizeof s_devs);
    memset(&s_stats, 0, sizeof s_stats);
    memset(s_recover_cbs, 0, sizeof s_recover_cbs);
    s_trace_head = 0;
    s_trace_on = false;
    s_ready = false;
    s_in_recovery = false;
}
