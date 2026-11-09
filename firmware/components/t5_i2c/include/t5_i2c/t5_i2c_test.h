/* SPDX-License-Identifier: Apache-2.0
 * Host tests only.
 */
#pragma once

/* Forget all state: devices, stats, recovery hooks, lock. */
void t5_i2c_reset_for_test(void);
