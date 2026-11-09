/*
 * Tab5 pin map. The only file in the OS that knows raw pin numbers.
 *
 * Status tags on each group:
 *   [bsp]      matches espressif/esp-bsp m5stack_tab5 1.3.2
 *   [docs]     from the M5Stack Tab5 docs pin tables
 *   [sch]      from the schematic netlist only
 *   [verified] confirmed on Unit A with the bring-up console (see bench-results.md)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

/* ---- Internal I2C bus (system bus) [bsp] -------------------------------- */
#define T5_PIN_SYS_I2C_SDA      31
#define T5_PIN_SYS_I2C_SCL      32
#define T5_SYS_I2C_HZ           400000  /* every chip on the system bus runs at this */

/* Internal I2C addresses (7-bit). Names and the reserved list: t5_pins/table.h.
 * Unit A scan (2026-11-05): 0x10 0x28 0x32 0x40 0x41 0x43 0x44 0x55 0x68 answer.
 * GT911 (0x14) is absent (ST7123 panel); the camera (0x36) answers once CAM_RST is released.
 */
#define T5_I2C_ADDR_ES8388      0x10    /* audio DAC                [verified] */
#define T5_I2C_ADDR_GT911       0x14    /* touch, ILI9881C variant  [docs] */
#define T5_I2C_ADDR_UNKNOWN28   0x28    /* ST7123 second address: gone when TP_RST or LCD_RST is low [verified] */
#define T5_I2C_ADDR_GT911_ALT   0x5D    /* GT911 alternate address [bsp] */
#define T5_I2C_ADDR_RX8130      0x32    /* RTC                      [verified] */
#define T5_I2C_ADDR_SC2356      0x36    /* camera SCCB, answers once CAM_RST is released [verified] */
#define T5_I2C_ADDR_ES7210      0x40    /* mic ADC                  [verified] */
#define T5_I2C_ADDR_INA226      0x41    /* battery monitor          [verified] */
#define T5_I2C_ADDR_EXP0        0x43    /* PI4IOE5V6408, ADDR low   [verified] */
#define T5_I2C_ADDR_EXP1        0x44    /* PI4IOE5V6408, ADDR high  [verified] */
#define T5_I2C_ADDR_ST712X      0x55    /* touch, ST7123/ST7121     [verified] */
#define T5_I2C_ADDR_BMI270      0x68    /* IMU                      [verified] */
#define T5_I2C_ADDR_BMI270_ALT  0x69    /* BMI270 with SDO high (Bosch datasheet) */

/* I/O expander pins, encoded as (address << 8) | pin so one value names an
 * expander pin. The design doc's "E1.P0" (antenna) is EXP0.P0. Pin names match
 * the schematic and M5Unified's Tab5 table. Rail and charger pins are named,
 * not yet verified by effect (bench-results.md, 0.4). */
#define T5_XPIN(addr, pin)      (((addr) << 8) | (pin))
#define T5_XPIN_ADDR(x)         ((uint8_t)((x) >> 8))
#define T5_XPIN_NUM(x)          ((uint8_t)((x) & 0x7))

#define T5_XP_RF_ANT_SEL        T5_XPIN(T5_I2C_ADDR_EXP0, 0) /* internal / MMCX antenna    [docs] */
#define T5_XP_SPK_EN            T5_XPIN(T5_I2C_ADDR_EXP0, 1) /* speaker amp                [bsp]  */
#define T5_XP_EXT5V_EN          T5_XPIN(T5_I2C_ADDR_EXP0, 2) /* Grove, M5-Bus, ext 5V      [docs] */
#define T5_XP_LCD_RST           T5_XPIN(T5_I2C_ADDR_EXP0, 4) /* BSP calls it LCD_EN        [verified] */
#define T5_XP_TP_RST            T5_XPIN(T5_I2C_ADDR_EXP0, 5) /* touch reset                [verified] */
#define T5_XP_CAM_RST           T5_XPIN(T5_I2C_ADDR_EXP0, 6) /* BSP calls it CAMERA_EN     [verified] */
#define T5_XP_HP_DET            T5_XPIN(T5_I2C_ADDR_EXP0, 7) /* headphone detect, input    [docs] */

