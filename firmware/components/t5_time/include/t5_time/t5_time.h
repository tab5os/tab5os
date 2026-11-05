/* SPDX-License-Identifier: Apache-2.0
 * Wall-clock service. The RTC holds UTC; the system clock is set from it at
 * boot. t5_time_valid() says whether wall time can be trusted: false after
 * the RTC lost power until the user, NTP (Phase 3) or a PC sets it.
 */
#pragma once

#include <stdbool.h>
#include <time.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    T5_TIME_SRC_NONE,
    T5_TIME_SRC_RTC,
    T5_TIME_SRC_USER,
    T5_TIME_SRC_NTP,
} t5_time_source_t;

t5_err_t t5_time_init(void);
bool t5_time_valid(void);
t5_time_source_t t5_time_source(void);
time_t t5_time_now(void);
/* Set wall time (UTC) in the system clock and the RTC. */
t5_err_t t5_time_set(time_t utc, t5_time_source_t src);

#ifdef __cplusplus
}
#endif
