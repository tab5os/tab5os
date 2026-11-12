/* Keyboard & Trackpad: the Tab5 as a computer's keyboard, trackpad and
 * media remote, over USB-C (t5/usbdev.h) or Bluetooth (t5/ble.h HID), with
 * the reports built by t5/hid.h.
 *
 *   The big panel is a trackpad. The gestures are recognised here, in the
 *   app, from every finger on the screen (t5_ui_touch_get):
 *     one finger moves the pointer; a quick tap clicks
 *     two fingers scroll (up/down and sideways); a two-finger tap right-clicks
 *   Type: sends the text field's contents as key strokes (US layout).
 *   The media buttons send consumer-control keys.
 *   "Use Bluetooth" switches transport: USB-C turns the console and sound
 *   off while it runs; Bluetooth pairs from the computer's settings.
 *
 * A 10 ms LVGL timer reads the fingers and queues reports; a worker task
 * owns the transport and sends them. The OS releases every key if the app
 * goes to the background and closes the transport when it exits.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <t5/app.h>
#include <t5/ble.h>
#include <t5/hid.h>
#include <t5/ui.h>
#include <t5/usbdev.h>

typedef struct {
    uint32_t part;            /* T5_HID_PART_* */
    uint8_t len;
    uint8_t data[T5_HID_GAMEPAD_LEN];
} report_t;

static struct {
    t5_app_t *app;
    lv_obj_t *pad, *status, *text, *bt;
    QueueHandle_t q;
    volatile bool want_bt, switch_now;
    /* trackpad state (UI task) */
    int fingers;
    uint8_t id0;
    int16_t x0, y0, mx, my;           /* first finger now; two-finger midpoint */
    uint32_t down_ms;
    int max_fingers, moved;
    int scroll_acc_y, scroll_acc_x;
} s;

/* ---- sending (any task) ------------------------------------------------------- */
static void queue_report(uint32_t part, const uint8_t *data, uint8_t len)
{
    report_t r = { .part = part, .len = len };
    memcpy(r.data, data, len);
    xQueueSend(s.q, &r, 0);                   /* full: dropped (pointer motion catches up) */
}

static void mouse(uint8_t buttons, int dx, int dy, int wheel, int pan)
{
    t5_hid_mouse_t m = { .buttons = buttons, .dx = (int16_t)dx, .dy = (int16_t)dy, .wheel = (int8_t)wheel,
                         .pan = (int8_t)pan };
    uint8_t r[T5_HID_MOUSE_LEN];
    t5_hid_pack_mouse(&m, r);
    queue_report(T5_HID_PART_MOUSE, r, sizeof r);
}

static void click(uint8_t button)
{
    mouse(button, 0, 0, 0, 0);
    mouse(0, 0, 0, 0, 0);
}

/* ---- the trackpad (UI task) --------------------------------------------------- */
#define TAP_MS 180
#define TAP_MOVE 12
#define SCROLL_PX 24

static void pad_timer(lv_timer_t *t)
{
    (void)t;
    t5_ui_touch_t f[T5_UI_TOUCH_MAX];
    int n = t5_ui_touch_get(f, T5_UI_TOUCH_MAX);
    lv_area_t a;
    lv_obj_get_coords(s.pad, &a);
    /* Only fingers on the pad count. */
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (f[i].x >= a.x1 && f[i].x <= a.x2 && f[i].y >= a.y1 && f[i].y <= a.y2) f[k++] = f[i];
    }
    uint32_t now = lv_tick_get();
    if (k && !s.fingers) {                     /* a gesture starts */
        s.down_ms = now;
        s.max_fingers = 0;
        s.moved = 0;
        s.scroll_acc_x = s.scroll_acc_y = 0;
    }
    if (!k && s.fingers) {                     /* it ends: was it a tap? */
        if (now - s.down_ms < TAP_MS && s.moved < TAP_MOVE) click(s.max_fingers >= 2 ? T5_HID_BUTTON_RIGHT : T5_HID_BUTTON_LEFT);
        s.fingers = 0;
        return;
    }
    if (!k) return;
    if (k > s.max_fingers) s.max_fingers = k;
    if (k == 1) {
        if (s.fingers == 1 && f[0].id == s.id0) {
            int dx = f[0].x - s.x0, dy = f[0].y - s.y0;
            s.moved += abs(dx) + abs(dy);
            /* A little acceleration: faster strokes go further. */
            int speed = abs(dx) + abs(dy);
            int mul = speed > 30 ? 3 : speed > 10 ? 2 : 1;
            if (dx || dy) mouse(0, dx * mul, dy * mul, 0, 0);
        }
        s.id0 = f[0].id;
        s.x0 = f[0].x;
        s.y0 = f[0].y;
    } else {
        int16_t mx = (int16_t)((f[0].x + f[1].x) / 2), my = (int16_t)((f[0].y + f[1].y) / 2);
        if (s.fingers >= 2) {
            s.scroll_acc_y += my - s.my;
            s.scroll_acc_x += mx - s.mx;
            s.moved += abs(mx - s.mx) + abs(my - s.my);
            int wheel = s.scroll_acc_y / SCROLL_PX, pan = -s.scroll_acc_x / SCROLL_PX;
            if (wheel || pan) {
                mouse(0, 0, 0, wheel, pan);    /* content follows the fingers */
                s.scroll_acc_y -= wheel * SCROLL_PX;
                s.scroll_acc_x += pan * SCROLL_PX;
            }
        }
        s.mx = mx;
        s.my = my;
    }
    s.fingers = k;
}

