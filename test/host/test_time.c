/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "unity.h"
#include "t5_mocks.h"
#include "t5_pins/pins.h"
#include "t5_storage/paths.h"
#include "t5_time/rtc.h"
#include "t5_time/t5_time.h"
#include "t5_time/t5_time_test.h"

static mock_i2c_dev_t *rtc;
static char s_dir[64];

static void set_rtc_time(int y, int mo, int d, int h, int mi, int s, int wday)
{
    struct tm t = { .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi,
                    .tm_sec = s, .tm_wday = wday };
    t5_rtc_encode(&t, &rtc->regs[0x10]);
}

void setUp(void)
{
    mock_reset_all();
    t5_time_reset_for_test();
    mock_system_time = 0;
    rtc = mock_rtc_add();
}

void tearDown(void)
{
    if (s_dir[0]) {
        char path[96];
        snprintf(path, sizeof path, "%s/tz", s_dir);
        unlink(path);
        rmdir(s_dir);
        s_dir[0] = 0;
    }
}

static void test_bcd(void)
{
    TEST_ASSERT_EQUAL_HEX8(0x59, t5_rtc_bin2bcd(59));
    TEST_ASSERT_EQUAL(59, t5_rtc_bcd2bin(0x59));
    TEST_ASSERT_TRUE(t5_rtc_bcd_valid(0x99));
    TEST_ASSERT_FALSE(t5_rtc_bcd_valid(0x1A));
}

static void test_tm_to_utc_known_dates(void)
{
    struct tm t = { .tm_year = 70, .tm_mon = 0, .tm_mday = 1 };
    TEST_ASSERT_EQUAL_INT64(0, (int64_t)t5_rtc_tm_to_utc(&t));
    struct tm u = { .tm_year = 126, .tm_mon = 10, .tm_mday = 5, .tm_hour = 10, .tm_min = 26, .tm_sec = 44 };
    TEST_ASSERT_EQUAL_INT64(1793874404LL, (int64_t)t5_rtc_tm_to_utc(&u));
    struct tm leap = { .tm_year = 128, .tm_mon = 1, .tm_mday = 29 };   /* 2028-02-29 */
    TEST_ASSERT_EQUAL_INT64(1835395200LL, (int64_t)t5_rtc_tm_to_utc(&leap));
}

static void test_encode_decode_roundtrip_over_years(void)
{
    for (time_t t = 1767225600; t < 1767225600 + 20LL * 366 * 86400; t += 86400 * 7 + 3671) {
        struct tm a, b;
        gmtime_r(&t, &a);
        uint8_t r[7];
        t5_rtc_encode(&a, r);
        TEST_ASSERT_TRUE(t5_rtc_decode(r, &b));
        TEST_ASSERT_EQUAL_INT64((int64_t)t, (int64_t)t5_rtc_tm_to_utc(&b));
        TEST_ASSERT_EQUAL(a.tm_wday, b.tm_wday);
    }
}

static void test_decode_rejects_garbage(void)
{
    uint8_t r[7] = { 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x26 };   /* weekday 0: invalid */
    struct tm t;
    TEST_ASSERT_FALSE(t5_rtc_decode(r, &t));
    uint8_t r2[7] = { 0x00, 0x00, 0x00, 0x01, 0x31, 0x02, 0x26 };  /* Feb 31 */
    TEST_ASSERT_FALSE(t5_rtc_decode(r2, &t));
    uint8_t r3[7] = { 0x5A, 0x00, 0x00, 0x01, 0x01, 0x01, 0x26 };  /* bad BCD */
    TEST_ASSERT_FALSE(t5_rtc_decode(r3, &t));
}

