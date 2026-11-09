/* SPDX-License-Identifier: Apache-2.0
 * Tests only.
 */
#pragma once
#include <stdbool.h>
#include "t5_core/err.h"

/* Whether the RTC's alarm or timer interrupt is enabled (device tests). */
t5_err_t t5_rtc_irqs_enabled_for_test(bool *on);

/* Forget the RTC handle and boot flags. */
void t5_rtc_reset_for_test(void);
/* Forget the RTC and the wall clock's validity. */
void t5_time_reset_for_test(void);
