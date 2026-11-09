/* SPDX-License-Identifier: Apache-2.0
 * Tests only.
 */
#pragma once
#include "t5_imu/t5_imu.h"

/* What t5_imu_start()'s background task runs, on the caller's task. */
t5_err_t t5_imu_init_blocking(void);
/* Forget the chip: status back to OFF. */
void t5_imu_reset_for_test(void);
