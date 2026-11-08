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
bool usb_uvc_streaming(void);

/* usb_midi.c (on the usb.t5 client task) */
void usb_midi_init(void);
void usb_midi_check(usb_device_handle_t h, uint8_t addr, const usb_config_desc_t *cfg);
void usb_midi_release_if_unused(usb_device_handle_t h, uint8_t addr);
void usb_midi_gone(usb_device_handle_t h);

/* usb_input.c */
void usb_input_init(void);
void usb_input_keyboard_attached(bool on);
void usb_input_mouse_attached(bool on);
void usb_input_keyboard_report(const uint8_t *r, size_t len);
void usb_input_mouse_report(const uint8_t *r, size_t len);

/* usbc.c */
void usbc_init(void);
