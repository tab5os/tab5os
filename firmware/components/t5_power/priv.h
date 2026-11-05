/* SPDX-License-Identifier: Apache-2.0
 * Internals shared between t5_power files and exposed to host tests.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "t5_power/power.h"
#include "power_hw.h"

/* ---- shutdown record, kept in the RTC's 4 bytes of user RAM ------------- *
 * byte 0: magic 0xA5, byte 1: off reason, byte 2: flags, byte 3: CRC-8 of 0-2.
 * Written just before power is cut, cleared at boot once read. */
#define T5_OFFREC_MAGIC 0xA5
#define T5_OFFREC_FLAG_ALARM      0x01   /* an RTC alarm was armed */
#define T5_OFFREC_FLAG_BACKGROUND 0x02   /* that alarm is a background wake */
#define T5_OFFREC_FLAG_CHARGE     0x04   /* in charge mode (deep sleep), not soft off */

typedef struct {
    uint8_t reason;     /* t5_off_reason_t */
    uint8_t flags;
} t5_offrec_t;

void t5_offrec_encode(const t5_offrec_t *r, uint8_t out[4]);
bool t5_offrec_decode(const uint8_t in[4], t5_offrec_t *out);

typedef struct {
    t5_reset_t reset;
    uint8_t rtc_flags;      /* t5_rtc_boot_flags() */
    bool rtc_ok;            /* the RTC answered */
    bool have_rec;
    t5_offrec_t rec;
    bool deep_sleep_timer;
} t5_wake_in_t;

t5_wake_reason_t t5_power_classify_wake(const t5_wake_in_t *in);

/* ---- suspend hooks (suspend.c) ------------------------------------------ */
/* min_prio: skip hooks below it (emergency off skips app hooks).
 * Returns the number of hooks that failed. *ms_out: total time. */
int t5_suspend_run(int min_prio, uint32_t *ms_out);
int t5_resume_run(int min_prio, uint32_t *ms_out);
void t5_suspend_reset_for_test(void);

/* ---- wake locks (wakelock.c) -------------------------------------------- */
void t5_wakelock_counts(uint16_t out[T5_LOCK_TYPE_COUNT]);
/* Drop locks whose timeout passed. Returns how many expired. */
int t5_wakelock_expire(uint32_t now_ms);
void t5_wakelock_reset_for_test(void);

/* ---- rails (rail.c) ----------------------------------------------------- */
void t5_rail_reset_for_test(void);

/* ---- autosave (autosave.c) ---------------------------------------------- */
void t5_autosave_reset_for_test(void);

/* ---- power.c ------------------------------------------------------------- */
void t5_power_reset_for_test(void);
