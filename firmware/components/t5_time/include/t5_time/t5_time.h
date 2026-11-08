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

/* ---- time zone (plan 6.5) ---------------------------------------------------
 * The clock stays in UTC; the zone is a POSIX TZ string, so localtime_r()
 * gives local time once it is applied. t5_time_tz_load() applies the saved
 * zone (main, after storage). */
#include <stdbool.h>
struct tm;
void t5_time_tz_load(void);
t5_err_t t5_time_set_tz(const char *posix);
const char *t5_time_tz(void);
const char *t5_time_tz_name(void);           /* list name, or the POSIX string */
int t5_time_zone_count(void);
bool t5_time_zone(int i, const char **name, const char **posix);
/* Local time now. Returns false if the wall clock isn't set yet (the
 * fields still hold whatever the clock says). */
bool t5_time_local(struct tm *out);

#ifdef __cplusplus
}
#endif