static void test_init_records_flags_and_clears_alarm_timer_only(void)
{
    rtc->regs[0x1D] = 0x1A;   /* VLF | AF | TF */
    rtc->regs[0x1E] = 0x18;   /* AIE | TIE */
    rtc->regs[0x1C] = 0x12;   /* TE | TSEL=2 */
    TEST_ASSERT_EQUAL(T5_OK, t5_rtc_init());
    TEST_ASSERT_EQUAL_HEX8(0x1A, t5_rtc_boot_flags());
    TEST_ASSERT_EQUAL_HEX8(0x02, rtc->regs[0x1D]);        /* VLF kept, AF/TF cleared */
    TEST_ASSERT_EQUAL_HEX8(0x00, rtc->regs[0x1E] & 0x18);  /* interrupts off: an IRQ would cut power */
    TEST_ASSERT_EQUAL_HEX8(0x00, rtc->regs[0x1C] & 0x10);  /* timer stopped */
}

static void test_time_invalid_when_vlf(void)
{
    set_rtc_time(2026, 11, 5, 10, 0, 0, 4);
    rtc->regs[0x1D] = 0x02;
    TEST_ASSERT_EQUAL(T5_OK, t5_time_init());
    TEST_ASSERT_FALSE(t5_time_valid());
    TEST_ASSERT_EQUAL_INT64(0, (int64_t)mock_system_time);
}

static void test_time_from_rtc_and_set(void)
{
    set_rtc_time(2026, 11, 5, 10, 26, 44, 4);
    TEST_ASSERT_EQUAL(T5_OK, t5_time_init());
    TEST_ASSERT_TRUE(t5_time_valid());
    TEST_ASSERT_EQUAL(T5_TIME_SRC_RTC, t5_time_source());
    TEST_ASSERT_EQUAL_INT64(1793874404LL, (int64_t)mock_system_time);
    TEST_ASSERT_EQUAL(1, mock_events_count(T5_EV_TIME_SET));

    rtc->regs[0x1D] = 0x02;   /* pretend VLF came up */
    TEST_ASSERT_EQUAL(T5_OK, t5_time_set(1800000000, T5_TIME_SRC_USER));
    TEST_ASSERT_EQUAL_HEX8(0x00, rtc->regs[0x1D] & 0x02);   /* write clears VLF */
    TEST_ASSERT_EQUAL_HEX8(0x00, rtc->regs[0x1E] & 0x40);   /* STOP released */
    struct tm t;
    bool ok;
    t5_rtc_read(&t, &ok);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT64(1800000000LL, (int64_t)t5_rtc_tm_to_utc(&t));
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_time_set(1000, T5_TIME_SRC_USER));
}

static void test_rtc_write_failure_still_sets_the_clock(void)
{
    set_rtc_time(2026, 11, 5, 10, 26, 44, 4);
    TEST_ASSERT_EQUAL(T5_OK, t5_time_init());
    mock_events_reset();
    mock_i2c_fail_next(T5_I2C_ADDR_RX8130, 100, T5_ERR_NACK);
    TEST_ASSERT_EQUAL(T5_OK, t5_time_set(1800000000, T5_TIME_SRC_NTP));
    TEST_ASSERT_EQUAL_INT64(1800000000, (int64_t)mock_system_time);
    TEST_ASSERT_EQUAL(T5_TIME_SRC_NTP, t5_time_source());
    TEST_ASSERT_EQUAL(1, mock_events_count(T5_EV_TIME_SET));
}

static void use_temp_sys_dir(void)
{
    snprintf(s_dir, sizeof s_dir, "/tmp/t5_time_test_%d", (int)getpid());
    TEST_ASSERT_EQUAL(0, mkdir(s_dir, 0755));
    t5_sys_dir_set(s_dir);
}

