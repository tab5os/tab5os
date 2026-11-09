/* SPDX-License-Identifier: Apache-2.0
 * Host tests only.
 */
#pragma once

void t5_battery_reset_for_test(void);
/* Forget the INA226, so the next t5_ina226_init() opens it again. */
void t5_ina226_reset_for_test(void);
