/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* LVGL 9 key codes (checked against lv_group.h in usb_input.c). */
#define T5_KEY_UP 17
#define T5_KEY_DOWN 18
#define T5_KEY_RIGHT 19
#define T5_KEY_LEFT 20
#define T5_KEY_ESC 27
#define T5_KEY_DEL 127
#define T5_KEY_BACKSPACE 8
#define T5_KEY_ENTER 10
#define T5_KEY_NEXT 9
#define T5_KEY_PREV 11
#define T5_KEY_HOME 2
#define T5_KEY_END 3

/* A USB-MIDI 4-byte event packet to a MIDI message; returns its length (0
 * for padding or unknown). */
size_t usb_midi_parse(const uint8_t pkt[4], uint8_t out[3]);
/* A MIDI message to a packet on cable 0; false if not supported. */
bool usb_midi_pack(const uint8_t *msg, size_t len, uint8_t pkt[4]);
/* HID keyboard usage (with shift) to an LVGL key or character, 0 if none. */
uint32_t usb_hid_to_lv_key(uint8_t usage, bool shift);
