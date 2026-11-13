/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_usb/usb.h"
#include "usb/usb_host.h"
#include "usb_proto.h"

usb_host_client_handle_t usb_client(void);
void usb_dev_set_class(uint8_t addr, t5_usb_class_t cls, bool driver);

/* usb_midi.c (on the usb.t5 client task) */
void usb_midi_init(void);
void usb_midi_check(usb_device_handle_t h, uint8_t addr, const usb_config_desc_t *cfg);
void usb_midi_release_if_unused(usb_device_handle_t h, uint8_t addr);
void usb_midi_gone(usb_device_handle_t h);

/* usb_midi.c: hand the MIDI interface to an app (and back). */
bool usb_midi_owns(uint8_t addr, uint8_t iface);
void usb_midi_detach(void);

/* usb.c: who has an interface, for raw claims (usbraw.c). */
typedef enum {
    USB_OWNER_NONE,           /* nobody the OS knows: free, or another app's */
    USB_OWNER_BUSY,           /* an OS driver that can't let go now (drive, serial, camera) */
    USB_OWNER_HID,            /* a boot keyboard or mouse */
    USB_OWNER_MIDI,
} usb_owner_t;
usb_owner_t usb_iface_owner(uint8_t addr, uint8_t iface);
/* The owner it had before an app took it over. */
usb_owner_t usb_iface_owner_was(uint8_t addr, uint8_t iface);
t5_err_t usb_iface_detach(uint8_t addr, uint8_t iface, usb_owner_t owner);
void usb_iface_reattach(usb_device_handle_t dev, uint8_t addr, usb_owner_t owner);

/* usbraw.c (on the usb.t5 client task) */
void usbraw_gone(usb_device_handle_t dev);

/* usbc.c */
void usbc_init(void);
