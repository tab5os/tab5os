/* SPDX-License-Identifier: Apache-2.0
 * Board bring-up shared by the OS, the recovery image and the device tests.
 */
#pragma once
#include "t5_core/err.h"

/* After t5_event_init(): the P4 pad rules that must hold from early boot,
 * the system I2C bus, both expanders and their boot defaults (every rail
 * off, charging on, camera in reset). Every step runs even if an earlier one
 * failed; each failure is logged and the first is returned. */
t5_err_t t5_bsp_bringup(void);
