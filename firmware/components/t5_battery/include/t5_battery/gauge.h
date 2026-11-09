/* SPDX-License-Identifier: Apache-2.0
 * Software fuel gauge for the pack (t5_battery/pack.h, plan 2.8). Pure logic:
 * no I/O, every input is passed in, so it runs on recorded traces in host
 * tests.
 *
 *  - Awake: integrate current over time (coulomb counting).
 *  - At rest (|I| small for T5_GAUGE_REST_MS): pull towards the voltage curve.
 *  - After soft off: subtract elapsed RTC time x standby draw, then check the
 *    first voltage; a big disagreement means the pack was swapped.
 *  - Learn real capacity from a discharge that starts at "full".
 *
 * The voltage curve is a generic Li-ion curve until real discharge traces
 * tune it (plan: "trace collection").
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "t5_battery/pack.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_GAUGE_MIN_MAH 1000
#define T5_GAUGE_MAX_MAH 3500
#define T5_GAUGE_REST_MA10 300         /* |I| below 30 mA counts as rest */
#define T5_GAUGE_REST_MS (5u * 60 * 1000)
#define T5_GAUGE_SWAP_PCT 25           /* boot voltage vs prediction */

/* What survives a power cycle (stored by the battery service). */
typedef struct {
    uint32_t magic;
    int32_t charge_uah;        /* remaining charge */
    int32_t capacity_mah;      /* learned */
    int64_t saved_utc;         /* wall time of the save, 0 = unknown */
    uint16_t saved_mv;
    uint16_t standby_ma10;     /* soft-off draw estimate */
    uint16_t cycles;           /* full-discharge learning events */
    uint16_t reserved;
} t5_gauge_saved_t;
#define T5_GAUGE_SAVED_MAGIC 0x47415547u   /* "GAUG" */

typedef struct {
    int32_t capacity_mah;
    int64_t charge_uah;        /* remaining, clamped 0..capacity */
    int32_t soc_permille;
    bool valid;                /* initialised from at least one sample */
    /* rest tracking */
    uint32_t rest_ms;
    bool rest_corrected;
    /* learning */
    bool full_anchor;          /* reached full and discharged since, without charging */
    int64_t discharged_uah;
    uint16_t cycles;
    uint16_t standby_ma10;
    /* filtered current for time-remaining */
    int32_t avg_ma10;
    /* diagnostics */
    uint32_t invalid_samples;
    bool swapped;              /* set by t5_gauge_boot() */
} t5_gauge_t;

/* Pack open-circuit voltage to state of charge, 0..1000. */
int32_t t5_gauge_ocv_to_permille(uint16_t pack_mv);
/* Estimate open-circuit voltage from a loaded reading. */
uint16_t t5_gauge_compensate(uint16_t mv, int32_t ma10);
/* Within T5_PACK_MIN_MV..T5_PACK_MAX_MV: a reading that can come from the pack. */
bool t5_gauge_sample_plausible(uint16_t mv);

/* Start from the saved record (or NULL) and the first sample at boot.
 * elapsed_s: wall time since saved_utc, or -1 if unknown. */
void t5_gauge_boot(t5_gauge_t *g, const t5_gauge_saved_t *saved, int64_t elapsed_s, uint16_t mv, int32_t ma10);
/* One sample, dt_ms since the previous. charging: the charger is on and
 * USB present (used for the full anchor). */
void t5_gauge_update(t5_gauge_t *g, uint16_t mv, int32_t ma10, uint32_t dt_ms, bool usb);
int t5_gauge_percent(const t5_gauge_t *g);
/* Minutes to empty at the recent average draw; -1 while charging or idle. */
int32_t t5_gauge_minutes_left(const t5_gauge_t *g);
void t5_gauge_save(const t5_gauge_t *g, uint16_t mv, int64_t utc, t5_gauge_saved_t *out);

#ifdef __cplusplus
}
#endif
