/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include "t5_battery/gauge.h"

/* Pack open-circuit voltage against state of charge: a generic Li-ion
 * shape between the pack's empty and full voltages. Tune against measured
 * discharge traces. */
static const struct { uint16_t mv; int16_t permille; } k_ocv[] = {
    { T5_PACK_EMPTY_MV, 0 },   { 6800, 20 },  { 7000, 40 },  { 7220, 70 },  { 7380, 110 }, { 7420, 150 },
    { 7460, 200 }, { 7500, 250 }, { 7540, 300 }, { 7580, 350 }, { 7600, 400 }, { 7640, 450 },
    { 7680, 500 }, { 7700, 540 }, { 7740, 590 }, { 7820, 650 }, { 7900, 710 }, { 7960, 760 },
    { 8040, 820 }, { 8160, 900 }, { 8220, 930 }, { 8300, 970 }, { T5_PACK_FULL_MV, 1000 },
};
#define N_OCV (sizeof k_ocv / sizeof k_ocv[0])

int32_t t5_gauge_ocv_to_permille(uint16_t mv)
{
    if (mv <= k_ocv[0].mv) return 0;
    if (mv >= k_ocv[N_OCV - 1].mv) return 1000;
    for (size_t i = 1; i < N_OCV; i++) {
        if (mv <= k_ocv[i].mv) {
            int32_t v0 = k_ocv[i - 1].mv, v1 = k_ocv[i].mv;
            int32_t p0 = k_ocv[i - 1].permille, p1 = k_ocv[i].permille;
            return p0 + (p1 - p0) * ((int32_t)mv - v0) / (v1 - v0);
        }
    }
    return 1000;
}

uint16_t t5_gauge_compensate(uint16_t mv, int32_t ma10)
{
    /* Discharge pulls the terminal voltage down by I*R; charging pushes it up. */
    int32_t v = (int32_t)mv + ma10 * T5_PACK_RINT_MOHM / 10000;
    if (v < 0) v = 0;
    if (v > 0xFFFF) v = 0xFFFF;
    return (uint16_t)v;
}

bool t5_gauge_sample_plausible(uint16_t mv)
{
    return mv >= T5_PACK_MIN_MV && mv <= T5_PACK_MAX_MV;
}

static int32_t clamp_cap(int32_t mah)
{
    if (mah < T5_GAUGE_MIN_MAH) return T5_GAUGE_MIN_MAH;
    if (mah > T5_GAUGE_MAX_MAH) return T5_GAUGE_MAX_MAH;
    return mah;
}

static void set_permille(t5_gauge_t *g, int32_t pm)
{
    if (pm < 0) pm = 0;
    if (pm > 1000) pm = 1000;
    g->charge_uah = (int64_t)g->capacity_mah * pm;   /* mAh * permille = uAh */
    g->soc_permille = pm;
}

static void refresh(t5_gauge_t *g)
{
    int64_t cap = (int64_t)g->capacity_mah * 1000;
    if (g->charge_uah < 0) g->charge_uah = 0;
    if (g->charge_uah > cap) g->charge_uah = cap;
    g->soc_permille = (int32_t)(g->charge_uah * 1000 / cap);
}

void t5_gauge_boot(t5_gauge_t *g, const t5_gauge_saved_t *saved, int64_t elapsed_s, uint16_t mv, int32_t ma10)
{
    memset(g, 0, sizeof *g);
    bool have = saved && saved->magic == T5_GAUGE_SAVED_MAGIC;
    g->capacity_mah = have ? clamp_cap(saved->capacity_mah) : T5_PACK_NOMINAL_MAH;
    g->standby_ma10 = have && saved->standby_ma10 ? saved->standby_ma10 : T5_PACK_STANDBY_MA10;
    g->cycles = have ? saved->cycles : 0;
    g->avg_ma10 = ma10;

    if (!t5_gauge_sample_plausible(mv)) {
        /* Start from the record if there is one; the first good sample fixes it. */
        if (have) {
            g->charge_uah = saved->charge_uah;
            refresh(g);
            g->valid = true;
        }
        return;
    }
    int32_t by_voltage = t5_gauge_ocv_to_permille(t5_gauge_compensate(mv, ma10));
    if (!have) {
        set_permille(g, by_voltage);
        g->valid = true;
        return;
    }
    /* Predict from the record: soft off still draws a little from the pack. */
    int64_t predicted = saved->charge_uah;
    if (elapsed_s > 0) {
        predicted -= (int64_t)g->standby_ma10 * elapsed_s * 100 / 3600;   /* 0.1 mA x s -> uAh */
    }
    g->charge_uah = predicted;
    refresh(g);
    int32_t diff = by_voltage - g->soc_permille;
    if (diff < 0) diff = -diff;
    if (diff > T5_GAUGE_SWAP_PCT * 10) {
        g->swapped = true;
        set_permille(g, by_voltage);
    } else if (diff > 50) {
        /* After a rest the voltage is a fair witness: meet it halfway. */
        set_permille(g, (g->soc_permille + by_voltage) / 2);
    }
    g->valid = true;
}

