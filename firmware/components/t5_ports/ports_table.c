/* SPDX-License-Identifier: Apache-2.0
 * The named port table. Pin numbers come from t5_pins/pins.h only.
 *
 * Role assignments (which pin is SDA, TX...) for grove, ext and stamp are the
 * OS's choice where the docs don't fix them; the P4 GPIO matrix can route any
 * signal to any of these pins. ext and stamp are from the schematic netlist
 * and still need a meter (bench check 10).
 */
#include <string.h>
#include "t5_pins/pins.h"
#include "ports_priv.h"

#define NONE2 { -1, -1 }
#define NONE3 { -1, -1, -1 }
/* General-purpose pins: GPIO and PWM, and ADC on the ones with a channel
 * (ports_modes). */
#define GPWM (PM(T5_MODE_GPIO) | PM(T5_MODE_PWM))

#define SINGLE(n, pin)                                                                            \
    { .name = n, .desc = "M5-Bus GPIO", .npins = 1, .gpio = { pin }, .label = { "IO" },          \
      .modes = GPWM, .i2c = NONE2, .uart = NONE3, .spi = NONE3 }

static const port_def_t s_ports[] = {
    {
        .name = "grove", .desc = "Grove HY2.0-4P, switched 5V",
        .npins = 2, .gpio = { T5_PIN_GROVE_A, T5_PIN_GROVE_B }, .label = { "SDA/TX", "SCL/RX" },
        .modes = PM(T5_MODE_I2C) | PM(T5_MODE_UART) | GPWM,
        .i2c = { 0, 1 }, .uart = { 0, 1, -1 }, .spi = NONE3,
    },
    {
        .name = "mbus.spi", .desc = "M5-Bus SPI (shares pins with stamp)",
        .npins = 3, .gpio = { T5_PIN_MBUS_SPI_MOSI, T5_PIN_MBUS_SPI_MISO, T5_PIN_MBUS_SPI_SCK },
        .label = { "MOSI", "MISO", "SCK" },
        .modes = PM(T5_MODE_SPI), .i2c = NONE2, .uart = NONE3, .spi = { 2, 0, 1 },
    },
    {
        .name = "mbus.uart", .desc = "M5-Bus UART (strapping pins)",
        .npins = 2, .gpio = { T5_PIN_MBUS_UART_RX, T5_PIN_MBUS_UART_TX }, .label = { "RX", "TX" },
        .modes = PM(T5_MODE_UART), .i2c = NONE2, .uart = { 1, 0, -1 }, .spi = NONE3,
    },
    {
        .name = "mbus.debug", .desc = "M5-Bus debug UART",
        .npins = 2, .gpio = { T5_PIN_MBUS_DEBUG_RX, T5_PIN_MBUS_DEBUG_TX }, .label = { "RX", "TX" },
        .modes = PM(T5_MODE_UART), .i2c = NONE2, .uart = { 1, 0, -1 }, .spi = NONE3,
    },
    {
        .name = "mbus.i2c", .desc = "M5-Bus I2C = system bus, shared",
        .npins = 2, .gpio = { T5_PIN_SYS_I2C_SDA, T5_PIN_SYS_I2C_SCL }, .label = { "SDA", "SCL" },
        .modes = PM(T5_MODE_I2C), .i2c = { 0, 1 }, .uart = NONE3, .spi = NONE3, .shared = true,
    },
    SINGLE("mbus.g2", T5_PIN_MBUS_G2),
    SINGLE("mbus.g3", T5_PIN_MBUS_G3),
    SINGLE("mbus.g4", T5_PIN_MBUS_G4),
    SINGLE("mbus.g16", T5_PIN_MBUS_G16),
    SINGLE("mbus.g17", T5_PIN_MBUS_G17),
    {
        .name = "mbus.g35", .desc = "M5-Bus G35 = power key, input only",
        .npins = 1, .gpio = { T5_PIN_MBUS_G35 }, .label = { "PWR" },
        .modes = PM(T5_MODE_GPIO), .i2c = NONE2, .uart = NONE3, .spi = NONE3,
    },
    SINGLE("mbus.g45", T5_PIN_MBUS_G45),
    SINGLE("mbus.g47", T5_PIN_MBUS_G47),
    SINGLE("mbus.g48", T5_PIN_MBUS_G48),
    SINGLE("mbus.g51", T5_PIN_MBUS_G51),
    SINGLE("mbus.g52", T5_PIN_MBUS_G52),
    {
        .name = "ext", .desc = "ext 2.54 mm 10-pin",
        .npins = 4, .gpio = { T5_PIN_EXT_G0, T5_PIN_EXT_G1, T5_PIN_EXT_G49, T5_PIN_EXT_G50 },
        .label = { "TX", "RX", "SDA", "SCL" },
        .modes = PM(T5_MODE_I2C) | PM(T5_MODE_UART) | GPWM,
        .i2c = { 2, 3 }, .uart = { 0, 1, -1 }, .spi = NONE3,
    },
    {
        .name = "rs485", .desc = "RS-485 HC1.25-6P, 6-24V",
        .npins = 3, .gpio = { T5_PIN_RS485_TX, T5_PIN_RS485_RX, T5_PIN_RS485_DIR },
        .label = { "TX", "RX", "DIR" },
        .modes = PM(T5_MODE_RS485), .i2c = NONE2, .uart = { 0, 1, 2 }, .spi = NONE3,
    },
    {
        .name = "stamp", .desc = "Stamp pads (shares pins with mbus.spi)",
        .npins = 6,
        .gpio = { T5_PIN_STAMP_CS, T5_PIN_MBUS_SPI_SCK, T5_PIN_MBUS_SPI_MOSI, T5_PIN_MBUS_SPI_MISO,
                  T5_PIN_MBUS_DEBUG_TX, T5_PIN_STAMP_IO },
        .label = { "CS", "SCK", "MOSI", "MISO", "TX", "RX" },
        .modes = PM(T5_MODE_SPI) | PM(T5_MODE_UART) | PM(T5_MODE_GPIO),
        .i2c = NONE2, .uart = { 4, 5, -1 }, .spi = { 1, 2, 3 },
    },
};

