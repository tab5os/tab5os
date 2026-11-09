/* SPDX-License-Identifier: Apache-2.0
 * USB keyboards and mice as LVGL input devices (plan 7.7, 6.2).
 *
 * Keyboard: keys are queued for an LVGL keypad device on the default group,
 * so a focused text area gets the characters and Tab moves focus. While one
 * is attached the shell keeps the on-screen keyboard hidden.
 * Mouse: relative motion moves an LVGL pointer with a cursor.
 * The LVGL objects are made on the UI task; keys and motion arrive on the
 * HID driver's task. Each new input device posts T5_EV_UI_INPUT_ADDED.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include "md/md_tokens.h"
#include "t5_core/event.h"
#include "t5_core/log.h"
#include "t5_ui/t5_ui.h"
#include "t5_usb/hid.h"
#include "ui_priv.h"

typedef struct {
    uint32_t key;
    bool pressed;
} key_ev_t;

static const char *TAG = "t5.hid";

static QueueHandle_t s_keys;
static lv_indev_t *s_kb_indev, *s_mouse_indev;
static lv_obj_t *s_cursor;
static volatile int32_t s_mx, s_my;
static volatile bool s_mbtn;

static uint32_t lv_key(uint32_t k)
{
    switch (k) {
    case T5_HID_KEY_ENTER: return LV_KEY_ENTER;
    case T5_HID_KEY_ESC: return LV_KEY_ESC;
    case T5_HID_KEY_BACKSPACE: return LV_KEY_BACKSPACE;
    case T5_HID_KEY_TAB: return LV_KEY_NEXT;
    case T5_HID_KEY_BACKTAB: return LV_KEY_PREV;
    case T5_HID_KEY_DEL: return LV_KEY_DEL;
    case T5_HID_KEY_HOME: return LV_KEY_HOME;
    case T5_HID_KEY_END: return LV_KEY_END;
    case T5_HID_KEY_UP: return LV_KEY_UP;
    case T5_HID_KEY_DOWN: return LV_KEY_DOWN;
    case T5_HID_KEY_LEFT: return LV_KEY_LEFT;
    case T5_HID_KEY_RIGHT: return LV_KEY_RIGHT;
    default: return k < 0x100 ? k : 0;
    }
}

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

static void kb_create(void)
{
    if (s_kb_indev) return;
    s_kb_indev = lv_indev_create();
    lv_indev_set_type(s_kb_indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(s_kb_indev, kb_read);
    lv_indev_set_group(s_kb_indev, lv_group_get_default());
    t5_event_post(T5_EV_UI_INPUT_ADDED, NULL, 0);
}

bool t5_ui_hw_keyboard(void)
{
    return s_kb_indev != NULL;
}

static void kb_delete(void)
{
    if (s_kb_indev) {
        lv_indev_delete(s_kb_indev);
        s_kb_indev = NULL;
    }
}

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

static void mouse_create(void)
{
    if (s_mouse_indev) return;
    lv_display_t *d = lv_display_get_default();
    if (d) {
        s_mx = lv_display_get_horizontal_resolution(d) / 2;
        s_my = lv_display_get_vertical_resolution(d) / 2;
    }
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
    t5_event_post(T5_EV_UI_INPUT_ADDED, NULL, 0);
}

static void mouse_delete(void)
{
    if (s_mouse_indev) {
        lv_indev_delete(s_mouse_indev);
        s_mouse_indev = NULL;
    }
    if (s_cursor) {
        lv_obj_delete(s_cursor);
        s_cursor = NULL;
    }
}

/* t5_ui_async's argument encodes the device and the direction. */
#define ATTACH_ON 0x100u

static void attach_on_ui(void *p)
{
    uintptr_t v = (uintptr_t)p;
    bool on = v & ATTACH_ON;
    if ((t5_hid_dev_t)(v & 0xFF) == T5_HID_KEYBOARD) {
        if (on) kb_create();
        else kb_delete();
    } else if (on) {
        mouse_create();
    } else {
        mouse_delete();
    }
}

static void on_attached(t5_hid_dev_t dev, bool on, void *arg)
{
    (void)arg;
    if (dev == T5_HID_KEYBOARD) xQueueReset(s_keys);
    t5_err_t e = t5_ui_async(attach_on_ui, (void *)(uintptr_t)((uint32_t)dev | (on ? ATTACH_ON : 0)));
    if (e != T5_OK) T5_LOGE(TAG, "HID %s %s: %s", dev == T5_HID_KEYBOARD ? "keyboard" : "mouse", on ? "attach" : "detach", t5_err_str(e));
}

static void on_key(uint32_t key, void *arg)
{
    (void)arg;
    uint32_t k = lv_key(key);
    if (!k) return;
    /* Both or neither: a press without its release would repeat forever.
     * This is the only producer, so the space can't shrink in between. */
    if (uxQueueSpacesAvailable(s_keys) < 2) return;
    key_ev_t down = { k, true }, up = { k, false };
    xQueueSend(s_keys, &down, 0);
    xQueueSend(s_keys, &up, 0);
}

/* Boot mice move in counts; two px a count feels right at 2 px/dp. */
#define PX_PER_COUNT 2

static void on_mouse(int dx, int dy, bool button, void *arg)
{
    (void)arg;
    s_mbtn = button;
    s_mx += dx * PX_PER_COUNT;
    s_my += dy * PX_PER_COUNT;
}

t5_err_t ui_hid_input_init(void)
{
    s_keys = xQueueCreate(32, sizeof(key_ev_t));
    if (!s_keys) return T5_ERR_NO_MEM;
    const t5_usb_hid_handler_t h = { .attached = on_attached, .key = on_key, .mouse = on_mouse };
    t5_usb_set_hid_handler(&h);
    return T5_OK;
}
