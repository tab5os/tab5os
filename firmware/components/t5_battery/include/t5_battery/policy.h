/* SPDX-License-Identifier: Apache-2.0
 * Discharge and charge policy (plan 2.9, 2.10). Pure, host-tested.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "t5_battery/pack.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- discharge ----------------------------------------------------------- */
#define T5_DIS_WARN_PCT 15
#define T5_DIS_LOW_POWER_PCT 10
#define T5_DIS_OFF_PCT 5             /* or T5_PACK_EMPTY_MV under light load */
#define T5_DIS_OFF_SAMPLES 5         /* consecutive 1 Hz samples */
#define T5_DIS_EMERGENCY_SAMPLES 3
#define T5_DIS_REARM_PCT 20          /* warn again after recovering past this */

typedef struct {
    bool warned;
    bool low_power;
    uint8_t off_count;
    uint8_t emergency_count;
} t5_discharge_t;

enum {
    T5_DIS_ACT_WARN = 0x01,
    T5_DIS_ACT_LOW_POWER_ON = 0x02,
    T5_DIS_ACT_LOW_POWER_OFF = 0x04,
    T5_DIS_ACT_SOFT_OFF = 0x08,
    T5_DIS_ACT_EMERGENCY_OFF = 0x10,
};

/* percent < 0: unknown. Returns T5_DIS_ACT_* bits. */
uint32_t t5_discharge_step(t5_discharge_t *st, int percent, uint16_t mv, int32_t ma10, bool usb);

/* ---- charge -------------------------------------------------------------- */
typedef enum {
    T5_CHG_NO_USB,
    T5_CHG_CHARGING,
    T5_CHG_FULL,
    T5_CHG_PAUSED_LIMIT,     /* the 80% limit */
    T5_CHG_PAUSED_TEMP,      /* proxy temperature too high */
    T5_CHG_STATE_COUNT,
} t5_charge_state_t;

const char *t5_charge_state_name(t5_charge_state_t s);

/* The design asked for 40 C on the proxies, but Unit A's BMI270 idles at
 * 42-43 C on USB at room temperature (the board heats it), so 40 C would
 * never charge. 50 C on the IMU is roughly 40 C ambient until a better proxy
 * exists (bench-results.md, Phase 2). */
#define T5_CHG_TEMP_PAUSE_C10 500
#define T5_CHG_TEMP_RESUME_C10 470
#define T5_CHG_LIMIT_PCT 80
#define T5_CHG_LIMIT_RESTART_PCT 75
#define T5_CHG_TEMP_UNKNOWN INT16_MIN

typedef struct {
    bool usb;
    int percent;             /* -1 unknown */
    uint16_t mv;
    int32_t ma10;
    int16_t temp_c10;        /* hottest proxy, T5_CHG_TEMP_UNKNOWN if none */
    bool limit_enabled;
    bool fast_enabled;
} t5_charge_in_t;

typedef struct {
    t5_charge_state_t state;
    bool chg_en;             /* CHG_EN pin */
    bool fast;               /* drive nCHG_QC_EN low */
} t5_charge_out_t;

/* prev: the previous state (hysteresis). */
void t5_charge_step(const t5_charge_in_t *in, t5_charge_state_t prev, t5_charge_out_t *out);

/* USB power present? CHG_STAT isn't trusted until its polarity is measured,
 * so this uses current direction and the bus-voltage anomaly only;
 * host_link is the USB Serial/JTAG seeing a host. */
bool t5_usb_power_guess(int32_t ma10, uint16_t mv, bool host_link);

#ifdef __cplusplus
}
#endif
