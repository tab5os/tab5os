/* SPDX-License-Identifier: Apache-2.0
 * Time zone (plan 6.5, Settings > Time). The RTC and the system clock stay
 * in UTC; the zone is a POSIX TZ string applied with tzset(), so
 * localtime_r() gives local time. Kept in /data/sys/tz and applied at boot.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "t5_core/log.h"
#include "t5_storage/t5_storage.h"
#include "t5_time/t5_time.h"

static const char *TAG = "t5.time";

#define TZ_FILE T5_DATA_PATH "/sys/tz"

typedef struct {
    const char *name;
    const char *posix;
} zone_t;

/* A short list of common zones, west to east. The POSIX rules carry DST. */
static const zone_t s_zones[] = {
    { "UTC", "UTC0" },
    { "Honolulu", "HST10" },
    { "Anchorage", "AKST9AKDT,M3.2.0,M11.1.0" },
    { "Los Angeles", "PST8PDT,M3.2.0,M11.1.0" },
    { "Denver", "MST7MDT,M3.2.0,M11.1.0" },
    { "Phoenix", "MST7" },
    { "Chicago", "CST6CDT,M3.2.0,M11.1.0" },
    { "New York", "EST5EDT,M3.2.0,M11.1.0" },
    { "Sao Paulo", "<-03>3" },
    { "London", "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Berlin, Paris", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Athens, Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Moscow", "MSK-3" },
    { "Dubai", "<+04>-4" },
    { "Karachi", "PKT-5" },
    { "India", "IST-5:30" },
    { "Dhaka", "<+06>-6" },
    { "Bangkok", "<+07>-7" },
    { "Singapore, Beijing", "CST-8" },
    { "Tokyo, Seoul", "JST-9" },
    { "Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3" },
};
#define NZONES (int)(sizeof s_zones / sizeof s_zones[0])

static char s_tz[64] = "UTC0";

int t5_time_zone_count(void)
{
    return NZONES;
}

bool t5_time_zone(int i, const char **name, const char **posix)
{
    if (i < 0 || i >= NZONES) return false;
    if (name) *name = s_zones[i].name;
    if (posix) *posix = s_zones[i].posix;
    return true;
}

const char *t5_time_tz(void)
{
    return s_tz;
}

const char *t5_time_tz_name(void)
{
    for (int i = 0; i < NZONES; i++) {
        if (!strcmp(s_zones[i].posix, s_tz)) return s_zones[i].name;
    }
    return s_tz;
}

static void apply(void)
{
    setenv("TZ", s_tz, 1);
    tzset();
}

t5_err_t t5_time_set_tz(const char *posix)
{
    if (!posix || !posix[0] || strlen(posix) >= sizeof s_tz) return T5_ERR_INVALID_ARG;
    snprintf(s_tz, sizeof s_tz, "%s", posix);
    apply();
    T5_LOGI(TAG, "time zone %s", s_tz);
    return t5_storage_write_atomic(TZ_FILE, s_tz, strlen(s_tz) + 1);
}

void t5_time_tz_load(void)
{
    FILE *f = fopen(TZ_FILE, "r");
    if (f) {
        char buf[64] = "";
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        if (n && buf[0]) snprintf(s_tz, sizeof s_tz, "%s", buf);
    }
    apply();
}

bool t5_time_local(struct tm *out)
{
    if (!out) return false;
    time_t now = time(NULL);
    localtime_r(&now, out);
    return t5_time_valid();
}
