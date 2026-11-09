/* SPDX-License-Identifier: Apache-2.0
 * HID keyboards and mice: boot-protocol reports to keys and motion for the
 * installed handler (t5_usb/hid.h). Reports arrive on the HID driver's task.
 */
#include <string.h>
#include "t5_core/os.h"
#include "t5_power/power.h"
#include "t5_usb/usb.h"
#include "usb_proto.h"

static t5_mutex_t s_mx;
static t5_usb_hid_handler_t s_h;
static volatile bool s_kb_present, s_mouse_present;
static uint8_t s_prev[6];

/* Created on first use: the UI installs its handler whether or not
 * t5_usb_init ran (or succeeded). */
static t5_mutex_t mx(void)
{
    return t5_mutex_once(&s_mx);
}

static t5_usb_hid_handler_t handler(void)
{
    t5_mutex_t m = mx();
    t5_mutex_lock(m);
    t5_usb_hid_handler_t h = s_h;
    t5_mutex_unlock(m);
    return h;
}

void t5_usb_set_hid_handler(const t5_usb_hid_handler_t *h)
{
    t5_mutex_t m = mx();
    t5_mutex_lock(m);
    s_h = h ? *h : (t5_usb_hid_handler_t){ 0 };
    t5_usb_hid_handler_t now = s_h;
    t5_mutex_unlock(m);
    if (!now.attached) return;
    if (s_kb_present) now.attached(T5_HID_KEYBOARD, true, now.arg);
    if (s_mouse_present) now.attached(T5_HID_MOUSE, true, now.arg);
}

bool t5_usb_keyboard_present(void)
{
    return s_kb_present;
}

bool t5_usb_mouse_present(void)
{
    return s_mouse_present;
}

void usb_hid_attached(t5_hid_dev_t dev, bool on)
{
    if (dev == T5_HID_KEYBOARD) {
        s_kb_present = on;
        memset(s_prev, 0, sizeof s_prev);
    } else {
        s_mouse_present = on;
    }
    t5_usb_hid_handler_t h = handler();
    if (h.attached) h.attached(dev, on, h.arg);
}

void usb_hid_keyboard_report(const uint8_t *r, size_t len)
{
    if (len < 8) return;
    bool shift = r[0] & 0x22;            /* left or right shift */
    t5_power_user_activity();
    t5_usb_hid_handler_t h = handler();
    for (int i = 2; i < 8; i++) {
        uint8_t u = r[i];
        if (u < 4) continue;             /* none, or a rollover error */
        bool was = false;
        for (int k = 0; k < 6; k++) was |= s_prev[k] == u;
        if (was) continue;
        uint32_t key = usb_hid_key(u, shift);
        if (key && h.key) h.key(key, h.arg);
    }
    memcpy(s_prev, &r[2], 6);
}

void usb_hid_mouse_report(const uint8_t *r, size_t len)
{
    if (len < 3) return;
    t5_power_user_activity();
    t5_usb_hid_handler_t h = handler();
    if (h.mouse) h.mouse((int8_t)r[1], (int8_t)r[2], r[0] & 1, h.arg);
}
