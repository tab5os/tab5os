/* SPDX-License-Identifier: Apache-2.0
 * Switched power rails, reference-counted (plan 2.1).
 *
 * A rail is on while anyone holds it. Each hold records its owner, so the
 * console and Port Inspector can show who keeps a rail up. During Sleep the
 * power service drops every rail whose holders didn't ask to keep it
 * (T5_RAIL_KEEP_IN_SLEEP), unless a T5_LOCK_RAIL wake lock is held, and puts
 * it back on resume. Holds survive that: the count never changes in sleep.
 *
 *   USB5V      USB-A port 5 V
 *   EXT5V      Grove, M5-Bus, ext 5 V
 *   WLAN       ESP32-C6 power
 *   SPK        speaker amplifier
 *   BACKLIGHT  not switchable: the backlight follows the power state (hold
 *              a T5_LOCK_SCREEN wake lock to keep it on). Acquiring it is
 *              refused; t5_rail_is_on() reports whether the screen is lit.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    T5_RAIL_USB5V,
    T5_RAIL_EXT5V,
    T5_RAIL_WLAN,
    T5_RAIL_SPK,
    T5_RAIL_BACKLIGHT,
    T5_RAIL_COUNT,
} t5_rail_t;

#define T5_RAIL_KEEP_IN_SLEEP 0x01u
#define T5_RAIL_OWNER_LEN 16
#define T5_RAIL_MAX_HOLDS 32

typedef struct t5_rail_hold t5_rail_hold_t;

/* owner is copied (truncated to T5_RAIL_OWNER_LEN - 1). The hold belongs to
 * the calling app, if any, and is released when it exits. */
T5_MUST_CHECK t5_err_t t5_rail_acquire(t5_rail_t rail, const char *owner, uint32_t flags, t5_rail_hold_t **out);
/* The same, held by the system whoever the caller is (a service's own hold). */
T5_MUST_CHECK t5_err_t t5_rail_acquire_system(t5_rail_t rail, const char *owner, uint32_t flags,
                                              t5_rail_hold_t **out);
T5_MUST_CHECK t5_err_t t5_rail_release(t5_rail_hold_t *hold);

const char *t5_rail_name(t5_rail_t rail);
bool t5_rail_is_on(t5_rail_t rail);          /* what the hardware has now */
int t5_rail_count(t5_rail_t rail);           /* holders */
/* Copies up to max owner names (pointers into the hold table, valid until
 * the hold is released). Returns the number of holders. */
int t5_rail_owners(t5_rail_t rail, const char **owners, int max);
/* Hardware switch failures since boot. */
uint32_t t5_rail_errors(t5_rail_t rail);

/* Power service only: drop or restore rails for Sleep. keep_all is true
 * while a T5_LOCK_RAIL wake lock is held. */
T5_MUST_CHECK t5_err_t t5_rail_suspend(bool keep_all);
T5_MUST_CHECK t5_err_t t5_rail_resume(void);

#ifdef __cplusplus
}
#endif
