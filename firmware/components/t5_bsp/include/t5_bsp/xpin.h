/* SPDX-License-Identifier: Apache-2.0
 * Typed access to the two PI4IOE5V6408 expanders (0x43, 0x44).
 *
 * The service keeps a shadow of every config register. At init it adopts
 * what the chips hold now (so a P4 reset doesn't glitch outputs), and it
 * re-writes the shadow after every I2C bus recovery.
 *
 * Pins are named with T5_XP_* from t5_pins/pins.h. Internal service API:
 * apps reach rails and ports through the Power and Ports services.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "t5_core/err.h"
#include "t5_pins/pins.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint16_t t5_xpin_t;

typedef enum {
    T5_XPIN_PULL_NONE,
    T5_XPIN_PULL_UP,
    T5_XPIN_PULL_DOWN,
} t5_xpin_pull_t;

typedef enum {
    T5_XPIN_MODE_INPUT,
    T5_XPIN_MODE_OUTPUT,
    T5_XPIN_MODE_OUTPUT_HIZ,
} t5_xpin_mode_t;

/* Open both expanders and adopt their current state. */
t5_err_t t5_xpin_init(void);

/* Drive push-pull. The level is set before the direction, so no glitch. */
t5_err_t t5_xpin_output(t5_xpin_t pin, bool level);
/* Make an input (releases a driven line). */
t5_err_t t5_xpin_input(t5_xpin_t pin, t5_xpin_pull_t pull);
/* Drive level for ms, then return to !level. Blocks. */
t5_err_t t5_xpin_pulse(t5_xpin_t pin, bool level, uint32_t ms);
/* Live pin level: the input register for inputs, the output register for
 * outputs (the PI4IOE5V6408 input register reads 0 for output pins). */
t5_err_t t5_xpin_read(t5_xpin_t pin, bool *level);

/* Shadow state, no I2C. */
t5_xpin_mode_t t5_xpin_mode(t5_xpin_t pin);
bool t5_xpin_out_level(t5_xpin_t pin);
t5_xpin_pull_t t5_xpin_pull(t5_xpin_t pin);

/* Write the whole shadow back to both chips. */
t5_err_t t5_xpin_reapply(void);
/* Number of times the shadow was re-applied after a recovery or chip reset. */
uint32_t t5_xpin_reapply_count(void);

/* Board defaults for Phase 1: rails off, camera in reset, charging on.
 * Display and touch reset lines are left to their services. */
t5_err_t t5_xpin_apply_boot_defaults(void);

#ifdef __cplusplus
}
#endif
