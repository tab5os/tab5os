/* SPDX-License-Identifier: Apache-2.0
 * Touch service: ST7123/ST7121 (0x55) or GT911 (0x14/0x5D) behind one API.
 * Coordinates are panel-native portrait pixels, 0..719 x 0..1279.
 * Rotation for landscape is applied by the display pipeline (Phase 4.9).
 *
 * The LVGL input device is a thin wrapper over t5_touch_read() and lands
 * with LVGL in Phase 4.2.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"
#include "t5_display/t5_display.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_TOUCH_MAX_POINTS 5

typedef struct {
    uint16_t x, y;
    uint8_t id;
    uint8_t strength;
} t5_touch_point_t;

typedef struct {
    uint8_t count;
    t5_touch_point_t p[T5_TOUCH_MAX_POINTS];
    uint32_t t_ms;
} t5_touch_state_t;

typedef struct {
    uint32_t irqs;
    uint32_t samples;
    uint32_t errors;
    uint16_t max_x, max_y;      /* controller's reported resolution */
    uint8_t fw_version;
    bool polling;               /* no usable interrupt (GT911 boards) */
} t5_touch_info_t;

t5_err_t t5_touch_init(void);
t5_err_t t5_touch_read(t5_touch_state_t *out);
void t5_touch_info(t5_touch_info_t *out);

/* Called from the touch task for every sample, including the release (count 0). */
typedef void (*t5_touch_cb_t)(const t5_touch_state_t *s, void *arg);
void t5_touch_set_callback(t5_touch_cb_t cb, void *arg);

/* One read from the controller (task internal; exposed for host tests). */
t5_err_t t5_touch_sample(t5_touch_state_t *out);

/* ---- Pure helpers (host-tested) ---------------------------------------- */
/* ST712x: reports = n x 7-byte records from register 0x0014. */
int t5_touch_parse_st712x(const uint8_t *reports, int n, t5_touch_point_t *out, int max);
/* GT911: status byte 0x814E and n x 8-byte records from 0x814F. */
int t5_touch_parse_gt911(uint8_t status, const uint8_t *points, t5_touch_point_t *out, int max);
/* Scale controller coordinates to the panel, clamping. */
void t5_touch_scale(t5_touch_point_t *p, uint16_t max_x, uint16_t max_y);
/* Mirror panel coordinates so touch lines up with the framebuffer. */
void t5_touch_mirror(t5_touch_point_t *p, bool mirror_x, bool mirror_y);
/* Per-panel mapping. ST7123: the init table's MADCTL (0x36 = 0x03) flips the
 * scan in both axes but not the touch layer, so touch is mirrored in X and Y
 * (measured on Unit A, ST7123). The ST7121 table has no MADCTL write, so no
 * mirror there (unverified: no ST7121 unit). */
void t5_touch_panel_mirror(t5_panel_t panel, bool *mirror_x, bool *mirror_y);

#ifdef __cplusplus
}
#endif
