/* SPDX-License-Identifier: Apache-2.0
 * Display service: MIPI-DSI bring-up for the panel t5_bsp_panel_probe() found,
 * backlight on G22, panel sleep and wake. The panel is natively 720x1280
 * portrait, RGB565.
 *
 * Three DPI framebuffers (plan 4.2), cleared to black at init. The backlight
 * stays off until t5_display_release_hold(): the UI calls it once its first
 * frame is on the panel, so boot never shows half-drawn screens. The UI
 * pipeline (t5_ui) then renders into a free buffer and presents it; the
 * panel switches buffers at the end of a frame, so nothing tears.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_bsp/panel.h"
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_DISPLAY_W T5_PANEL_NATIVE_W
#define T5_DISPLAY_H T5_PANEL_NATIVE_H

t5_err_t t5_display_init(void);
bool t5_display_ready(void);
/* Let the backlight come on (it is held off from init until something
 * real has been drawn). */
t5_err_t t5_display_release_hold(void);

/* 0..100, kept across sleep. 0 is backlight off (a bench measurement); the
 * person's controls stop at T5_DISPLAY_BRIGHTNESS_MIN, as a black screen
 * can't be turned back up. */
#define T5_DISPLAY_BRIGHTNESS_MIN 1
t5_err_t t5_display_set_brightness(int percent);
int t5_display_brightness(void);
/* Diagnostics: change the backlight PWM frequency (default 19531 Hz). */
#define T5_DISPLAY_BL_HZ_MIN 100
#define T5_DISPLAY_BL_HZ_MAX 40000
t5_err_t t5_display_set_backlight_freq(uint32_t hz);

/* Power states (Phase 2). Both keep the DSI video stream running, because the
 * ST7123 only scans touch while it receives video (bench 0.8):
 *   blank: backlight off, panel on. Used by Screen off and Sleep.
 *   dim:   cap the backlight at percent without changing the user's
 *          brightness; -1 removes the cap. Used by Dim and low-power mode. */
t5_err_t t5_display_blank(bool blank);
t5_err_t t5_display_dim(int percent);
/* The backlight level actually applied (0 when asleep or blanked). */
int t5_display_effective_brightness(void);

/* Backlight off, display off, panel sleep-in. Wake reverses it. Touch stops
 * working while the panel sleeps, so only soft off and charge mode use this. */
t5_err_t t5_display_sleep(void);
t5_err_t t5_display_wake(void);
bool t5_display_asleep(void);

#define T5_DISPLAY_FBS 3

/* The framebuffer on screen (or queued for the next frame), 720x1280 RGB565, PSRAM. */
uint16_t *t5_display_fb(void);
/* Framebuffer i (0..T5_DISPLAY_FBS-1), NULL before init. */
uint16_t *t5_display_fb_n(int i);
/* Show framebuffer i from the next frame on. Writes back the CPU cache for
 * the whole buffer only if cache_sync (a PPA/DMA-written buffer needs none). */
t5_err_t t5_display_present(int i, bool cache_sync);
/* Index of the buffer being scanned out now, and of the one queued next
 * (equal when nothing is queued). Updated from the frame-done interrupt. */
int t5_display_scanout(void);
int t5_display_queued(void);
/* Frames scanned out since init. */
uint32_t t5_display_frames(void);
/* Wait until a frame has been scanned out with the hold released (the UI's
 * first frame is on the panel). False on timeout or without a display. */
bool t5_display_wait_shown(uint32_t timeout_ms);

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
