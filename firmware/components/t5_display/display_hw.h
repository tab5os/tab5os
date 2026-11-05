/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "t5_display/t5_display.h"

t5_err_t t5_display_hw_init(t5_panel_t panel, uint16_t **fb_out);
t5_err_t t5_display_hw_backlight(int percent);
t5_err_t t5_display_hw_sleep(bool sleep);
t5_err_t t5_display_hw_flush(uint16_t *fb, int y0, int y1);
t5_err_t t5_display_hw_read(uint8_t cmd, uint8_t *buf, int len);
t5_err_t t5_display_hw_backlight_freq(uint32_t hz);