#define N_PORTS ((int)(sizeof s_ports / sizeof s_ports[0]))

int ports_table_count(void)
{
    return N_PORTS;
}

const port_def_t *ports_table_get(int i)
{
    return (i >= 0 && i < N_PORTS) ? &s_ports[i] : NULL;
}

const port_def_t *ports_table_find(const char *name)
{
    if (!name) {
        return NULL;
    }
    for (int i = 0; i < N_PORTS; i++) {
        if (!strcmp(s_ports[i].name, name)) {
            return &s_ports[i];
        }
    }
    return NULL;
}

uint32_t ports_modes(const port_def_t *d)
{
    uint32_t m = d->modes;
    if ((m & GPWM) == GPWM) {
        for (int i = 0; i < d->npins; i++) {
            if (ports_pin_adc(d->gpio[i], NULL, NULL)) m |= PM(T5_MODE_ADC);
        }
    }
    return m;
}

/* Pins with another job on the board. hands_off: the power key line (also
 * the sleep wake line) is input only, and its pad config is left exactly as
 * the board set it: never configured, never reset. */
static const struct {
    uint8_t gpio;
    const char *risk;
    bool hands_off;
} s_pin_rules[] = {
    { T5_PIN_PWR_KEY, "the power key and a boot strap", true },
    { T5_PIN_MBUS_UART_TX, "a boot strapping pin", false },
    { T5_PIN_MBUS_UART_RX, "a boot strapping pin", false },
};
#define N_RULES (sizeof s_pin_rules / sizeof s_pin_rules[0])

const char *ports_pin_risk(uint8_t gpio)
{
    for (size_t i = 0; i < N_RULES; i++) {
        if (s_pin_rules[i].gpio == gpio) return s_pin_rules[i].risk;
    }
    return NULL;
}

bool ports_pin_risky(uint8_t gpio)
{
    return ports_pin_risk(gpio) != NULL;
}

bool ports_pin_hands_off(uint8_t gpio)
{
    for (size_t i = 0; i < N_RULES; i++) {
        if (s_pin_rules[i].gpio == gpio) return s_pin_rules[i].hands_off;
    }
    return false;
}

/* ESP32-P4 (soc/esp32p4/include/soc/adc_channel.h): ADC1 = GPIO16..23 ch0..7,
 * ADC2 = GPIO49..54 ch0..5. */
bool ports_pin_adc(uint8_t gpio, int *unit, int *channel)
{
    int u, c;
    if (gpio >= 16 && gpio <= 23) {
        u = 1;
        c = gpio - 16;
    } else if (gpio >= 49 && gpio <= 54) {
        u = 2;
        c = gpio - 49;
    } else {
        return false;
    }
    if (unit) *unit = u;
    if (channel) *channel = c;
    return true;
}
