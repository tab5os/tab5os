/* SPDX-License-Identifier: Apache-2.0
 * Hardware layer under t5_battery. Device: battery_hw.c. Host: mocks.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "t5_core/err.h"

/* P4 die temperature in 0.1 C. */
t5_err_t t5_battery_hw_chip_temp(int16_t *c10);
/* A USB host is talking to the USB Serial/JTAG port (so VBUS is present). */
bool t5_battery_hw_usb_host(void);
t5_err_t t5_battery_hw_start_task(void (*step)(void), uint32_t period_ms);
