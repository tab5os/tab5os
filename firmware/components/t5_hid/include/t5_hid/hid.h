/* SPDX-License-Identifier: Apache-2.0
 * HID devices the Tab5 plays, for any transport (USB-C device mode,
 * Bluetooth LE HID): report descriptors, report packing and text to key
 * strokes. Pure: no hardware, any task.
 * In the SDK through t5/hid.h (API level 2).
 *
 * A descriptor is built from parts (T5_HID_PART_*). Combined, each part's
 * reports carry its fixed report ID (T5_HID_ID_*); a single part may be
 * built without IDs (T5_HID_NO_IDS), which a boot keyboard or mouse needs.
 * The pack functions write the report body, without the ID: transports take
 * the ID separately.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- descriptors ---------------------------------------------------------- */
#define T5_HID_PART_KEYBOARD 0x01   /* 8 modifiers, 6 keys; LED output report */
#define T5_HID_PART_MOUSE 0x02      /* 5 buttons, 16-bit relative x/y, wheel, pan */
#define T5_HID_PART_CONSUMER 0x04   /* one consumer-page usage (media keys) */
#define T5_HID_PART_GAMEPAD 0x08    /* 16 buttons, 2 sticks, 2 triggers, a hat */
#define T5_HID_PART_ABSOLUTE 0x10   /* absolute pointer 0..32767, 3 buttons (works on macOS too) */
#define T5_HID_PART_ALL 0x1f
#define T5_HID_NO_IDS 0x80          /* only with one part */

#define T5_HID_ID_KEYBOARD 1
#define T5_HID_ID_MOUSE 2
#define T5_HID_ID_CONSUMER 3
#define T5_HID_ID_GAMEPAD 4
#define T5_HID_ID_ABSOLUTE 5

/* Report body sizes (without the ID byte). */
#define T5_HID_KEYBOARD_LEN 8
#define T5_HID_KEYBOARD_OUT_LEN 1
#define T5_HID_MOUSE_LEN 7
#define T5_HID_CONSUMER_LEN 2
#define T5_HID_GAMEPAD_LEN 9
#define T5_HID_ABSOLUTE_LEN 5
#define T5_HID_DESC_MAX 400         /* every part together fits */

/* The descriptor for parts into out. Returns its length, or a negative
 * t5_err_t: T5_ERR_INVALID_ARG for no part, an unknown bit or NO_IDS with
 * several parts, T5_ERR_NO_MEM when cap is too small. */
int t5_hid_descriptor(uint32_t parts, uint8_t *out, size_t cap);
/* The report ID a part's input reports carry in a descriptor built with
 * these parts: 0 with T5_HID_NO_IDS, -1 if part isn't in parts. */
int t5_hid_report_id(uint32_t parts, uint32_t part);

/* ---- keyboard ------------------------------------------------------------- */
/* Modifier bits, the first byte of a keyboard report. */
#define T5_HID_MOD_LCTRL 0x01
#define T5_HID_MOD_LSHIFT 0x02
#define T5_HID_MOD_LALT 0x04
#define T5_HID_MOD_LGUI 0x08        /* Windows / Command */
#define T5_HID_MOD_RCTRL 0x10
#define T5_HID_MOD_RSHIFT 0x20
#define T5_HID_MOD_RALT 0x40        /* AltGr on most non-US layouts */
#define T5_HID_MOD_RGUI 0x80

/* Some usage IDs (HID Usage Tables, keyboard page 0x07). Letters run
 * T5_HID_KEY_A..+25, digits 1..9 then 0 from T5_HID_KEY_1. */
#define T5_HID_KEY_A 0x04
#define T5_HID_KEY_1 0x1e
#define T5_HID_KEY_ENTER 0x28
#define T5_HID_KEY_ESC 0x29
#define T5_HID_KEY_BACKSPACE 0x2a
#define T5_HID_KEY_TAB 0x2b
#define T5_HID_KEY_SPACE 0x2c
#define T5_HID_KEY_CAPS_LOCK 0x39
#define T5_HID_KEY_F1 0x3a          /* ..F12 = 0x45 */
#define T5_HID_KEY_PRINT_SCREEN 0x46
#define T5_HID_KEY_INSERT 0x49
#define T5_HID_KEY_HOME 0x4a
#define T5_HID_KEY_PAGE_UP 0x4b
#define T5_HID_KEY_DELETE 0x4c
#define T5_HID_KEY_END 0x4d
#define T5_HID_KEY_PAGE_DOWN 0x4e
#define T5_HID_KEY_RIGHT 0x4f
#define T5_HID_KEY_LEFT 0x50
#define T5_HID_KEY_DOWN 0x51
#define T5_HID_KEY_UP 0x52
#define T5_HID_KEY_LEFT_CTRL 0xe0   /* ..0xe7: the modifiers as keys */