#define T5_XP_WLAN_PWR_EN       T5_XPIN(T5_I2C_ADDR_EXP1, 0) /* C6 power                   [bsp]  */
#define T5_XP_USB5V_EN          T5_XPIN(T5_I2C_ADDR_EXP1, 3) /* USB-A 5V                   [bsp]  */
#define T5_XP_PWROFF_PULSE      T5_XPIN(T5_I2C_ADDR_EXP1, 4) /* PWROFF_PLUSE: U7 pin 6 [sch]. Driving it high cuts power */
#define T5_XP_NCHG_QC_EN        T5_XPIN(T5_I2C_ADDR_EXP1, 5) /* fast charge, active low    [docs] */
#define T5_XP_CHG_STAT          T5_XPIN(T5_I2C_ADDR_EXP1, 6) /* charger status, input      [docs] */
#define T5_XP_CHG_EN            T5_XPIN(T5_I2C_ADDR_EXP1, 7) /* charger enable             [docs] */

/* ---- Display and touch [bsp] -------------------------------------------- */
#define T5_PIN_LCD_BACKLIGHT    22      /* [verified] */
#define T5_PIN_TOUCH_INT        23      /* falling edge on touch [verified] */

/* ---- Audio, I2S [bsp] --------------------------------------------------- */
#define T5_PIN_I2S_MCLK         30
#define T5_PIN_I2S_SCLK         27
#define T5_PIN_I2S_LRCK         29
#define T5_PIN_I2S_DOUT         26
#define T5_PIN_I2S_DIN          28

/* microSD, 4-bit SDIO on SDMMC slot 0's IO MUX pins 39-44 (D0-D3, CLK, CMD),
 * which the driver selects itself [bsp]. No card-detect GPIO (bench check 9). */

/* ---- System lines [docs] ------------------------------------------------ */
#define T5_PIN_PWR_KEY          35      /* strap, power key, shared with PMS150G. Keep its pad config in sleep */
#define T5_PIN_C6_WAKE          14      /* C6 IO2 -> P4, bench check 12 */

/* ---- External ports [docs], [sch] for ext and stamp ----------------------
 * Only t5_ports maps these to named ports and modes (ports_table.c).
 */
#define T5_PIN_GROVE_A          53      /* white, I2C SDA (M5Unified Port A) [docs] */
#define T5_PIN_GROVE_B          54      /* yellow, I2C SCL                   [docs] */
#define T5_PIN_RS485_TX         20
#define T5_PIN_RS485_RX         21
#define T5_PIN_RS485_DIR        34
#define T5_PIN_MBUS_SPI_MOSI    18
#define T5_PIN_MBUS_SPI_MISO    19
#define T5_PIN_MBUS_SPI_SCK     5
#define T5_PIN_MBUS_UART_RX     38
#define T5_PIN_MBUS_UART_TX     37
#define T5_PIN_MBUS_DEBUG_RX    7
#define T5_PIN_MBUS_DEBUG_TX    6
#define T5_PIN_STAMP_CS         33
#define T5_PIN_STAMP_IO         46

/* M5-Bus free GPIOs (mbus.gpio) [docs]. G35 is T5_PIN_PWR_KEY: input only */
#define T5_PIN_MBUS_G2          2
#define T5_PIN_MBUS_G3          3
#define T5_PIN_MBUS_G4          4
#define T5_PIN_MBUS_G16         16      /* ADC1 ch0 */
#define T5_PIN_MBUS_G17         17      /* ADC1 ch1 */
#define T5_PIN_MBUS_G35         T5_PIN_PWR_KEY
#define T5_PIN_MBUS_G45         45
#define T5_PIN_MBUS_G47         47
#define T5_PIN_MBUS_G48         48
#define T5_PIN_MBUS_G51         51      /* ADC2 ch2 */
#define T5_PIN_MBUS_G52         52      /* ADC2 ch3 */

/* ext 2.54 mm 10-pin header [sch], unmetered (bench check 10) */
#define T5_PIN_EXT_G0           0
#define T5_PIN_EXT_G1           1
#define T5_PIN_EXT_G49          49      /* ADC2 ch0 */
#define T5_PIN_EXT_G50          50      /* ADC2 ch1 */