void t5_gauge_update(t5_gauge_t *g, uint16_t mv, int32_t ma10, uint32_t dt_ms, bool usb)
{
    if (!t5_gauge_sample_plausible(mv)) {
        g->invalid_samples++;
        return;
    }
    if (!g->valid) {
        t5_gauge_boot(g, NULL, -1, mv, ma10);
        return;
    }
    /* Coulomb count. Charging is assumed 98% efficient. */
    int64_t d_uah = (int64_t)ma10 * dt_ms / 36000;   /* 0.1 mA x ms -> uAh */
    if (d_uah < 0) d_uah = d_uah * 98 / 100;
    g->charge_uah -= d_uah;
    if (ma10 > 0) {
        g->discharged_uah += d_uah;
    }
    /* ~60 s exponential average for time remaining. */
    g->avg_ma10 += (ma10 - g->avg_ma10) * (int32_t)(dt_ms > 60000 ? 60000 : dt_ms) / 60000;

    /* Full: on USB, at full voltage, and the charge current has tapered off. */
    if (usb && mv >= T5_PACK_CHARGED_MV && ma10 <= 0 && -ma10 < T5_PACK_TAPER_MA10) {
        g->charge_uah = (int64_t)g->capacity_mah * 1000;
        g->full_anchor = true;
        g->discharged_uah = 0;
    } else if (ma10 < -T5_GAUGE_REST_MA10) {
        g->full_anchor = false;   /* any real charging breaks a learning run */
    }

    /* Empty: learn capacity if this discharge started from full. */
    if (!usb && mv <= T5_PACK_EMPTY_MV + 100 && ma10 > 0 && ma10 < T5_PACK_LIGHT_LOAD_MA10) {
        if (g->full_anchor && g->discharged_uah > (int64_t)T5_GAUGE_MIN_MAH * 1000 / 2) {
            int32_t measured = clamp_cap((int32_t)(g->discharged_uah / 1000));
            g->capacity_mah = clamp_cap((g->capacity_mah * 7 + measured * 3) / 10);
            g->cycles++;
            g->full_anchor = false;
        }
        int32_t floor_pm = t5_gauge_ocv_to_permille(t5_gauge_compensate(mv, ma10));
        if (g->soc_permille > floor_pm + 20) set_permille(g, floor_pm);
    }

    /* Rest correction. */
    int32_t a = ma10 < 0 ? -ma10 : ma10;
    if (a < T5_GAUGE_REST_MA10) {
        g->rest_ms += dt_ms;
        if (g->rest_ms >= T5_GAUGE_REST_MS && !g->rest_corrected) {
            int32_t by_voltage = t5_gauge_ocv_to_permille(mv);
            refresh(g);
            int32_t diff = by_voltage - g->soc_permille;
            if (diff > 30 || diff < -30) {
                set_permille(g, (g->soc_permille * 3 + by_voltage * 7) / 10);
            }
            g->rest_corrected = true;
        }
    } else {
        g->rest_ms = 0;
        g->rest_corrected = false;
    }
    refresh(g);
}

int t5_gauge_percent(const t5_gauge_t *g)
{
    if (!g->valid) return -1;
    return (g->soc_permille + 5) / 10;
}

int32_t t5_gauge_minutes_left(const t5_gauge_t *g)
{
    if (!g->valid || g->avg_ma10 < 200) return -1;   /* under 20 mA: charging or no load */
    return (int32_t)(g->charge_uah * 60 / ((int64_t)g->avg_ma10 * 100));
}

void t5_gauge_save(const t5_gauge_t *g, uint16_t mv, int64_t utc, t5_gauge_saved_t *out)
{
    memset(out, 0, sizeof *out);
    out->magic = T5_GAUGE_SAVED_MAGIC;
    out->charge_uah = (int32_t)g->charge_uah;
    out->capacity_mah = g->capacity_mah;
    out->saved_utc = utc;
    out->saved_mv = mv;
    out->standby_ma10 = g->standby_ma10;
    out->cycles = g->cycles;
}