static void test_time_zone_saved_and_loaded(void)
{
    use_temp_sys_dir();
    TEST_ASSERT_EQUAL(T5_OK, t5_time_set_tz("JST-9"));
    TEST_ASSERT_EQUAL_STRING("Tokyo, Seoul", t5_time_tz_name());
    TEST_ASSERT_EQUAL(T5_OK, t5_time_set_tz("UTC0"));
    char path[96];
    snprintf(path, sizeof path, "%s/tz", s_dir);
    FILE *f = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs("<+05>-5", f);   /* not in the list: shown as the POSIX string */
    fclose(f);
    t5_time_tz_load();
    TEST_ASSERT_EQUAL_STRING("<+05>-5", t5_time_tz());
    TEST_ASSERT_EQUAL_STRING("<+05>-5", t5_time_tz_name());
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_time_set_tz(""));
    T5_IGNORE(t5_time_set_tz("UTC0"));
}

static void test_local_time_follows_the_wall_clock(void)
{
    use_temp_sys_dir();
    set_rtc_time(2026, 11, 5, 10, 26, 44, 4);
    TEST_ASSERT_EQUAL(T5_OK, t5_time_init());
    TEST_ASSERT_EQUAL(T5_OK, t5_time_set_tz("JST-9"));
    struct tm lt;
    TEST_ASSERT_TRUE(t5_time_local(&lt));
    TEST_ASSERT_EQUAL(19, lt.tm_hour);
    TEST_ASSERT_EQUAL(26, lt.tm_min);
    T5_IGNORE(t5_time_set_tz("UTC0"));   /* leave the process in UTC */
}

static void test_alarm_and_timer_registers(void)
{
    t5_rtc_init();
    struct tm at = { .tm_mday = 7, .tm_hour = 23, .tm_min = 45 };
    TEST_ASSERT_EQUAL(T5_OK, t5_rtc_alarm_set(&at));
    TEST_ASSERT_EQUAL_HEX8(0x45, rtc->regs[0x17]);
    TEST_ASSERT_EQUAL_HEX8(0x23, rtc->regs[0x18]);
    TEST_ASSERT_EQUAL_HEX8(0x07, rtc->regs[0x19]);
    TEST_ASSERT_EQUAL_HEX8(0x08, rtc->regs[0x1C] & 0x08);   /* WADA: day of month */
    TEST_ASSERT_EQUAL_HEX8(0x08, rtc->regs[0x1E] & 0x08);   /* AIE */
    TEST_ASSERT_EQUAL(T5_OK, t5_rtc_alarm_clear());
    TEST_ASSERT_EQUAL_HEX8(0x00, rtc->regs[0x1E] & 0x08);

    uint32_t got;
    TEST_ASSERT_EQUAL(T5_OK, t5_rtc_timer_set(300, &got));
    TEST_ASSERT_EQUAL(300, got);
    TEST_ASSERT_EQUAL_HEX8(0x02, rtc->regs[0x1C] & 0x07);   /* 1 Hz */
    TEST_ASSERT_EQUAL(300, rtc->regs[0x1A] | (rtc->regs[0x1B] << 8));
    TEST_ASSERT_EQUAL(T5_OK, t5_rtc_timer_set(100000, &got));
    TEST_ASSERT_EQUAL(100020, got);                          /* rounded up to minutes */
    TEST_ASSERT_EQUAL_HEX8(0x03, rtc->regs[0x1C] & 0x07);   /* 1/60 Hz */
    TEST_ASSERT_EQUAL(T5_OK, t5_rtc_timer_set(0, &got));
    TEST_ASSERT_EQUAL_HEX8(0x00, rtc->regs[0x1C] & 0x10);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bcd);
    RUN_TEST(test_tm_to_utc_known_dates);
    RUN_TEST(test_encode_decode_roundtrip_over_years);
    RUN_TEST(test_decode_rejects_garbage);
    RUN_TEST(test_init_records_flags_and_clears_alarm_timer_only);
    RUN_TEST(test_time_invalid_when_vlf);
    RUN_TEST(test_time_from_rtc_and_set);
    RUN_TEST(test_rtc_write_failure_still_sets_the_clock);
    RUN_TEST(test_time_zone_saved_and_loaded);
    RUN_TEST(test_local_time_follows_the_wall_clock);
    RUN_TEST(test_alarm_and_timer_registers);
    return UNITY_END();
}
