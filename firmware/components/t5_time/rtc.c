/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include "t5_core/log.h"
#include "t5_i2c/t5_i2c.h"
#include "t5_pins/pins.h"
#include "t5_time/rtc.h"

static const char *TAG = "t5.rtc";

enum {
    REG_SEC = 0x10,
    REG_ALARM_MIN = 0x17,   /* 0x17..0x19, bit7 = "don't care" */
    REG_TIMER0 = 0x1A,
    REG_EXT = 0x1C,         /* TSEL[2:0], WADA bit3, TE bit4 */
    REG_FLAG = 0x1D,        /* write-0-to-clear */
    REG_CTRL0 = 0x1E,       /* AIE bit3, TIE bit4, UIE bit5, STOP bit6 */
    REG_CTRL1 = 0x1F,       /* INIEN bit4, CHGEN bit5 */
    REG_RAM = 0x20,         /* 0x20..0x23 user RAM */
};

#define EXT_WADA  0x08
#define EXT_TE    0x10
#define CTRL0_AIE 0x08
#define CTRL0_TIE 0x10
#define CTRL0_STOP 0x40

static t5_i2c_dev_t *s_dev;
static uint8_t s_boot_flags;

uint8_t t5_rtc_bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
uint8_t t5_rtc_bcd2bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
bool t5_rtc_bcd_valid(uint8_t v) { return (v & 0x0F) <= 9 && (v >> 4) <= 9; }

