/* SPDX-License-Identifier: Apache-2.0
 * Display service: panel variant probe, MIPI-DSI bring-up, backlight on G22,
 * panel sleep and wake. The panel is natively 720x1280 portrait, RGB565.
 *
 * Phase 1 renders straight into one DPI framebuffer (test pattern, touch
 * dots). Phase 4.2 replaces that with triple buffering, PPA rotation and LVGL.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_DISPLAY_W 720
#define T5_DISPLAY_H 1280

typedef enum {
    T5_PANEL_UNKNOWN,
    T5_PANEL_ILI9881C_GT911,   /* first Tab5 batch */
    T5_PANEL_ST7123,           /* touch firmware version 3 */
    T5_PANEL_ST7121,           /* touch firmware version 1 */
} t5_panel_t;

const char *t5_display_panel_name(t5_panel_t p);

/* Pure decision used by the probe (host-tested). st712x_fw < 0 if unread. */
t5_panel_t t5_display_variant_decide(bool st712x_acks, int st712x_fw, bool gt911_acks);

/* Release the reset lines, probe I2C to identify the panel. Safe to call
 * before t5_display_init(); the touch service uses it too. */
t5_err_t t5_display_probe(t5_panel_t *out);
t5_panel_t t5_display_panel(void);

t5_err_t t5_display_init(void);
bool t5_display_ready(void);

/* 0..100. Kept across sleep. */
t5_err_t t5_display_set_brightness(int percent);
int t5_display_brightness(void);
/* Diagnostics: change the backlight PWM frequency (default 19531 Hz). */
t5_err_t t5_display_set_backlight_freq(uint32_t hz);

/* Backlight off, display off, panel sleep-in. Wake reverses it. */
t5_err_t t5_display_sleep(void);
t5_err_t t5_display_wake(void);
bool t5_display_asleep(void);

/* The DPI framebuffer, 720x1280 RGB565, in PSRAM. */
uint16_t *t5_display_fb(void);
/* After writing to the framebuffer: write back the cache for rows y0..y1-1. */
t5_err_t t5_display_flush_rows(int y0, int y1);

/* Draw a test pattern: colour bars, grey ramp, 40 px grid, corner markers. */
t5_err_t t5_display_test_pattern(void);
/* Fill a rectangle and flush it. */
t5_err_t t5_display_fill_rect(int x, int y, int w, int h, uint16_t rgb565);

/* Read the panel's DCS ID (0x04) and power mode (0x0A). Fails if the
 * DBI-over-DSI read path doesn't answer. */
t5_err_t t5_display_read_ids(uint8_t id[3], uint8_t *power_mode);

static inline uint16_t t5_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

#ifdef __cplusplus
}
#endif
