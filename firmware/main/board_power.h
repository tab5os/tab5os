/* SPDX-License-Identifier: Apache-2.0
 * The composition root's power wiring: the power and battery services
 * publish what they want (backlight, standby, low-power mode, the charge
 * screen) and this connects it to the display, touch, IMU and UI, which don't
 * know about power at all. Host-tested with the services (test/host/test_power.c).
 */
#pragma once

/* Install the outputs and hooks; call before t5_power_init(). */
void board_power_wire(void);
