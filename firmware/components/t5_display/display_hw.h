/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "t5_display/t5_display.h"

t5_err_t t5_display_hw_init(t5_panel_t panel, uint16_t *fbs[T5_DISPLAY_FBS]);
/* Queue fb (one of the DPI buffers) for scanout from the next frame. */
t5_err_t t5_display_hw_present(uint16_t *fb, bool cache_sync);
/* Frame-done interrupt: display.c's handler. True on the first frame done
 * after the hold was released, which t5_display_hw_wait_shown() waits for. */
bool t5_display_frame_isr(void);
bool t5_display_hw_wait_shown(uint32_t timeout_ms);
t5_err_t t5_display_hw_backlight(int percent);
t5_err_t t5_display_hw_sleep(bool sleep);
t5_err_t t5_display_hw_flush(uint16_t *fb, int y0, int y1);
t5_err_t t5_display_hw_read(uint8_t cmd, uint8_t *buf, int len);
t5_err_t t5_display_hw_backlight_freq(uint32_t hz);
