/* SPDX-License-Identifier: Apache-2.0
 * Host tests only.
 */
#pragma once

/* Forget the probe result, so the next t5_bsp_panel_probe() looks again. */
void t5_bsp_panel_reset_for_test(void);