/* LED bits in the keyboard output report. */
#define T5_HID_LED_NUM_LOCK 0x01
#define T5_HID_LED_CAPS_LOCK 0x02
#define T5_HID_LED_SCROLL_LOCK 0x04

/* Keys held, as a keyboard report says them. */
typedef struct {
    uint8_t mods;             /* T5_HID_MOD_* */
    uint8_t keys[6];          /* usage IDs, 0 for none */
} t5_hid_keyboard_t;

/* Press or release a key: 0xe0..0xe7 change mods, others take or free a
 * slot. Pressing a seventh key returns T5_ERR_NO_MEM and changes nothing. */
t5_err_t t5_hid_key_press(t5_hid_keyboard_t *k, uint8_t key);
void t5_hid_key_release(t5_hid_keyboard_t *k, uint8_t key);
void t5_hid_pack_keyboard(const t5_hid_keyboard_t *k, uint8_t out[T5_HID_KEYBOARD_LEN]);

/* Keyboard layouts the host may use: the host's setting decides what a key
 * means, so pick the one the host is set to. */
typedef enum {
    T5_HID_LAYOUT_US,
    T5_HID_LAYOUT_UK,
    T5_HID_LAYOUT_DE,
    T5_HID_LAYOUT_FR,
    T5_HID_LAYOUT_COUNT,
} t5_hid_layout_t;
/* The key and modifiers that type a Unicode character (\n is Enter, \t
 * Tab, \b Backspace). T5_ERR_NOT_FOUND if the layout can't type it with
 * one stroke (dead keys aren't used). */
t5_err_t t5_hid_char_key(uint32_t ch, t5_hid_layout_t layout, uint8_t *key, uint8_t *mods);
/* Each stroke for UTF-8 text in order. Stops at the first character the
 * layout can't type: returns the bytes of text consumed, so a return less
 * than strlen(text) points at it. A negative cb return stops too. */
typedef int (*t5_hid_stroke_cb_t)(uint8_t key, uint8_t mods, void *arg);
int t5_hid_text(const char *text, t5_hid_layout_t layout, t5_hid_stroke_cb_t cb, void *arg);

/* ---- other reports -------------------------------------------------------- */
#define T5_HID_BUTTON_LEFT 0x01
#define T5_HID_BUTTON_RIGHT 0x02
#define T5_HID_BUTTON_MIDDLE 0x04
#define T5_HID_BUTTON_BACK 0x08
#define T5_HID_BUTTON_FORWARD 0x10

typedef struct {
    uint8_t buttons;          /* T5_HID_BUTTON_* */
    int16_t dx, dy;           /* -32767..32767 */
    int8_t wheel, pan;        /* scroll up and right are positive */
} t5_hid_mouse_t;
void t5_hid_pack_mouse(const t5_hid_mouse_t *m, uint8_t out[T5_HID_MOUSE_LEN]);

/* Consumer page usages (0 = none held). */
#define T5_HID_CC_PLAY_PAUSE 0x00cd
#define T5_HID_CC_NEXT 0x00b5
#define T5_HID_CC_PREV 0x00b6
#define T5_HID_CC_STOP 0x00b7
#define T5_HID_CC_MUTE 0x00e2
#define T5_HID_CC_VOL_UP 0x00e9
#define T5_HID_CC_VOL_DOWN 0x00ea
#define T5_HID_CC_BRIGHT_UP 0x006f
#define T5_HID_CC_BRIGHT_DOWN 0x0070
void t5_hid_pack_consumer(uint16_t usage, uint8_t out[T5_HID_CONSUMER_LEN]);

#define T5_HID_HAT_NONE 8         /* 0 = up, then clockwise in 45° steps */
typedef struct {
    uint16_t buttons;         /* bit n = button n+1 */
    int8_t lx, ly, rx, ry;    /* -127..127 */
    uint8_t lt, rt;           /* 0..255 */
    uint8_t hat;              /* 0..7, T5_HID_HAT_NONE */
} t5_hid_gamepad_t;
void t5_hid_pack_gamepad(const t5_hid_gamepad_t *g, uint8_t out[T5_HID_GAMEPAD_LEN]);

typedef struct {
    uint8_t buttons;          /* T5_HID_BUTTON_LEFT/RIGHT/MIDDLE */
    uint16_t x, y;            /* 0..32767 across the host's screen */
} t5_hid_absolute_t;
void t5_hid_pack_absolute(const t5_hid_absolute_t *a, uint8_t out[T5_HID_ABSOLUTE_LEN]);

#ifdef __cplusplus
}
#endif
