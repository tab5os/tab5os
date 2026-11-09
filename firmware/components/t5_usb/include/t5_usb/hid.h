/* SPDX-License-Identifier: Apache-2.0
 * USB HID keyboards and mice, decoded from their boot-protocol reports, for
 * an input adapter (the UI turns them into LVGL input devices). t5_usb knows
 * nothing about the UI: it calls the installed handler.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { T5_HID_KEYBOARD, T5_HID_MOUSE } t5_hid_dev_t;

/* A key is its character (US layout, shift applied) or one of these. */
enum {
    T5_HID_KEY_ENTER = 0x100,
    T5_HID_KEY_ESC,
    T5_HID_KEY_BACKSPACE,
    T5_HID_KEY_TAB,
    T5_HID_KEY_BACKTAB,       /* shift + tab */
    T5_HID_KEY_DEL,
    T5_HID_KEY_HOME,
    T5_HID_KEY_END,
    T5_HID_KEY_UP,
    T5_HID_KEY_DOWN,
    T5_HID_KEY_LEFT,
    T5_HID_KEY_RIGHT,
};

/* Called on the HID driver's task: keep them short. Any member may be NULL.
 *   attached  a keyboard or mouse came or went
 *   key       a key went down (keys don't repeat)
 *   mouse     relative motion in report counts, and the left button */
typedef struct {
    void (*attached)(t5_hid_dev_t dev, bool on, void *arg);
    void (*key)(uint32_t key, void *arg);
    void (*mouse)(int dx, int dy, bool button, void *arg);
    void *arg;
} t5_usb_hid_handler_t;

/* Install the handler (NULL removes it). Devices already attached are
 * reported to it at once. */
void t5_usb_set_hid_handler(const t5_usb_hid_handler_t *h);

#ifdef __cplusplus
}
#endif