static int days_in_month(int year, int mon /* 1-12 */)
{
    static const uint8_t d[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return d[mon - 1] + (mon == 2 && leap);
}

bool t5_rtc_decode(const uint8_t r[7], struct tm *out)
{
    uint8_t sec = r[0] & 0x7F, min = r[1] & 0x7F, hour = r[2] & 0x3F;
    uint8_t wd = r[3], day = r[4] & 0x3F, mon = r[5] & 0x1F, yr = r[6];
    if (!t5_rtc_bcd_valid(sec) || !t5_rtc_bcd_valid(min) || !t5_rtc_bcd_valid(hour) ||
        !t5_rtc_bcd_valid(day) || !t5_rtc_bcd_valid(mon) || !t5_rtc_bcd_valid(yr)) {
        return false;
    }
    if (wd == 0 || (wd & (wd - 1)) != 0 || wd > 0x40) {
        return false;   /* weekday is one-hot, bits 0-6 */
    }
    struct tm t = {
        .tm_sec = t5_rtc_bcd2bin(sec), .tm_min = t5_rtc_bcd2bin(min), .tm_hour = t5_rtc_bcd2bin(hour),
        .tm_mday = t5_rtc_bcd2bin(day), .tm_mon = t5_rtc_bcd2bin(mon) - 1,
        .tm_year = t5_rtc_bcd2bin(yr) + 100, .tm_wday = __builtin_ctz(wd), .tm_isdst = 0,
    };
    if (t.tm_sec > 59 || t.tm_min > 59 || t.tm_hour > 23 || t.tm_mon < 0 || t.tm_mon > 11 ||
        t.tm_mday < 1 || t.tm_mday > days_in_month(t.tm_year + 1900, t.tm_mon + 1)) {
        return false;
    }
    *out = t;
    return true;
}

void t5_rtc_encode(const struct tm *tm, uint8_t r[7])
{
    r[0] = t5_rtc_bin2bcd((uint8_t)tm->tm_sec);
    r[1] = t5_rtc_bin2bcd((uint8_t)tm->tm_min);
    r[2] = t5_rtc_bin2bcd((uint8_t)tm->tm_hour);
    r[3] = (uint8_t)(1u << (tm->tm_wday & 7));
    r[4] = t5_rtc_bin2bcd((uint8_t)tm->tm_mday);
    r[5] = t5_rtc_bin2bcd((uint8_t)(tm->tm_mon + 1));
    r[6] = t5_rtc_bin2bcd((uint8_t)(tm->tm_year % 100));
}

time_t t5_rtc_tm_to_utc(const struct tm *tm)
{
    /* Howard Hinnant's days_from_civil: no libc timegm() dependency. */
    int y = tm->tm_year + 1900;
    unsigned m = (unsigned)tm->tm_mon + 1, d = (unsigned)tm->tm_mday;
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    return (time_t)(days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}

t5_err_t t5_rtc_init(void)
{
    if (!s_dev) {
        T5_RETURN_ON_ERR(t5_i2c_open(T5_I2C_ADDR_RX8130, 400000, "rtc", &s_dev));
    }
    T5_RETURN_ON_ERR(t5_i2c_read_u8(s_dev, REG_FLAG, &s_boot_flags));
    /* INIEN on; CHGEN off (the backup cell is not rechargeable as far as we know). */
    T5_RETURN_ON_ERR(t5_i2c_update_u8(s_dev, REG_CTRL1, 0x30, 0x10));
    /* Interrupts off, timer stopped, then clear AF/TF (W0C: 1s leave others alone). */
    T5_RETURN_ON_ERR(t5_i2c_update_u8(s_dev, REG_CTRL0, CTRL0_AIE | CTRL0_TIE | CTRL0_STOP, 0));
    T5_RETURN_ON_ERR(t5_i2c_update_u8(s_dev, REG_EXT, EXT_TE, 0));
    T5_RETURN_ON_ERR(t5_rtc_clear_flags(T5_RTC_FLAG_AF | T5_RTC_FLAG_TF));
    T5_LOGI(TAG, "boot flags 0x%02x%s%s%s", s_boot_flags,
            (s_boot_flags & T5_RTC_FLAG_VLF) ? " VLF(time lost)" : "",
            (s_boot_flags & T5_RTC_FLAG_AF) ? " AF" : "", (s_boot_flags & T5_RTC_FLAG_TF) ? " TF" : "");
    return T5_OK;
}

uint8_t t5_rtc_boot_flags(void)
{
    return s_boot_flags;
}

t5_err_t t5_rtc_flags(uint8_t *flags)
{
    return t5_i2c_read_u8(s_dev, REG_FLAG, flags);
}

t5_err_t t5_rtc_clear_flags(uint8_t flags)
{
    /* Write-0-to-clear: zeros only where asked, ones elsewhere. */
    return t5_i2c_write_u8(s_dev, REG_FLAG, (uint8_t)~flags);
}

t5_err_t t5_rtc_read(struct tm *out, bool *valid)
{
    if (!s_dev) {
        return T5_ERR_STATE;
    }
    uint8_t r[7], flags;
    T5_RETURN_ON_ERR(t5_i2c_read(s_dev, REG_SEC, r, sizeof r));
    T5_RETURN_ON_ERR(t5_i2c_read_u8(s_dev, REG_FLAG, &flags));
    bool ok = t5_rtc_decode(r, out);
    if (!ok) {
        memset(out, 0, sizeof *out);
    }
    if (valid) {
        *valid = ok && !(flags & T5_RTC_FLAG_VLF);
    }
    return T5_OK;
}

t5_err_t t5_rtc_write(const struct tm *tm)
{
    if (!s_dev || !tm) {
        return T5_ERR_INVALID_ARG;
    }
    uint8_t r[7];
    t5_rtc_encode(tm, r);
    T5_RETURN_ON_ERR(t5_i2c_lock(1000));
    /* STOP holds the divider so the new second starts cleanly. */
    t5_err_t err = t5_i2c_update_u8(s_dev, REG_CTRL0, CTRL0_STOP, CTRL0_STOP);
    if (err == T5_OK) err = t5_i2c_write(s_dev, REG_SEC, r, sizeof r);
    t5_err_t err2 = t5_i2c_update_u8(s_dev, REG_CTRL0, CTRL0_STOP, 0);
    if (err == T5_OK) err = err2;
    if (err == T5_OK) err = t5_rtc_clear_flags(T5_RTC_FLAG_VLF);
    t5_i2c_unlock();
    return err;
}

t5_err_t t5_rtc_alarm_set(const struct tm *at)
{
    if (!s_dev || !at) {
        return T5_ERR_INVALID_ARG;
    }
    const uint8_t a[3] = {
        (uint8_t)(t5_rtc_bin2bcd((uint8_t)at->tm_min) & 0x7F),
        (uint8_t)(t5_rtc_bin2bcd((uint8_t)at->tm_hour) & 0x3F),
        (uint8_t)(t5_rtc_bin2bcd((uint8_t)at->tm_mday) & 0x3F),
    };
    T5_RETURN_ON_ERR(t5_i2c_lock(1000));
    /* AIE off while the alarm registers change, so a half-written alarm can't fire. */
    t5_err_t err = t5_i2c_update_u8(s_dev, REG_CTRL0, CTRL0_AIE, 0);
    if (err == T5_OK) err = t5_i2c_update_u8(s_dev, REG_EXT, EXT_WADA, EXT_WADA);   /* day of month */
    if (err == T5_OK) err = t5_i2c_write(s_dev, REG_ALARM_MIN, a, sizeof a);
    if (err == T5_OK) err = t5_rtc_clear_flags(T5_RTC_FLAG_AF);
    if (err == T5_OK) err = t5_i2c_update_u8(s_dev, REG_CTRL0, CTRL0_AIE, CTRL0_AIE);
    t5_i2c_unlock();
    return err;
}

t5_err_t t5_rtc_alarm_clear(void)
{
    if (!s_dev) {
        return T5_ERR_STATE;
    }
    const uint8_t off[3] = { 0x80, 0x80, 0x80 };
    T5_RETURN_ON_ERR(t5_i2c_update_u8(s_dev, REG_CTRL0, CTRL0_AIE, 0));
    T5_RETURN_ON_ERR(t5_i2c_write(s_dev, REG_ALARM_MIN, off, sizeof off));
    return t5_rtc_clear_flags(T5_RTC_FLAG_AF);
}

t5_err_t t5_rtc_timer_set(uint32_t seconds, uint32_t *actual)
{
    if (!s_dev) {
        return T5_ERR_STATE;
    }
    T5_RETURN_ON_ERR(t5_i2c_lock(1000));
    /* Stop: TE=0, TIE=0, clear TF. */
    t5_err_t err = t5_i2c_update_u8(s_dev, REG_EXT, EXT_TE, 0);
    if (err == T5_OK) err = t5_i2c_update_u8(s_dev, REG_CTRL0, CTRL0_TIE, 0);
    if (err == T5_OK) err = t5_rtc_clear_flags(T5_RTC_FLAG_TF);
    uint32_t got = 0;
    if (err == T5_OK && seconds) {
        uint8_t tsel;
        uint32_t count;
        if (seconds <= 65535) {
            tsel = 0x02;                      /* 1 Hz */
            count = seconds;
            got = count;
        } else {
            tsel = 0x03;                      /* 1/60 Hz */
            count = (seconds + 59) / 60;
            if (count > 65535) count = 65535;
            got = count * 60;
        }
        const uint8_t c[2] = { (uint8_t)count, (uint8_t)(count >> 8) };
        /* Datasheet order: TE=0 + TSEL, clear TF, TIE, preset, TE=1 last. */
        err = t5_i2c_update_u8(s_dev, REG_EXT, 0x07, tsel);
        if (err == T5_OK) err = t5_i2c_update_u8(s_dev, REG_CTRL0, CTRL0_TIE, CTRL0_TIE);
        if (err == T5_OK) err = t5_i2c_write(s_dev, REG_TIMER0, c, 2);
        if (err == T5_OK) err = t5_i2c_update_u8(s_dev, REG_EXT, EXT_TE, EXT_TE);
    }
    t5_i2c_unlock();
    if (actual) {
        *actual = err == T5_OK ? got : 0;
    }
    return err;
}

t5_err_t t5_rtc_ram_read(uint8_t out[T5_RTC_RAM_LEN])
{
    if (!s_dev || !out) {
        return T5_ERR_STATE;
    }
    return t5_i2c_read(s_dev, REG_RAM, out, T5_RTC_RAM_LEN);
}

t5_err_t t5_rtc_ram_write(const uint8_t in[T5_RTC_RAM_LEN])
{
    if (!s_dev || !in) {
        return T5_ERR_STATE;
    }
    return t5_i2c_write(s_dev, REG_RAM, in, T5_RTC_RAM_LEN);
}

void t5_rtc_reset_for_test(void)
{
    s_dev = NULL;
    s_boot_flags = 0;
}
