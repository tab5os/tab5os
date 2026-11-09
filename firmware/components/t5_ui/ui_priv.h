/* SPDX-License-Identifier: Apache-2.0
 * Shared between t5_ui's files (ui_md.c also by the simulator's t5_ui).
 */
#pragma once
#include "t5_core/err.h"

/* hid_input.c: USB keyboards and mice as LVGL input devices. */
t5_err_t ui_hid_input_init(void);
/* ui_md.c: md's ripple and settled probes. */
void ui_md_probes_init(void);
