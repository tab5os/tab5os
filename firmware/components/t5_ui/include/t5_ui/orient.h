/* SPDX-License-Identifier: Apache-2.0
 * Auto-rotate orientation filter (plan 4.9). Pure logic, host-tested.
 *
 * Input: the accelerometer's "up" vector in panel axes, milli-g (x towards
 * the panel's right edge, y towards its bottom edge, z out of the glass).
 * An accelerometer at rest reads +1 g pointing away from the ground, so
 * holding the panel upright in portrait gives (0, -1000, 0).
 *
 * Rules (design "Auto-rotate"):
 *   - flat: if the tilt out of the screen plane is under ~35 degrees from
 *     flat, ignore the reading (keep the current orientation, drop pending);
 *   - hysteresis: only consider a new orientation once the angle is within
 *     45 - hyst degrees of its centre, so 45-degree tilts don't flicker;
 *   - hold: the candidate must persist for hold_ms before it is taken.
 * Orientations outside the allowed mask (the app's manifest, the rotation
 * lock) are never taken.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "t5_ui/rotation.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_ROT_MASK(r)    (1u << (r))
#define T5_ROT_ALL        0xFu
#define T5_ROT_LANDSCAPE  (T5_ROT_MASK(T5_ROT_90) | T5_ROT_MASK(T5_ROT_270))
#define T5_ROT_PORTRAIT   (T5_ROT_MASK(T5_ROT_0) | T5_ROT_MASK(T5_ROT_180))

typedef struct {
    uint32_t hold_ms;         /* default 500 */
    uint16_t hyst_deg;        /* default 15 */
    uint16_t flat_min_mg;     /* in-plane gravity below this = flat. default 570 (~35 deg) */
    uint16_t min_mg;          /* total below this = free fall / bad sample. default 500 */
    uint16_t max_mg;          /* total above this = shaking. default 1500 */
} t5_orient_cfg_t;

typedef struct {
    t5_orient_cfg_t cfg;
    t5_rotation_t current;
    bool pending;
    t5_rotation_t candidate;
    uint32_t since_ms;
    bool flat;
    int16_t angle_deg;        /* last in-plane angle, 0..359; for diagnostics */
} t5_orient_t;

void t5_orient_default_cfg(t5_orient_cfg_t *cfg);
void t5_orient_init(t5_orient_t *o, const t5_orient_cfg_t *cfg, t5_rotation_t initial);
/* Feed one sample. Returns true when the orientation changed; *out is the
 * orientation now in force either way. */
bool t5_orient_update(t5_orient_t *o, int32_t ux, int32_t uy, int32_t uz, uint32_t now_ms,
                      uint32_t allowed_mask, t5_rotation_t *out);

/* The mounting of the IMU: chip axes to panel axes. Each output axis is
 * one chip axis (0 x, 1 y, 2 z) times a sign. */
typedef struct {
    uint8_t src[3];
    int8_t sign[3];
} t5_axis_map_t;
void t5_axis_map_apply(const t5_axis_map_t *m, const int16_t chip[3], int32_t panel[3]);

#ifdef __cplusplus
}
#endif
