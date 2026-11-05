/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include "t5_core/err.h"
/* G23 falling-edge interrupt. If use_irq is false the line is driven low
 * (GT911 workaround from Espressif's BSP) and the task polls. */
t5_err_t t5_touch_hw_init(bool use_irq);
/* Block until an interrupt or timeout. true if an interrupt happened. */
bool t5_touch_hw_wait(uint32_t timeout_ms);
