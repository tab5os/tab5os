/* SPDX-License-Identifier: Apache-2.0
 * Display service: panel variant probe, MIPI-DSI bring-up, backlight on G22,
 * panel sleep and wake. The panel is natively 720x1280 portrait, RGB565.
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
/* Let the backlight come on (it is held off from init until something
 * real has been drawn). */
t5_err_t t5_display_release_hold(void);

/* 0..100. Kept across sleep. */
t5_err_t t5_display_set_brightness(int percent);
int t5_display_brightness(void);
/* Diagnostics: change the backlight PWM frequency (default 19531 Hz). */
t5_err_t t5_display_set_backlight_freq(uint32_t hz);

/* Power states (Phase 2). Both keep the DSI video stream running, because the
 * ST7123 only scans touch while it receives video (bench 0.8):
 *   blank: backlight off, panel on. Used by Screen off and Sleep.
 *   dim:   cap the backlight at percent without changing the user's
 *          brightness; -1 removes the cap. Used by Dim and low-power mode. */
t5_err_t t5_display_blank(bool blank);
bool t5_display_blanked(void);
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
/* Called from the frame-done interrupt (ISR context: keep it short). */
typedef void (*t5_display_frame_cb_t)(void *arg);
void t5_display_set_frame_cb(t5_display_frame_cb_t cb, void *arg);
uint32_t t5_display_frames(void);
/* After writing to the framebuffer: write back the cache for rows y0..y1-1. */
t5_err_t t5_display_flush_rows(int y0, int y1);

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
