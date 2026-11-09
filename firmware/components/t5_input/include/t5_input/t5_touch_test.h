/* SPDX-License-Identifier: Apache-2.0
 * Tests only.
 */
#pragma once
#include "t5_bsp/panel.h"
#include "t5_i2c/t5_i2c.h"
#include "t5_input/t5_touch.h"

/* One read from the controller, as the touch task does it. */
t5_err_t t5_touch_sample(t5_touch_state_t *out);
/* Set up the parser state without a task. */
void t5_touch_setup_for_test(t5_i2c_dev_t *dev, const t5_panel_info_t *panel, uint16_t max_x, uint16_t max_y,
                             uint8_t reports);
