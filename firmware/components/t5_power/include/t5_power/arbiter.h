/* SPDX-License-Identifier: Apache-2.0
 * The wake-lock arbiter: pure logic, host-tested (plan 2.2).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "t5_power/power.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t idle_ms;                         /* since the last input */
    uint16_t locks[T5_LOCK_TYPE_COUNT];       /* held locks per type */
    t5_power_timeouts_t to;
    bool usb_power;                           /* never soft off from idle on USB */
} t5_arb_in_t;

/* The deepest state allowed. Never returns CHARGE_MODE. */
t5_power_state_t t5_power_arbitrate(const t5_arb_in_t *in);

/* States the service walks through, shallow to deep. */
static inline int t5_power_depth(t5_power_state_t s)
{
    return s <= T5_PWR_SLEEP ? (int)s : 4;
}

#ifdef __cplusplus
}
#endif
