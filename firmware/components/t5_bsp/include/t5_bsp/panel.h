/* SPDX-License-Identifier: Apache-2.0
 * Which display and touch controller this Tab5 has.
 *
 * Tab5 units ship with one of three panels, told apart by their touch
 * controller: an ST712x at 0x55 whose firmware version names the panel, or a
 * GT911 at 0x14/0x5D. t5_bsp_panel_probe() releases the LCD and touch resets,
 * looks once, and keeps the answer; the display and touch services both read
 * it, so neither depends on the other.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Every variant is 720x1280 portrait. */
#define T5_PANEL_NATIVE_W 720
#define T5_PANEL_NATIVE_H 1280

typedef enum {
    T5_PANEL_UNKNOWN,
    T5_PANEL_ILI9881C_GT911,   /* first Tab5 batch */
    T5_PANEL_ST7123,           /* touch firmware version 3 */
    T5_PANEL_ST7121,           /* touch firmware version 1 */
} t5_panel_t;

/* MADCTL (0x36) that the ST7123 init table sends. It flips the scan in both
 * axes but not the touch layer, so ST7123 touch is mirrored in X and Y
 * (measured on Unit A). The ST7121 and ILI9881C tables send no MADCTL, so
 * their touch is taken to line up (unverified: no such unit). */
#define T5_ST7123_MADCTL 0x03

typedef struct {
    t5_panel_t panel;
    uint8_t touch_addr;        /* 7-bit I2C address of the touch controller */
    uint8_t touch_fw;          /* ST712x firmware version; 0 for GT911 */
    bool touch_mirror_x, touch_mirror_y;
} t5_panel_info_t;

/* Release the LCD and touch resets and identify the panel. Probes the bus
 * only the first time. T5_ERR_NOT_SUPPORTED if no known panel answers. */
t5_err_t t5_bsp_panel_probe(void);
/* The probe's result (panel T5_PANEL_UNKNOWN before a successful probe). */
const t5_panel_info_t *t5_bsp_panel(void);
const char *t5_bsp_panel_name(t5_panel_t p);

#ifdef __cplusplus
}
#endif
