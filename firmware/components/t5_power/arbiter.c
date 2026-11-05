/* SPDX-License-Identifier: Apache-2.0 */
#include "t5_power/arbiter.h"

/* t + u, saturating, where 0 in u means "never" (so the sum is never too). */
static uint32_t after(uint32_t t, uint32_t u)
{
    if (t == UINT32_MAX || u == 0) return UINT32_MAX;
    return (UINT32_MAX - t < u) ? UINT32_MAX : t + u;
}

t5_power_state_t t5_power_arbitrate(const t5_arb_in_t *in)
{
    if (in->locks[T5_LOCK_SCREEN]) {
        return T5_PWR_ACTIVE;
    }
    const t5_power_timeouts_t *to = &in->to;
    uint32_t dim = to->dim_ms ? to->dim_ms : UINT32_MAX;
    uint32_t off = to->screen_off_ms ? to->screen_off_ms : UINT32_MAX;
    if (off < dim) {
        dim = off;   /* screen off before dim: skip Dim */
    }
    if (in->idle_ms < dim) {
        return T5_PWR_ACTIVE;
    }
    if (in->idle_ms < off) {
        return T5_PWR_DIM;
    }
    if (in->locks[T5_LOCK_CPU] || in->locks[T5_LOCK_AUDIO]) {
        return T5_PWR_SCREEN_OFF;
    }
    uint32_t sleep = after(off, to->sleep_ms);
    if (to->sleep_ms == 0) {
        sleep = UINT32_MAX;
    }
    if (in->idle_ms < sleep) {
        return T5_PWR_SCREEN_OFF;
    }
    /* Never soft off from idle on USB power, or while the net is held: a
     * mounted panel on a charger, or a download, stays in Sleep. */
    uint32_t soft = after(sleep, to->soft_off_ms);
    if (in->usb_power || in->locks[T5_LOCK_NET] || in->locks[T5_LOCK_RAIL] || in->idle_ms < soft) {
        return T5_PWR_SLEEP;
    }
    return T5_PWR_SOFT_OFF;
}

void t5_power_profile_timeouts(t5_power_profile_t p, t5_power_timeouts_t *out)
{
    if (p == T5_PROFILE_MOUNTED) {
        *out = (t5_power_timeouts_t){
            .dim_ms = 5 * 60 * 1000,
            .screen_off_ms = 30 * 60 * 1000,
            .sleep_ms = 10 * 1000,
            .soft_off_ms = 0,
        };
    } else {
        *out = (t5_power_timeouts_t){
            .dim_ms = 30 * 1000,
            .screen_off_ms = 2 * 60 * 1000,
            .sleep_ms = 10 * 1000,
            .soft_off_ms = 24u * 60 * 60 * 1000,
        };
    }
}
