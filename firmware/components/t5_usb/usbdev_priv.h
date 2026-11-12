/* SPDX-License-Identifier: Apache-2.0
 * USB-C device mode between usbc.c (the port) and usbdev.c (the app's
 * device).
 */
#pragma once
#include "t5_core/err.h"
#include "tinyusb.h"

/* usbc.c: take USB-C for device mode (T5_ERR_BUSY unless it's the
 * console), and give it back. */
t5_err_t usbc_device_begin(const tinyusb_config_t *tc);
void usbc_device_end(void);

/* usbdev.c: end the open device for the person (Quick Settings, or
 * t5_usbc_set_mode(CONSOLE)); the app's handle stays, and calls on it
 * return T5_ERR_STATE. */
void usbdev_revoke(void);

/* usbdev.c: once at boot, from usbc_init. */
void usbdev_init(void);
