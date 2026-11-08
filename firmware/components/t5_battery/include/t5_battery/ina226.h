/* SPDX-License-Identifier: Apache-2.0
 * INA226 on the battery rail (0x41), 5 mOhm shunt (plan 2.8).
 *
 * Current comes straight from the shunt-voltage register (2.5 uV = 0.5 mA
 * per LSB through 5 mOhm), so no calibration register is involved. Sign:
 * positive = discharging, as the design defines it. The chip reads charge
 * current as negative shunt voltage (M5Unified inverts it for its API).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_INA226_SHUNT_UOHM 5000

typedef struct {
    uint16_t mv;          /* bus voltage */
    int32_t ma10;         /* current in 0.1 mA, + discharging */
    int32_t mw;           /* mv * ma / 1000 */
} t5_ina226_sample_t;

/* Open, check the IDs, set 16x averaging and 1.1 ms conversions (~35 ms per
 * result), continuous shunt and bus. */
T5_MUST_CHECK t5_err_t t5_ina226_init(void);
bool t5_ina226_ready(void);
T5_MUST_CHECK t5_err_t t5_ina226_read(t5_ina226_sample_t *out);

/* Pure conversions (host-tested). */
uint16_t t5_ina226_bus_to_mv(uint16_t raw);          /* 1.25 mV/LSB */
int32_t t5_ina226_shunt_to_ma10(int16_t raw);        /* 2.5 uV/LSB through the shunt */

#define T5_INA226_CONFIG 0x4527   /* AVG 16, VBUSCT 1.1 ms, VSHCT 1.1 ms, shunt+bus continuous */

#ifdef __cplusplus
}
#endif
