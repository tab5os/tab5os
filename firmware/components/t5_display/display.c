/* SPDX-License-Identifier: Apache-2.0 */
#include "t5_bsp/panel.h"
#include "t5_core/event.h"
#include "t5_display/t5_display.h"
#include "t5_display/t5_display_test.h"
#include "display_hw.h"

static bool s_ready, s_asleep, s_blank;
static bool s_hold = true;    /* backlight held off until there's something to show */
static int s_brightness = 80;
static int s_dim = -1;
static uint16_t *s_fb;                      /* buffer on screen or queued */
static uint16_t *s_fbs[T5_DISPLAY_FBS];
static volatile int s_scanout, s_queued;
static volatile uint32_t s_frames;
static volatile bool s_shown;

static t5_err_t apply_backlight(void);

t5_err_t t5_display_init(void)
{
    if (s_ready) {
        return T5_OK;
    }
    T5_RETURN_ON_ERR(t5_bsp_panel_probe());
    T5_RETURN_ON_ERR(t5_display_hw_init(t5_bsp_panel()->panel, s_fbs));
    s_fb = s_fbs[0];
    s_scanout = s_queued = 0;
    /* Black in every buffer, so whichever is shown first holds no stale
     * PSRAM contents. */
    for (int i = 0; i < T5_DISPLAY_FBS; i++) {
        for (size_t k = 0; k < (size_t)T5_DISPLAY_W * T5_DISPLAY_H; k++) s_fbs[i][k] = 0;
        t5_display_hw_flush(s_fbs[i], 0, T5_DISPLAY_H);
    }
    s_ready = true;
    s_asleep = false;
    T5_RETURN_ON_ERR(apply_backlight());
    t5_event_post(T5_EV_DISPLAY_READY, NULL, 0);
    return T5_OK;
}

bool t5_display_ready(void)
{
    return s_ready;
}

t5_err_t t5_display_release_hold(void)
{
    if (!s_hold) return T5_OK;
    s_hold = false;
    return apply_backlight();
}

int t5_display_effective_brightness(void)
{
    if (!s_ready || s_asleep || s_blank || s_hold) {
        return 0;
    }
    return (s_dim >= 0 && s_dim < s_brightness) ? s_dim : s_brightness;
}

static t5_err_t apply_backlight(void)
{
    return s_ready ? t5_display_hw_backlight(t5_display_effective_brightness()) : T5_OK;
}

t5_err_t t5_display_set_brightness(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_brightness = percent;
    return apply_backlight();
}

t5_err_t t5_display_blank(bool blank)
{
    if (!s_ready) return T5_ERR_STATE;
    s_blank = blank;
    return apply_backlight();
}

t5_err_t t5_display_dim(int percent)
{
    if (percent > 100) percent = 100;
    s_dim = percent < 0 ? -1 : percent;
    return apply_backlight();
}

t5_err_t t5_display_set_backlight_freq(uint32_t hz)
{
    if (!s_ready) return T5_ERR_STATE;
    if (hz < T5_DISPLAY_BL_HZ_MIN || hz > T5_DISPLAY_BL_HZ_MAX) return T5_ERR_INVALID_ARG;
    T5_RETURN_ON_ERR(t5_display_hw_backlight_freq(hz));
    return apply_backlight();
}

int t5_display_brightness(void)
{
    return s_brightness;
}

t5_err_t t5_display_sleep(void)
{
    if (!s_ready) return T5_ERR_STATE;
    if (s_asleep) return T5_OK;
    T5_RETURN_ON_ERR(t5_display_hw_backlight(0));
    T5_RETURN_ON_ERR(t5_display_hw_sleep(true));
    s_asleep = true;
    return T5_OK;
}

t5_err_t t5_display_wake(void)
{
    if (!s_ready) return T5_ERR_STATE;
    if (!s_asleep) return T5_OK;
    T5_RETURN_ON_ERR(t5_display_hw_sleep(false));
    s_asleep = false;
    return apply_backlight();
}

bool t5_display_asleep(void)
{
    return s_asleep;
}

uint16_t *t5_display_fb(void)
{
    return s_fb;
}

uint16_t *t5_display_fb_n(int i)
{
    return (i >= 0 && i < T5_DISPLAY_FBS) ? s_fbs[i] : NULL;
}

t5_err_t t5_display_present(int i, bool cache_sync)
{
    if (!s_ready || i < 0 || i >= T5_DISPLAY_FBS || !s_fbs[i]) return T5_ERR_STATE;
    T5_RETURN_ON_ERR(t5_display_hw_present(s_fbs[i], cache_sync));
    s_queued = i;
    s_fb = s_fbs[i];
    return T5_OK;
}

int t5_display_scanout(void)
{
    return s_scanout;
}

int t5_display_queued(void)
{
    return s_queued;
}

uint32_t t5_display_frames(void)
{
    return s_frames;
}

bool t5_display_wait_shown(uint32_t timeout_ms)
{
    if (s_shown) return true;
    if (!s_ready) return false;
    return t5_display_hw_wait_shown(timeout_ms) || s_shown;
}

/* The DMA restarts on the queued buffer when a frame completes. */
bool t5_display_frame_isr(void)
{
    s_scanout = s_queued;
    s_frames++;
    if (s_shown || s_hold) return false;
    s_shown = true;
    return true;
}

static t5_err_t flush_rows(int y0, int y1)
{
    if (!s_fb) return T5_ERR_STATE;
    if (y0 < 0) y0 = 0;
    if (y1 > T5_DISPLAY_H) y1 = T5_DISPLAY_H;
    if (y0 >= y1) return T5_OK;
    return t5_display_hw_flush(s_fb, y0, y1);
}

t5_err_t t5_display_fill_rect(int x, int y, int w, int h, uint16_t c)
{
    if (!s_fb) return T5_ERR_STATE;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > T5_DISPLAY_W ? T5_DISPLAY_W : x + w;
    int y1 = y + h > T5_DISPLAY_H ? T5_DISPLAY_H : y + h;
    if (x0 >= x1 || y0 >= y1) return T5_OK;
    for (int yy = y0; yy < y1; yy++) {
        uint16_t *row = s_fb + (size_t)yy * T5_DISPLAY_W;
        for (int xx = x0; xx < x1; xx++) {
            row[xx] = c;
        }
    }
    return flush_rows(y0, y1);
}

t5_err_t t5_display_read_ids(uint8_t id[3], uint8_t *power_mode)
{
    if (!s_ready) return T5_ERR_STATE;
    T5_RETURN_ON_ERR(t5_display_hw_read(0x04, id, 3));
    return t5_display_hw_read(0x0A, power_mode, 1);
}

void t5_display_reset_for_test(void)
{
    s_ready = false;
    s_asleep = false;
    s_blank = false;
    s_dim = -1;
    s_fb = NULL;
    for (int i = 0; i < T5_DISPLAY_FBS; i++) s_fbs[i] = NULL;
    s_scanout = s_queued = 0;
    s_frames = 0;
    s_shown = false;
    s_hold = true;
    s_brightness = 80;
}
