/* SPDX-License-Identifier: Apache-2.0
 * Wall time for MicroPython's `time` module (MICROPY_PY_TIME_INCLUDEFILE).
 * UTC, from the system clock the RTC and SNTP keep; t5.time.local() gives
 * the local time zone. */
#include <sys/time.h>
#include "py/obj.h"
#include "shared/timeutils/timeutils.h"

static void mp_time_localtime_get(timeutils_struct_time_t *tm)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    timeutils_seconds_since_epoch_to_struct_time(tv.tv_sec, tm);
}

static mp_obj_t mp_time_time_get(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return timeutils_obj_from_timestamp(tv.tv_sec);
}
