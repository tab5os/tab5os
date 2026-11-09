/* SPDX-License-Identifier: Apache-2.0
 * Pure C with no IDF dependencies, so host tests build it.
 */
#include "t5_pins/table.h"
#include "t5_pins/pins.h"

const t5_pins_chip_t t5_pins_chips[] = {
    { T5_I2C_ADDR_ES8388, "ES8388 audio DAC", true },
    { T5_I2C_ADDR_GT911, "GT911 touch (ILI9881C panel)", true },
    { T5_I2C_ADDR_UNKNOWN28, "unidentified, ignores register address", true },
    { T5_I2C_ADDR_RX8130, "RX8130CE RTC", true },
    { T5_I2C_ADDR_SC2356, "SC2356 camera? (unconfirmed)", true },
    { T5_I2C_ADDR_ES7210, "ES7210 mic ADC", true },
    { T5_I2C_ADDR_INA226, "INA226 battery monitor", true },
    { T5_I2C_ADDR_EXP0, "PI4IOE5V6408 expander EXP0", true },
    { T5_I2C_ADDR_EXP1, "PI4IOE5V6408 expander EXP1", true },
    { T5_I2C_ADDR_ST712X, "ST7123/ST7121 touch", true },
    { T5_I2C_ADDR_GT911_ALT, "GT911 touch, alt address", true },
    { T5_I2C_ADDR_BMI270, "BMI270 IMU", true },
    { T5_I2C_ADDR_BMI270_ALT, "BMI270 IMU, alt address", true },
};
const size_t t5_pins_chip_count = sizeof t5_pins_chips / sizeof t5_pins_chips[0];

const t5_pins_xpin_t t5_pins_xpins[] = {
    { T5_XP_RF_ANT_SEL, "RF_ANT_SEL" },
    { T5_XP_SPK_EN, "SPK_EN" },
    { T5_XP_EXT5V_EN, "EXT5V_EN" },
    { T5_XP_LCD_RST, "LCD_RST" },
    { T5_XP_TP_RST, "TP_RST" },
    { T5_XP_CAM_RST, "CAM_RST" },
    { T5_XP_HP_DET, "HP_DET" },
    { T5_XP_WLAN_PWR_EN, "WLAN_PWR_EN" },
    { T5_XP_USB5V_EN, "USB5V_EN" },
    { T5_XP_PWROFF_PULSE, "PWROFF_PULSE?" },
    { T5_XP_NCHG_QC_EN, "nCHG_QC_EN" },
    { T5_XP_CHG_STAT, "CHG_STAT" },
    { T5_XP_CHG_EN, "CHG_EN" },
};
const size_t t5_pins_xpin_count = sizeof t5_pins_xpins / sizeof t5_pins_xpins[0];

const uint16_t t5_pins_rail_en[T5_PINS_RAIL_COUNT] = {
    T5_XP_USB5V_EN,
    T5_XP_EXT5V_EN,
    T5_XP_WLAN_PWR_EN,
    T5_XP_SPK_EN,
};

const t5_pins_chip_t *t5_pins_chip(uint8_t addr)
{
    for (size_t i = 0; i < t5_pins_chip_count; i++) {
        if (t5_pins_chips[i].addr == addr) {
            return &t5_pins_chips[i];
        }
    }
    return NULL;
}

const char *t5_pins_i2c_name(uint8_t addr)
{
    const t5_pins_chip_t *c = t5_pins_chip(addr);
    return c ? c->name : NULL;
}

const char *t5_pins_xpin_name(uint8_t addr, uint8_t pin)
{
    if (pin > 7) {
        return NULL;
    }
    for (size_t i = 0; i < t5_pins_xpin_count; i++) {
        if (t5_pins_xpins[i].xpin == T5_XPIN(addr, pin)) {
            return t5_pins_xpins[i].name;
        }
    }
    return NULL;
}
