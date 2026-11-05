/* SPDX-License-Identifier: Apache-2.0
 * Battery service (plan 2.7-2.10): samples the INA226 at 1 Hz, runs the fuel
 * gauge and the discharge and charge policies, drives CHG_EN and
 * nCHG_QC_EN, and runs charge mode when shutdown is asked for on USB.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "t5_battery/policy.h"
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_BATTERY_PERIOD_MS 1000

typedef struct {
    bool ready;
    uint16_t mv;               /* last plausible pack voltage */
    int32_t ma10;              /* + discharging */
    int32_t mw;
    int percent;               /* -1 unknown */
    int32_t minutes_left;      /* -1 unknown, charging or idle */
    int32_t capacity_mah;
    int32_t charge_mah;
    bool usb;
    t5_charge_state_t charge;
    bool chg_en, fast;
    bool chg_stat;             /* CHG_STAT pin level (polarity not yet verified) */
    uint32_t chg_stat_hi_charging, chg_stat_hi_discharging;  /* cross-check counts */
    uint32_t chg_stat_lo_charging, chg_stat_lo_discharging;
    int16_t imu_c10, chip_c10; /* proxy temperatures, T5_CHG_TEMP_UNKNOWN if none */
    bool low_power;
    bool swapped;              /* pack swap detected at boot */
    uint32_t samples, invalid_samples, read_errors;
    uint16_t cycles;
} t5_battery_status_t;

/* Start the INA226, restore the gauge, start the 1 Hz task, and register the
 * charge-mode shutdown handler with the power service. */
t5_err_t t5_battery_init(void);
void t5_battery_status(t5_battery_status_t *out);
int t5_battery_percent(void);

/* Settings (saved). */
t5_err_t t5_battery_set_charge_limit(bool on);   /* 80% stop, restart at 75% */
t5_err_t t5_battery_set_fast_charge(bool on);
bool t5_battery_charge_limit(void);
bool t5_battery_fast_charge(void);

/* Trace logging (plan: start on day one of Phase 2): one CSV line per sample.
 * path NULL: /sd/battery-<epoch>.csv if the card is mounted, else
 * /data/battery.csv (capped at 1 MB). */
t5_err_t t5_battery_trace_start(const char *path);
void t5_battery_trace_stop(void);
const char *t5_battery_trace_path(void);   /* NULL when not logging */

/* Charge mode, on a charge-timer wake: check USB and temperature, then
 * deep sleep again, or leave charge mode (soft off on unplug). Call early in
 * boot, after the expanders and the RTC. Returns only if charge mode ended
 * and the OS should boot normally. */
void t5_battery_charge_mode_resume(void);

/* One sampling step (the task runs it; host tests too). */
void t5_battery_step(void);

#ifdef __cplusplus
}
#endif
