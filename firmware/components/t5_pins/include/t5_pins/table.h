/* SPDX-License-Identifier: Apache-2.0
 * The board table: every chip on the system I2C bus and every named
 * expander pin. The I2C broker's reserved list, the console's scan and
 * expander views and the tests all read this one table.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t addr;          /* 7-bit */
    const char *name;
    bool reserved;         /* the port broker never gives apps a handle to it */
} t5_pins_chip_t;

typedef struct {
    uint16_t xpin;         /* T5_XP_* */
    const char *name;      /* schematic net name */
} t5_pins_xpin_t;

extern const t5_pins_chip_t t5_pins_chips[];
extern const size_t t5_pins_chip_count;
extern const t5_pins_xpin_t t5_pins_xpins[];
extern const size_t t5_pins_xpin_count;

/* Enable pin (T5_XP_*) of each switched rail, in t5_rail_t order
 * (t5_power/rail.h): USB5V, EXT5V, WLAN, SPK. */
#define T5_PINS_RAIL_COUNT 4
extern const uint16_t t5_pins_rail_en[T5_PINS_RAIL_COUNT];

/* The chip at addr on the system bus, or NULL. */
const t5_pins_chip_t *t5_pins_chip(uint8_t addr);
/* Name of a known device at addr on the system bus, or NULL. */
const char *t5_pins_i2c_name(uint8_t addr);
/* Signal name of expander pin (addr, pin), or NULL. */
const char *t5_pins_xpin_name(uint8_t addr, uint8_t pin);

#ifdef __cplusplus
}
#endif
