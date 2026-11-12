/* SPDX-License-Identifier: Apache-2.0
 * Who holds a port or radio beyond what the OS shares (USB-C device mode,
 * Bluetooth HID, the Wi-Fi monitor, the exclusive claims), so the shell can
 * show it and the person can stop it (docs/io-access-design.md). The
 * service that hands the hardware out records the holder and how to take it
 * back; the shell lists holds and calls stop. Any task. Not for apps.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    T5_IO_USBC_DEVICE,        /* t5_usb/usbdev.h */
    T5_IO_BLE_HID,            /* Bluetooth keyboard/mouse */
    T5_IO_WIFI_MONITOR,       /* Wi-Fi monitor mode */
    T5_IO_EXCL_USBC,          /* the exclusive claims */
    T5_IO_EXCL_USBA,
    T5_IO_EXCL_WIFI,
    T5_IO_EXCL_BT,
    T5_IO_COUNT,
} t5_io_kind_t;

/* Takes the hardware back; runs on a short-lived task of its own, so it
 * may block (it should finish within a few seconds). */
typedef void (*t5_io_stop_fn_t)(void);

/* Record the holder (its name as the person knows it, "" for the system)
 * and how to stop it; who NULL clears the hold. */
void t5_io_hold_set(t5_io_kind_t kind, const char *who, t5_io_stop_fn_t stop);
/* Held? Copies the holder's name. */
bool t5_io_hold_get(t5_io_kind_t kind, char *who, size_t n);
/* Ask the holder's stop function to run (returns at once). */
void t5_io_hold_stop(t5_io_kind_t kind);
/* What the shell shows: "USB-C as a USB device", ... */
const char *t5_io_kind_text(t5_io_kind_t kind);

#ifdef __cplusplus
}
#endif
