/* SPDX-License-Identifier: Apache-2.0
 * Simulated board chips shared by several tests.
 */
#include <string.h>
#include "t5_mocks.h"
#include "t5_bsp/xpin.h"
#include "t5_time/rtc.h"

void mock_expanders_start(void)
{
    mock_i2c_dev_t *a = mock_i2c_add(0x43, false);
    mock_i2c_dev_t *b = mock_i2c_add(0x44, false);
    a->regs[0x01] = 0xA0;
    b->regs[0x01] = 0xA0;
    t5_xpin_reset_for_test();
    t5_xpin_init();
}

/* 0x1D is write-0-to-clear. */
static bool rtc_write(mock_i2c_dev_t *d, uint32_t reg, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint32_t r = reg + i;
        if (r == 0x1D) d->regs[r] &= data[i];
        else d->regs[r] = data[i];
    }
    return true;
}

mock_i2c_dev_t *mock_rtc_start(uint8_t boot_flags)
{
    mock_i2c_dev_t *r = mock_i2c_add(0x32, false);
    r->on_write = rtc_write;
    r->regs[0x1D] = boot_flags;
    struct tm t = { .tm_year = 126, .tm_mon = 10, .tm_mday = 5, .tm_hour = 12, .tm_wday = 4 };
    t5_rtc_encode(&t, &r->regs[0x10]);
    t5_rtc_reset_for_test();
    t5_rtc_init();
    return r;
}

mock_i2c_dev_t *mock_ina226_add(void)
{
    mock_i2c_dev_t *d = mock_i2c_add(0x41, false);
    /* 16-bit big-endian registers laid out as two bytes per register index. */
    return d;
}

static bool ina_read(mock_i2c_dev_t *d, uint32_t reg, uint8_t *out, size_t len)
{
    uint16_t v = 0;
    switch (reg) {
    case 0x00: v = (uint16_t)(d->regs[0x100] << 8 | d->regs[0x101]); break;
    case 0x01: v = (uint16_t)(d->regs[0x102] << 8 | d->regs[0x103]); break;
    case 0x02: v = (uint16_t)(d->regs[0x104] << 8 | d->regs[0x105]); break;
    case 0xFE: v = 0x5449; break;
    case 0xFF: v = 0x2260; break;
    default: break;
    }
    if (len >= 1) out[0] = (uint8_t)(v >> 8);
    if (len >= 2) out[1] = (uint8_t)v;
    return true;
}

static bool ina_write(mock_i2c_dev_t *d, uint32_t reg, const uint8_t *data, size_t len)
{
    if (reg == 0x00 && len >= 2) {
        d->regs[0x100] = data[0];
        d->regs[0x101] = data[1];
    }
    return true;
}

void mock_ina226_set(mock_i2c_dev_t *d, uint16_t mv, int32_t ma10)
{
    d->on_read = ina_read;
    d->on_write = ina_write;
    uint16_t bus = (uint16_t)((uint32_t)mv * 100 / 125);
    int16_t sh = (int16_t)(ma10 / 5);
    d->regs[0x102] = (uint8_t)((uint16_t)sh >> 8);
    d->regs[0x103] = (uint8_t)sh;
    d->regs[0x104] = (uint8_t)(bus >> 8);
    d->regs[0x105] = (uint8_t)bus;
}