/* ---- buttons (UI task) ---------------------------------------------------------- */
static int stroke(uint8_t key, uint8_t mods, void *arg)
{
    (void)arg;
    t5_hid_keyboard_t kb = { .mods = mods };
    kb.keys[0] = key;
    uint8_t r[T5_HID_KEYBOARD_LEN];
    t5_hid_pack_keyboard(&kb, r);
    queue_report(T5_HID_PART_KEYBOARD, r, sizeof r);
    memset(&kb, 0, sizeof kb);
    t5_hid_pack_keyboard(&kb, r);
    queue_report(T5_HID_PART_KEYBOARD, r, sizeof r);
    return 0;
}

static void type_clicked(lv_event_t *e)
{
    (void)e;
    const char *t = lv_textarea_get_text(s.text);
    int done = t5_hid_text(t, T5_HID_LAYOUT_US, stroke, NULL);
    if (done >= 0 && (size_t)done < strlen(t)) md_snackbar_show("Stopped at a character the US layout can't type", NULL, NULL, NULL);
    lv_textarea_set_text(s.text, "");
}

static void media_clicked(lv_event_t *e)
{
    uint16_t usage = (uint16_t)(uintptr_t)lv_event_get_user_data(e);
    uint8_t r[T5_HID_CONSUMER_LEN];
    t5_hid_pack_consumer(usage, r);
    queue_report(T5_HID_PART_CONSUMER, r, sizeof r);
    t5_hid_pack_consumer(0, r);
    queue_report(T5_HID_PART_CONSUMER, r, sizeof r);
}

static void bt_changed(lv_event_t *e)
{
    s.want_bt = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    s.switch_now = true;
}

static void set_status_ui(void *text)
{
    lv_label_set_text(s.status, text);
    free(text);
}

static void status(const char *text)
{
    char *t = strdup(text);
    if (t && t5_ui_async(set_status_ui, t) != T5_OK) free(t);
}

/* ---- the transport (worker task) ------------------------------------------------ */
static uint8_t s_desc[T5_HID_DESC_MAX];

static void worker(void *arg)
{
    (void)arg;
    t5_usbdev_t *usb = NULL;
    t5_ble_hid_t *ble = NULL;
    int desc_len = t5_hid_descriptor(T5_HID_PART_KEYBOARD | T5_HID_PART_MOUSE | T5_HID_PART_CONSUMER, s_desc, sizeof s_desc);
    s.switch_now = true;
    for (;;) {
        if (s.switch_now) {
            s.switch_now = false;
            if (usb) t5_usbdev_close(usb);
            if (ble) t5_ble_hid_close(ble);
            usb = NULL;
            ble = NULL;
            t5_err_t e;
            if (s.want_bt) {
                t5_ble_hid_cfg_t cfg = { .size = sizeof cfg, .name = "Tab5 Keyboard", .appearance = T5_BLE_APPEARANCE_KEYBOARD };
                e = t5_ble_hid_open(&cfg, &ble);
                status(e == T5_OK ? "Bluetooth: pair with \"Tab5 Keyboard\" from the computer's Bluetooth settings."
                                  : "Bluetooth didn't start. Is Bluetooth on in Settings?");
            } else {
                t5_usbdev_cfg_t cfg = { .size = sizeof cfg, .product = "Tab5 Keyboard", .hid_count = 1,
                                        .hid = { { .report_desc = s_desc, .desc_len = (uint16_t)desc_len, .poll_ms = 5 } } };
                e = t5_usbdev_open(&cfg, &usb);
                status(e == T5_OK ? "USB-C: plug the Tab5 into a computer."
                                  : e == T5_ERR_DENIED ? "Not allowed. Turn on \"Act as a USB device\" in App Manager."
                                                       : "USB-C is busy (mass storage?).");
            }
        }
        if (usb) {
            t5_usbdev_ev_t ev;
            uint8_t b[8];
            if (t5_usbdev_next(usb, &ev, b, sizeof b, 0) >= 0) {
                if (ev.type == T5_USBDEV_MOUNTED) status("USB-C: connected. Use the pad and keys below.");
                if (ev.type == T5_USBDEV_UNMOUNTED) status("USB-C: plug the Tab5 into a computer.");
            }
        }
        if (ble) {
            t5_ble_hid_ev_t ev;
            uint8_t b[8];
            if (t5_ble_hid_next(ble, &ev, b, sizeof b, 0) >= 0) {
                if (ev.type == T5_BLE_HID_CONNECTED) status("Bluetooth: connected. Use the pad and keys below.");
                if (ev.type == T5_BLE_HID_DISCONNECTED) status("Bluetooth: waiting for the computer.");
            }
        }
        report_t r;
        if (xQueueReceive(s.q, &r, pdMS_TO_TICKS(20)) != pdTRUE) continue;
        int id = t5_hid_report_id(T5_HID_PART_KEYBOARD | T5_HID_PART_MOUSE | T5_HID_PART_CONSUMER, r.part);
        if (usb && id > 0) T5_IGNORE(t5_usbdev_hid_send(usb, 0, (uint8_t)id, r.data, r.len, 50));
        if (ble) T5_IGNORE(t5_ble_hid_send(ble, r.part, r.data, r.len, 50));
    }
}

