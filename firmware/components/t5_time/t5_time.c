/* SPDX-License-Identifier: Apache-2.0 */
#include "t5_core/event.h"
#include "t5_core/log.h"
#include "t5_time/rtc.h"
#include "t5_time/t5_time.h"
#include "t5_time/t5_time_test.h"
#include "rtc_priv.h"
#include "t5_time_hw.h"

static const char *TAG = "t5.time";

/* Anything before this is an RTC that was never set. */
#define T5_TIME_MIN_PLAUSIBLE ((time_t)1767225600)   /* 2026-01-01 */

static bool s_valid;
static t5_time_source_t s_src = T5_TIME_SRC_NONE;

t5_err_t t5_time_init(void)
{
    t5_err_t err = t5_rtc_init();
    if (err != T5_OK) {
        T5_LOGE(TAG, "RTC not available: %s", t5_err_str(err));
        return err;
    }
    struct tm tm;
    bool ok = false;
    T5_RETURN_ON_ERR(t5_rtc_read(&tm, &ok));
    time_t t = ok ? t5_rtc_tm_to_utc(&tm) : 0;
    if (ok && t >= T5_TIME_MIN_PLAUSIBLE) {
        t5_time_hw_set_system(t);
        s_valid = true;
        s_src = T5_TIME_SRC_RTC;
        T5_LOGI(TAG, "wall time from RTC: %04d-%02d-%02d %02d:%02d:%02d UTC", tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        t5_event_post(T5_EV_TIME_SET, NULL, 0);
    } else {
        s_valid = false;
        T5_LOGW(TAG, "RTC time not valid (lost power or never set); wall time unknown");
    }
    return T5_OK;
}

bool t5_time_valid(void)
{
    return s_valid;
}

t5_time_source_t t5_time_source(void)
{
    return s_src;
}

time_t t5_time_now(void)
{
    return t5_time_hw_get_system();
}

t5_err_t t5_time_set(time_t utc, t5_time_source_t src)
{
    if (utc < T5_TIME_MIN_PLAUSIBLE) {
        return T5_ERR_INVALID_ARG;
    }
    struct tm tm;
    gmtime_r(&utc, &tm);
    t5_time_hw_set_system(utc);
    t5_err_t err = t5_rtc_write(&tm);
    if (err != T5_OK) {
        T5_LOGW(TAG, "system time set but RTC write failed: %s", t5_err_str(err));
    }
    s_valid = true;
    s_src = src;
    t5_event_post(T5_EV_TIME_SET, NULL, 0);
    return T5_OK;
}

void t5_time_reset_for_test(void)
{
    t5_rtc_reset_for_test();
    s_valid = false;
    s_src = T5_TIME_SRC_NONE;
}
