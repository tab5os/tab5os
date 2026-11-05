/* SPDX-License-Identifier: Apache-2.0
 * Simulated system I2C bus: chips are register files with optional hooks.
 */
#include <stdlib.h>
#include <string.h>
#include "t5_i2c_hw.h"
#include "t5_mocks.h"

#define MAX_DEVS 16
static mock_i2c_dev_t *s_devs[MAX_DEVS];
static int s_recovers;

struct t5_i2c_hw_dev { uint8_t addr; };
static struct t5_i2c_hw_dev s_handles[32];

mock_i2c_dev_t *mock_i2c_add(uint8_t addr, bool reg16)
{
    for (int i = 0; i < MAX_DEVS; i++) {
        if (!s_devs[i]) {
            s_devs[i] = calloc(1, sizeof(mock_i2c_dev_t));
            s_devs[i]->addr = addr;
            s_devs[i]->present = true;
            s_devs[i]->reg16 = reg16;
            return s_devs[i];
        }
    }
    return NULL;
}

mock_i2c_dev_t *mock_i2c_get(uint8_t addr)
{
    for (int i = 0; i < MAX_DEVS; i++) {
        if (s_devs[i] && s_devs[i]->addr == addr) return s_devs[i];
    }
    return NULL;
}

void mock_i2c_reset(void)
{
    for (int i = 0; i < MAX_DEVS; i++) { free(s_devs[i]); s_devs[i] = NULL; }
    s_recovers = 0;
}

void mock_i2c_fail_next(uint8_t addr, int count, t5_err_t err)
{
    mock_i2c_dev_t *d = mock_i2c_get(addr);
    if (d) { d->fail_count = count; d->fail_err = err; }
}

int mock_i2c_recover_count(void) { return s_recovers; }

t5_err_t t5_i2c_hw_init(void) { return T5_OK; }

t5_err_t t5_i2c_hw_add(uint8_t addr, uint32_t hz, t5_i2c_hw_dev_t *out)
{
    (void)hz;
    for (int i = 0; i < 32; i++) {
        if (!s_handles[i].addr) {
            s_handles[i].addr = addr;
            *out = &s_handles[i];
            return T5_OK;
        }
    }
    return T5_ERR_NO_MEM;
}

void t5_i2c_hw_remove(t5_i2c_hw_dev_t dev) { dev->addr = 0; }

t5_err_t t5_i2c_hw_xfer(t5_i2c_hw_dev_t h, const uint8_t *tx, size_t txlen, uint8_t *rx, size_t rxlen,
                        uint32_t timeout_ms)
{
    (void)timeout_ms;
    mock_i2c_dev_t *d = mock_i2c_get(h->addr);
    if (!d || !d->present) return T5_ERR_NACK;
    if (d->fail_count > 0) { d->fail_count--; return d->fail_err; }
    size_t alen = d->reg16 ? 2 : 1;
    if (txlen < alen) return T5_ERR_IO;
    uint32_t reg = d->reg16 ? ((uint32_t)tx[0] << 8 | tx[1]) : tx[0];
    size_t dlen = txlen - alen;
    if (dlen) {
        d->writes++;
        if (dlen == 1 && d->wlog_n < 256) d->wlog[d->wlog_n++] = (reg << 8) | tx[alen];
        if (!d->on_write || !d->on_write(d, reg, tx + alen, dlen)) {
            for (size_t i = 0; i < dlen; i++) d->regs[(reg + i) & 0xFFFF] = tx[alen + i];
        }
    }
    if (rxlen) {
        d->reads++;
        if (!d->on_read || !d->on_read(d, reg, rx, rxlen)) {
            for (size_t i = 0; i < rxlen; i++) rx[i] = d->regs[(reg + i) & 0xFFFF];
        }
    }
    return T5_OK;
}

t5_err_t t5_i2c_hw_probe(uint8_t addr, uint32_t timeout_ms)
{
    (void)timeout_ms;
    mock_i2c_dev_t *d = mock_i2c_get(addr);
    return d && d->present ? T5_OK : T5_ERR_NACK;
}

t5_err_t t5_i2c_hw_recover(void)
{
    s_recovers++;
    return T5_OK;
}

void mock_i2c_handles_reset(void) { memset(s_handles, 0, sizeof s_handles); }
