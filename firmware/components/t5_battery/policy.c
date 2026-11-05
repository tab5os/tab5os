/* SPDX-License-Identifier: Apache-2.0 */
#include "t5_battery/policy.h"

uint32_t t5_discharge_step(t5_discharge_t *st, int pct, uint16_t mv, int32_t ma10, bool usb)
{
    uint32_t act = 0;
    if (usb) {
        st->off_count = st->emergency_count = 0;
        st->warned = false;
        if (st->low_power) {
            st->low_power = false;
            act |= T5_DIS_ACT_LOW_POWER_OFF;
        }
        return act;
    }
    /* Voltage floors first: they protect the pack even if the gauge is wrong. */
    bool plausible = mv >= 5000 && mv <= 9000;
    if (plausible && mv <= T5_DIS_EMERGENCY_MV) {
        if (++st->emergency_count >= T5_DIS_EMERGENCY_SAMPLES) return act | T5_DIS_ACT_EMERGENCY_OFF;
    } else {
        st->emergency_count = 0;
    }
    bool light = ma10 < T5_DIS_LIGHT_LOAD_MA10;
    bool low_v = plausible && mv <= T5_DIS_OFF_MV && light;
    bool low_pct = pct >= 0 && pct <= T5_DIS_OFF_PCT;
    if (low_v || low_pct) {
        if (++st->off_count >= T5_DIS_OFF_SAMPLES) return act | T5_DIS_ACT_SOFT_OFF;
    } else {
        st->off_count = 0;
    }
    if (pct < 0) return act;
    if (!st->warned && pct <= T5_DIS_WARN_PCT) {
        st->warned = true;
        act |= T5_DIS_ACT_WARN;
    } else if (st->warned && pct >= T5_DIS_REARM_PCT) {
        st->warned = false;
    }
    if (!st->low_power && pct <= T5_DIS_LOW_POWER_PCT) {
        st->low_power = true;
        act |= T5_DIS_ACT_LOW_POWER_ON;
    } else if (st->low_power && pct > T5_DIS_WARN_PCT) {
        st->low_power = false;
        act |= T5_DIS_ACT_LOW_POWER_OFF;
    }
    return act;
}

const char *t5_charge_state_name(t5_charge_state_t s)
{
    switch (s) {
    case T5_CHG_NO_USB: return "on battery";
    case T5_CHG_CHARGING: return "charging";
    case T5_CHG_FULL: return "full";
    case T5_CHG_PAUSED_LIMIT: return "paused (limit)";
    case T5_CHG_PAUSED_TEMP: return "paused (temperature)";
    default: return "?";
    }
}

void t5_charge_step(const t5_charge_in_t *in, t5_charge_state_t prev, t5_charge_out_t *out)
{
    out->fast = in->fast_enabled;
    if (!in->usb) {
        /* CHG_EN stays on: plugging in then charges at once, even before the
         * policy runs again, and a crash loop can't leave charging off. */
        out->state = T5_CHG_NO_USB;
        out->chg_en = true;
        return;
    }
    bool temp_known = in->temp_c10 != T5_CHG_TEMP_UNKNOWN;
    if (temp_known && (in->temp_c10 >= T5_CHG_TEMP_PAUSE_C10 ||
                       (prev == T5_CHG_PAUSED_TEMP && in->temp_c10 > T5_CHG_TEMP_RESUME_C10))) {
        out->state = T5_CHG_PAUSED_TEMP;
        out->chg_en = false;
        out->fast = false;
        return;
    }
    if (in->limit_enabled && in->percent >= 0 &&
        (in->percent >= T5_CHG_LIMIT_PCT || (prev == T5_CHG_PAUSED_LIMIT && in->percent > T5_CHG_LIMIT_RESTART_PCT))) {
        out->state = T5_CHG_PAUSED_LIMIT;
        out->chg_en = false;
        out->fast = false;
        return;
    }
    out->chg_en = true;
    bool full_v = in->mv >= 8300 && in->mv <= 9000;
    bool tapered = in->ma10 > -T5_CHG_FULL_MA10;
    out->state = (full_v && tapered) || in->percent >= 100 ? T5_CHG_FULL : T5_CHG_CHARGING;
}

bool t5_usb_power_guess(int32_t ma10, uint16_t mv, bool host_link)
{
    /* On battery alone the running P4 and panel draw well over 100 mA, so a
     * small or negative battery current means something else powers the
     * board. The bus-voltage jump to ~4.4 V only happens on USB (bench 0.4). */
    if (host_link) return true;
    if (mv < 5000) return true;
    return ma10 < 500;
}
