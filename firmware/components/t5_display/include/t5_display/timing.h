/* SPDX-License-Identifier: Apache-2.0
 * Diagnostics: an ST712x DPI timing override, read at display init (so after
 * storage) and written by the console's `disp timing`. The file holds
 * "<pclk_mhz> <vsync_front_porch>"; values outside these ranges are ignored.
 */
#pragma once
#include "t5_storage/paths.h"

#define T5_DISPLAY_TIMING_FILE T5_DATA_PATH "/display_timing.txt"
#define T5_DISPLAY_PCLK_MHZ_MIN 40
#define T5_DISPLAY_PCLK_MHZ_MAX 90
#define T5_DISPLAY_VFP_MIN 4
#define T5_DISPLAY_VFP_MAX 400
