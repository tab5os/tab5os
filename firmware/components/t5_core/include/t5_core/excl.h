/* SPDX-License-Identifier: Apache-2.0
 * Exclusive control (API level 2; in the SDK through t5/excl.h): an app
 * takes the whole USB-C port, USB-A bus, Wi-Fi radio or Bluetooth
 * controller for itself, below the OS's drivers and stacks
 * (docs/io-access-design.md, section 6).
 *
 * A claim asks the person the first time, naming what stops meanwhile
 * (needs usb for USB-C and USB-A, net for Wi-Fi, ble for Bluetooth). Then
 * the OS lets go: its own use of that hardware stops, other apps' handles
 * on it stop working, and other claims get T5_ERR_BUSY. The claim lasts
 * until released, until the app exits or crashes, or until the person
 * stops it in Quick Settings (t5_excl_active() turns false and the calls
 * that take the handle return T5_ERR_STATE). On release the OS resets the
 * hardware and starts its own drivers again.
 *
 * What the claim unlocks is in each service's header: t5_usb/usb.h
 * (t5_usba_*), t5_usb/usbdev.h (t5_usbc_raw_*), t5_net/wifimon.h
 * (t5_wifi_excl_*), t5_net/ble.h (t5_bt_hci_*). Blocks while the OS lets
 * go (a second or two). Any task, not the UI task (the first question).
 */
#pragma once
#include <stdbool.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    T5_EXCL_USBC,             /* USB-C as a device of the app's own design; console and sound off */
    T5_EXCL_USBA,             /* every device on USB-A; the OS's USB drivers off */
    T5_EXCL_WIFI,             /* the radio: modes, channels, power; the OS's network off */
    T5_EXCL_BT,               /* the Bluetooth controller over HCI; the OS's Bluetooth off */
    T5_EXCL_COUNT,
} t5_excl_kind_t;

typedef struct t5_excl t5_excl_t;

t5_err_t t5_excl_claim(t5_excl_kind_t what, t5_excl_t **out);
/* Still held (false once the person stopped it). */
bool t5_excl_active(t5_excl_t *h);
t5_err_t t5_excl_release(t5_excl_t *h);

#ifdef __cplusplus
}
#endif
