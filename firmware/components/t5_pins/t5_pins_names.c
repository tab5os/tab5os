/* SPDX-License-Identifier: Apache-2.0
 * Device and pin names. Pure C with no IDF dependencies, so host tests build it.
 */
#include <stddef.h>
#include <stdint.h>
#include "t5_pins/pins.h"
#include "t5_pins/names.h"

const char *t5_bsp_i2c_name(uint8_t addr)
{
    switch (addr) {
    case T5_I2C_ADDR_ES8388: return "ES8388 audio DAC";
    case T5_I2C_ADDR_GT911:  return "GT911 touch (ILI9881C panel)";
    case T5_I2C_ADDR_GT911_ALT:               return "GT911 touch, alt address";
    case T5_I2C_ADDR_UNKNOWN28: return "unidentified, ignores register address";
    case T5_I2C_ADDR_RX8130: return "RX8130CE RTC";
    case T5_I2C_ADDR_SC2356: return "SC2356 camera? (unconfirmed)";
    case T5_I2C_ADDR_ES7210: return "ES7210 mic ADC";
    case T5_I2C_ADDR_INA226: return "INA226 battery monitor";
    case T5_I2C_ADDR_EXP0:   return "PI4IOE5V6408 expander EXP0";
    case T5_I2C_ADDR_EXP1:   return "PI4IOE5V6408 expander EXP1";
    case T5_I2C_ADDR_ST712X: return "ST7123/ST7121 touch";
    case T5_I2C_ADDR_BMI270: return "BMI270 IMU";
    case 0x69:               return "BMI270 IMU, alt address";
    default:                 return NULL;
    }
}

const char *t5_bsp_xpin_name(uint8_t addr, uint8_t pin)
{
    static const char *const exp0[8] = {
        "RF_ANT_SEL", "SPK_EN", "EXT5V_EN", NULL,
        "LCD_RST", "TP_RST", "CAM_RST", "HP_DET",
    };
    static const char *const exp1[8] = {
        "WLAN_PWR_EN", NULL, NULL, "USB5V_EN",
        "PWROFF_PULSE?", "nCHG_QC_EN", "CHG_STAT", "CHG_EN",
    };
    if (pin > 7) {
        return NULL;
    }
    if (addr == T5_I2C_ADDR_EXP0) {
        return exp0[pin];
    }
    if (addr == T5_I2C_ADDR_EXP1) {
        return exp1[pin];
    }
    return NULL;
}
