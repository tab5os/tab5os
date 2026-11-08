/* SPDX-License-Identifier: Apache-2.0
 * Charge history for the Power app (plan 6.8): one point every 5 minutes,
 * the last 24 hours, kept across reboots. Pure logic, host-tested.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define T5_BAT_HIST_LEN 288
#define T5_BAT_HIST_PERIOD_S 300
#define T5_BAT_HIST_VERSION 1

#define T5_BAT_HIST_CHARGING 0x01
#define T5_BAT_HIST_USB 0x02

typedef struct {
    uint32_t t;               /* wall clock (UTC seconds), 0 if unknown */
    int8_t pct;               /* -1 unknown */
    uint8_t flags;
    uint16_t mv;
    int16_t ma;               /* average over the period, + discharging */
} t5_bat_hist_pt_t;

typedef struct {
    uint32_t version;
    uint16_t head;            /* next slot */
    uint16_t count;
    t5_bat_hist_pt_t pt[T5_BAT_HIST_LEN];
} t5_bat_hist_t;

void t5_bat_hist_init(t5_bat_hist_t *h);
void t5_bat_hist_add(t5_bat_hist_t *h, const t5_bat_hist_pt_t *p);
/* Oldest first. Returns the count copied. */
int t5_bat_hist_read(const t5_bat_hist_t *h, t5_bat_hist_pt_t *out, int max);
bool t5_bat_hist_valid(const t5_bat_hist_t *h);

/* The service's copy (t5_battery). */
int t5_battery_history(t5_bat_hist_pt_t *out, int max);

#ifdef __cplusplus
}
#endif
