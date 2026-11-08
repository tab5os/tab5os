/* SPDX-License-Identifier: Apache-2.0
 * HID keyboards and mice as LVGL input devices (plan 7.7, 6.2).
 *
 * Keyboard: boot-protocol reports become key presses (US layout) queued for
 * an LVGL keypad device on the default group, so a focused text area gets
 * the characters and Tab moves focus. While one is attached the shell keeps
 * the on-screen keyboard hidden.
 * Mouse: relative reports move an LVGL pointer with a cursor.
 * The LVGL objects are made on the UI task; the reports arrive on the HID
 * driver's task and go through a queue.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include "md/md.h"
#include "t5_power/power.h"
#include "t5_ui/t5_ui.h"
#include "usb/hid_usage_keyboard.h"
#include "usb_priv.h"

_Static_assert(T5_KEY_ENTER == LV_KEY_ENTER && T5_KEY_UP == LV_KEY_UP && T5_KEY_NEXT == LV_KEY_NEXT &&
                   T5_KEY_HOME == LV_KEY_HOME && T5_KEY_DEL == LV_KEY_DEL, "key codes");

/* ---- keyboard ---------------------------------------------------------------------- */
typedef struct {
    uint32_t key;
    bool pressed;
} key_ev_t;

static QueueHandle_t s_keys;
static lv_indev_t *s_kb_indev, *s_mouse_indev;
static lv_obj_t *s_cursor;
static volatile bool s_kb_present, s_mouse_present;
static uint8_t s_prev[6];
static volatile int32_t s_mx, s_my;
static volatile bool s_mbtn;

static void kb_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static key_ev_t last;
    key_ev_t ev;
    if (xQueueReceive(s_keys, &ev, 0) == pdTRUE) {
        last = ev;
        data->continue_reading = uxQueueMessagesWaiting(s_keys) > 0;
    }
    data->key = last.key;
    data->state = last.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void kb_create(void *p)
{
    (void)p;
    if (s_kb_indev) return;
    lv_group_t *g = lv_group_get_default();
    if (!g) {
        g = lv_group_create();
        lv_group_set_default(g);
    }
    s_kb_indev = lv_indev_create();
    lv_indev_set_type(s_kb_indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(s_kb_indev, kb_read);
    lv_indev_set_group(s_kb_indev, g);
}

static void kb_delete(void *p)
{
    (void)p;
    if (s_kb_indev) {
        lv_indev_delete(s_kb_indev);
        s_kb_indev = NULL;
    }
}

bool t5_usb_keyboard_present(void)
{
    return s_kb_present;
}

void usb_input_keyboard_attached(bool on)
{
    s_kb_present = on;
    memset(s_prev, 0, sizeof s_prev);
    t5_ui_async(on ? kb_create : kb_delete, NULL);
}

void usb_input_keyboard_report(const uint8_t *r, size_t len)
{
    if (len < 8) return;
    bool shift = r[0] & 0x22;            /* left or right shift */
    t5_power_user_activity();
    for (int i = 2; i < 8; i++) {
        uint8_t u = r[i];
        if (u < 4) continue;
        bool was = false;
        for (int k = 0; k < 6; k++) was |= s_prev[k] == u;
        if (was) continue;
        uint32_t key = usb_hid_to_lv_key(u, shift);
        if (!key) continue;
        key_ev_t down = { key, true }, up = { key, false };
        xQueueSend(s_keys, &down, 0);
        xQueueSend(s_keys, &up, 0);
    }
    memcpy(s_prev, &r[2], 6);
}

/* ---- mouse ---------------------------------------------------------------------------- */
static void mouse_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    lv_display_t *d = lv_display_get_default();
    int32_t w = lv_display_get_horizontal_resolution(d), h = lv_display_get_vertical_resolution(d);
    if (s_mx < 0) s_mx = 0;
    if (s_my < 0) s_my = 0;
    if (s_mx >= w) s_mx = w - 1;
    if (s_my >= h) s_my = h - 1;
    data->point.x = s_mx;
    data->point.y = s_my;
    data->state = s_mbtn ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void mouse_create(void *p)
{
    (void)p;
    if (s_mouse_indev) return;
    s_mouse_indev = lv_indev_create();
    lv_indev_set_type(s_mouse_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_mouse_indev, mouse_read);
    s_cursor = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(s_cursor);
    lv_obj_set_size(s_cursor, T5_DP(12), T5_DP(12));
    lv_obj_set_style_radius(s_cursor, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_cursor, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_cursor, LV_OPA_80, 0);
    lv_obj_set_style_border_color(s_cursor, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_cursor, T5_DP(1), 0);
    lv_obj_set_clickable(s_cursor, false);
    lv_indev_set_cursor(s_mouse_indev, s_cursor);
}

static void mouse_delete(void *p)
{
    (void)p;
    if (s_mouse_indev) {
        lv_indev_delete(s_mouse_indev);
        s_mouse_indev = NULL;
    }
    if (s_cursor) {
        lv_obj_delete(s_cursor);
        s_cursor = NULL;
    }
}

bool t5_usb_mouse_present(void)
{
    return s_mouse_present;
}

void usb_input_mouse_attached(bool on)
{
    s_mouse_present = on;
    if (on) {
        lv_display_t *d = lv_display_get_default();
        if (d) {
            s_mx = lv_display_get_horizontal_resolution(d) / 2;
            s_my = lv_display_get_vertical_resolution(d) / 2;
        }
    }
    t5_ui_async(on ? mouse_create : mouse_delete, NULL);
}

void usb_input_mouse_report(const uint8_t *r, size_t len)
{
    if (len < 3) return;
    s_mbtn = r[0] & 1;
    /* Boot mice move in counts; two px a count feels right at 2 px/dp. */
    s_mx += (int8_t)r[1] * 2;
    s_my += (int8_t)r[2] * 2;
    t5_power_user_activity();
}

void usb_input_init(void)
{
    s_keys = xQueueCreate(32, sizeof(key_ev_t));
}
