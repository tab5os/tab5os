/* SPDX-License-Identifier: Apache-2.0
 * RX8130CE driver (0x32). Stores UTC. Register map per Epson's datasheet,
 * cross-checked against M5Unified's RX8130_Class.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Flag register 0x1D bits */
#define T5_RTC_FLAG_VLF   0x02   /* oscillation stopped / power lost: time invalid */
#define T5_RTC_FLAG_AF    0x08   /* alarm fired */
#define T5_RTC_FLAG_TF    0x10   /* countdown timer fired */

/* Opens the device, records the flags (wake reason) and then clears AF/TF
 * and disables alarm and timer interrupts. */
t5_err_t t5_rtc_init(void);
/* Flags as they were at t5_rtc_init(): tells soft-off wake by alarm/timer. */
uint8_t t5_rtc_boot_flags(void);

/* valid is false if VLF is set or the registers don't decode to a sane date. */
t5_err_t t5_rtc_read(struct tm *out, bool *valid);
/* Set the time with t5_time_set(), which keeps the system clock in step. */

t5_err_t t5_rtc_flags(uint8_t *flags);

/* Alarm at minute resolution (day of month + hour + minute, UTC). */
t5_err_t t5_rtc_alarm_set(const struct tm *at);
t5_err_t t5_rtc_alarm_clear(void);
/* Countdown timer: fires once after seconds (1..65535 at 1 Hz, longer at 1/60 Hz,
 * rounded up to whole minutes). 0 stops it. Returns the actual seconds in *actual. */
t5_err_t t5_rtc_timer_set(uint32_t seconds, uint32_t *actual);

/* Four bytes of user RAM (0x20-0x23). The RTC sits on the standby rail, so
 * this survives soft off: the power service keeps its shutdown record here. */
#define T5_RTC_RAM_LEN 4
t5_err_t t5_rtc_ram_read(uint8_t out[T5_RTC_RAM_LEN]);
t5_err_t t5_rtc_ram_write(const uint8_t in[T5_RTC_RAM_LEN]);

/* Pure helpers. */
uint8_t t5_rtc_bin2bcd(uint8_t v);
uint8_t t5_rtc_bcd2bin(uint8_t v);
bool t5_rtc_bcd_valid(uint8_t v);
/* regs: 7 bytes from 0x10 (sec, min, hour, week, day, month, year). */
bool t5_rtc_decode(const uint8_t regs[7], struct tm *out);
void t5_rtc_encode(const struct tm *tm, uint8_t regs[7]);
/* struct tm (UTC) to epoch seconds, like timegm(). */
time_t t5_rtc_tm_to_utc(const struct tm *tm);

#ifdef __cplusplus
}
#endif
