/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Name of a known device at a 7-bit address on the system bus, or NULL. */
const char *t5_bsp_i2c_name(uint8_t addr);

/* Signal name of an expander pin (addr 0x43/0x44, pin 0-7), or NULL. */
const char *t5_bsp_xpin_name(uint8_t addr, uint8_t pin);

#ifdef __cplusplus
}
#endif