/* ---- the screen ----------------------------------------------------------------- */
static void back(lv_event_t *e)
{
    (void)e;
    t5_app_exit(t5_app_self());
}

static lv_obj_t *media_button(lv_obj_t *row, const char *icon, uint16_t usage)
{
    lv_obj_t *b = md_fab_create(row, icon, true);
    lv_obj_add_event_cb(b, media_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)usage);
    return b;
}

static void on_create(t5_app_t *app)
{
    s.app = app;
    s.q = xQueueCreate(64, sizeof(report_t));
    lv_obj_t *scr = t5_app_screen(app);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(scr, 0, 0);
    md_appbar_create(scr, "Keyboard & Trackpad", MD_ICON_ARROW_BACK, back, NULL);

    lv_obj_t *row = md_row_create(scr, MD_ICON_BLUETOOTH, "Use Bluetooth", "Off: USB-C (the console and sound are off meanwhile)");
    s.bt = md_row_switch(row, false);
    lv_obj_add_event_cb(s.bt, bt_changed, LV_EVENT_VALUE_CHANGED, NULL);

    s.status = md_label_create(scr, "Starting...", MD_TYPE_BODY2);
    lv_obj_set_width(s.status, LV_PCT(100));
    lv_obj_set_style_pad_hor(s.status, T5_DP(16), 0);
    lv_label_set_long_mode(s.status, LV_LABEL_LONG_MODE_WRAP);

    /* The trackpad: a plain surface the timer reads fingers on. */
    s.pad = lv_obj_create(scr);
    lv_obj_set_width(s.pad, LV_PCT(100));
    lv_obj_set_flex_grow(s.pad, 1);
    lv_obj_set_style_bg_color(s.pad, md_color(md_scheme()->c.surface), 0);
    lv_obj_set_style_border_width(s.pad, 0, 0);
    lv_obj_set_style_radius(s.pad, T5_DP(16), 0);
    lv_obj_set_style_margin_all(s.pad, T5_DP(12), 0);
    lv_obj_set_scrollable(s.pad, false);
    lv_obj_t *hint = md_label_create(s.pad, "Trackpad: one finger moves, tap clicks, two fingers scroll", MD_TYPE_CAPTION);
    lv_obj_center(hint);

    lv_obj_t *media = lv_obj_create(scr);
    lv_obj_remove_style_all(media);
    lv_obj_set_size(media, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(media, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(media, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    media_button(media, MD_ICON_SKIP_PREVIOUS, T5_HID_CC_PREV);
    media_button(media, MD_ICON_PLAY_ARROW, T5_HID_CC_PLAY_PAUSE);
    media_button(media, MD_ICON_SKIP_NEXT, T5_HID_CC_NEXT);
    media_button(media, MD_ICON_VOLUME_DOWN, T5_HID_CC_VOL_DOWN);
    media_button(media, MD_ICON_VOLUME_UP, T5_HID_CC_VOL_UP);

    lv_obj_t *trow = lv_obj_create(scr);
    lv_obj_remove_style_all(trow);
    lv_obj_set_size(trow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(trow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(trow, T5_DP(12), 0);
    lv_obj_set_style_pad_gap(trow, T5_DP(8), 0);
    s.text = lv_textarea_create(trow);
    lv_textarea_set_one_line(s.text, true);
    lv_textarea_set_placeholder_text(s.text, "Text to type on the computer");
    lv_obj_set_flex_grow(s.text, 1);
    lv_obj_t *type = md_button_create(trow, "Type", MD_BUTTON_CONTAINED);
    lv_obj_add_event_cb(type, type_clicked, LV_EVENT_CLICKED, NULL);

    lv_timer_create(pad_timer, 10, NULL);
    if (!s.q || t5_task_create("hid.worker", worker, NULL, 4096, 3, T5_CORE_ANY, NULL) != T5_OK) {
        lv_label_set_text(s.status, "Couldn't start: not enough memory.");
    }
}

T5_APP(.on_create = on_create);
