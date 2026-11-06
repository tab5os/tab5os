/* SPDX-License-Identifier: Apache-2.0
 * Screen rotation maths (plan 4.2, 4.9). Pure, host-tested.
 *
 * The panel is natively 720x1280 portrait. Rotation names how the logical
 * screen (what LVGL and apps draw) sits on the panel, clockwise:
 *
 *   T5_ROT_0    portrait, panel top is the top            720 x 1280
 *   T5_ROT_90   landscape, panel right edge is the top    1280 x 720
 *   T5_ROT_180  portrait, upside down                     720 x 1280
 *   T5_ROT_270  landscape, panel left edge is the top     1280 x 720
 *
 * Logical (lx, ly) to panel (px, py), PW = 720, PH = 1280:
 *   0:   px = lx,          py = ly
 *   90:  px = PW-1-ly,     py = lx
 *   180: px = PW-1-lx,     py = PH-1-ly
 *   270: px = ly,          py = PH-1-lx
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    T5_ROT_0,
    T5_ROT_90,
    T5_ROT_180,
    T5_ROT_270,
} t5_rotation_t;

#define T5_PANEL_W 720
#define T5_PANEL_H 1280

/* Inclusive corners, like lv_area_t. */
typedef struct {
    int32_t x1, y1, x2, y2;
} t5_rect_t;

static inline bool t5_rot_landscape(t5_rotation_t r) { return r == T5_ROT_90 || r == T5_ROT_270; }
static inline int t5_rot_degrees(t5_rotation_t r) { return (int)r * 90; }
const char *t5_rot_name(t5_rotation_t r);

/* Logical resolution for a rotation. */
void t5_rot_size(t5_rotation_t r, int32_t *w, int32_t *h);

void t5_rot_to_panel(t5_rotation_t r, int32_t lx, int32_t ly, int32_t *px, int32_t *py);
void t5_rot_from_panel(t5_rotation_t r, int32_t px, int32_t py, int32_t *lx, int32_t *ly);
/* A logical rectangle's footprint on the panel. */
t5_rect_t t5_rot_rect_to_panel(t5_rotation_t r, t5_rect_t a);

static inline int32_t t5_rect_w(const t5_rect_t *a) { return a->x2 - a->x1 + 1; }
static inline int32_t t5_rect_h(const t5_rect_t *a) { return a->y2 - a->y1 + 1; }
/* Clip a to b; false if nothing is left. */
bool t5_rect_clip(t5_rect_t *a, const t5_rect_t *b);

#ifdef __cplusplus
}
#endif
