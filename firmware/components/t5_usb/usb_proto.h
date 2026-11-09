/* SPDX-License-Identifier: Apache-2.0
 * The parts of t5_usb that build without the USB host stack (host-tested).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_usb/hid.h"

/* USB Audio 1.0: the audio-class subclass of a MIDI interface. */
#define USB_SUBCLASS_MIDISTREAMING 3

/* A USB-MIDI 4-byte event packet to a MIDI message; returns its length (0
 * for padding or unknown). */
size_t usb_midi_parse(const uint8_t pkt[4], uint8_t out[3]);
/* A MIDI message to a packet on cable 0; false if not supported. */
bool usb_midi_pack(const uint8_t *msg, size_t len, uint8_t pkt[4]);
/* HID keyboard usage (with shift) to a character or T5_HID_KEY_*, 0 if none. */
uint32_t usb_hid_key(uint8_t usage, bool shift);

/* usb_hid.c: boot-protocol reports, on the HID driver's task. */
void usb_hid_attached(t5_hid_dev_t dev, bool on);
void usb_hid_keyboard_report(const uint8_t *r, size_t len);
void usb_hid_mouse_report(const uint8_t *r, size_t len);
