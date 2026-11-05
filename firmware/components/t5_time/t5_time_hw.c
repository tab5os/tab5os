/* SPDX-License-Identifier: Apache-2.0 */
#include <sys/time.h>
#include "t5_time_hw.h"

void t5_time_hw_set_system(time_t utc)
{
    struct timeval tv = { .tv_sec = utc, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

time_t t5_time_hw_get_system(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec;
}
