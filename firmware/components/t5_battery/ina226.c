/* SPDX-License-Identifier: Apache-2.0 */
#include "t5_battery/battery_test.h"
#include "t5_battery/ina226.h"
#include "t5_core/log.h"
#include "t5_i2c/t5_i2c.h"
#include "t5_pins/pins.h"

static const char *TAG = "t5.ina226";

enum {
    REG_CONFIG = 0x00,
    REG_SHUNT = 0x01,
    REG_BUS = 0x02,
    REG_MANUF = 0xFE,
    REG_DIE = 0xFF,
};

static t5_i2c_dev_t *s_dev;
static bool s_ready;

uint16_t t5_ina226_bus_to_mv(uint16_t raw)
{
    return (uint16_t)(((uint32_t)raw * 125 + 50) / 100);
}

int32_t t5_ina226_shunt_to_ma10(int16_t raw)
{
    /* 2.5 uV / 5 mOhm = 0.5 mA = 5 x 0.1 mA */
    return (int32_t)raw * 25000 / T5_INA226_SHUNT_UOHM;
}

static t5_err_t rd16(uint8_t reg, uint16_t *v)
{
    uint8_t b[2];
    T5_RETURN_ON_ERR(t5_i2c_read(s_dev, reg, b, 2));
    *v = (uint16_t)((b[0] << 8) | b[1]);
    return T5_OK;
}

static t5_err_t wr16(uint8_t reg, uint16_t v)
{
    const uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    return t5_i2c_write(s_dev, reg, b, 2);
}

t5_err_t t5_ina226_init(void)
{
    if (!s_dev) {
        T5_RETURN_ON_ERR(t5_i2c_open(T5_I2C_ADDR_INA226, T5_SYS_I2C_HZ, "ina226", &s_dev));
    }
    uint16_t manuf = 0, die = 0;
    T5_RETURN_ON_ERR(rd16(REG_MANUF, &manuf));
    T5_RETURN_ON_ERR(rd16(REG_DIE, &die));
    if (manuf != T5_INA226_MANUF_ID || (die & 0xFFF0) != T5_INA226_DIE_ID) {
        T5_LOGE(TAG, "unexpected IDs manuf 0x%04x die 0x%04x", manuf, die);
        return T5_ERR_NOT_FOUND;
    }
    T5_RETURN_ON_ERR(wr16(REG_CONFIG, T5_INA226_CONFIG));
    uint16_t cfg = 0;
    T5_RETURN_ON_ERR(rd16(REG_CONFIG, &cfg));
    if (cfg != T5_INA226_CONFIG) {
        T5_LOGE(TAG, "config readback 0x%04x", cfg);
        return T5_ERR_IO;
    }
    s_ready = true;
    return T5_OK;
}

bool t5_ina226_ready(void)
{
    return s_ready;
}

t5_err_t t5_ina226_read(t5_ina226_sample_t *out)
{
    if (!s_ready) return T5_ERR_STATE;
    uint16_t sh, bus;
    T5_RETURN_ON_ERR(rd16(REG_SHUNT, &sh));
    T5_RETURN_ON_ERR(rd16(REG_BUS, &bus));
    out->mv = t5_ina226_bus_to_mv(bus);
    out->ma10 = t5_ina226_shunt_to_ma10((int16_t)sh);
    out->mw = (int32_t)((int64_t)out->mv * out->ma10 / 10000);
    return T5_OK;
}

void t5_ina226_reset_for_test(void)
{
    s_dev = NULL;
    s_ready = false;
}
