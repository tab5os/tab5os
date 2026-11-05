/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <time.h>
/* System clock. Device: settimeofday/gettimeofday. Host: a variable. */
void t5_time_hw_set_system(time_t utc);
time_t t5_time_hw_get_system(void);
