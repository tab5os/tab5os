/* SPDX-License-Identifier: Apache-2.0
 * USB-A host (plan 7.7). Tasks:
 *   usb.lib     usb_host_lib_handle_events (the library's own loop)
 *   usb.t5      our client: the device list (every device, any class) and
 *               the USB MIDI class, which has no ESP-IDF driver
 *   usb.work    class events that need blocking calls (MSC mount, CDC open)
 * The HID, MSC, CDC-ACM and UVC drivers run their own background tasks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "t5_core/event.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_core/res.h"
#include "t5_power/autosave.h"
#include "t5_power/power.h"
#include "t5_power/rail.h"
#include "t5_storage/paths.h"
#include "t5_storage/t5_storage.h"
#include "t5_usb/usb.h"
#include "usb/cdc_acm_host.h"
#include "usb/hid_host.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"
#include "usb_priv.h"

static const char *TAG = "t5.usb";

#define SETTINGS_FILE "usb.bin"

typedef enum { W_MSC_CONNECTED, W_MSC_GONE, W_HID_CONNECTED, W_CDC_NEW } work_kind_t;
typedef struct {
    work_kind_t kind;
    uint8_t addr;
    void *handle;
} work_t;

static t5_mutex_t s_mx;
static bool s_inited, s_installed, s_enabled = true, s_saved_enabled = true;
static t5_rail_hold_t *s_rail;
static usb_host_client_handle_t s_client;
static QueueHandle_t s_work;
static t5_usb_dev_t s_devs[T5_USB_MAX_DEVICES];
static int s_ndevs;
static t5_usb_stats_t s_st;

const char *t5_usb_class_name(t5_usb_class_t c)
{
    static const char *n[T5_USB_CLASS_COUNT] = { "other", "hub", "keyboard", "mouse", "storage",
                                                 "serial", "midi", "camera", "audio" };
    return c < T5_USB_CLASS_COUNT ? n[c] : "?";
}

static t5_usb_dev_t *dev_find(uint8_t addr)
{
    for (int i = 0; i < s_ndevs; i++) {
        if (s_devs[i].addr == addr) return &s_devs[i];
    }
    return NULL;
}

void usb_dev_set_class(uint8_t addr, t5_usb_class_t cls, bool driver)
{
    t5_mutex_lock(s_mx);
    t5_usb_dev_t *d = dev_find(addr);
    if (d) {
        d->cls = (uint8_t)cls;
        d->driver = driver;
    }
    t5_mutex_unlock(s_mx);
}

int t5_usb_devices(t5_usb_dev_t *out, int max)
{
    t5_mutex_lock(s_mx);
    int n = s_ndevs < max ? s_ndevs : max;
    memcpy(out, s_devs, (size_t)n * sizeof *out);
    t5_mutex_unlock(s_mx);
    return n;
}

int t5_usb_device_count(void)
{
    int n = 0;
    t5_mutex_lock(s_mx);
    for (int i = 0; i < s_ndevs; i++) n += s_devs[i].cls != T5_USB_HUB;
    t5_mutex_unlock(s_mx);
    return n;
}

static t5_usb_class_t guess_class(const usb_config_desc_t *cfg, const usb_device_desc_t *dd)
{
    if (dd->bDeviceClass == USB_CLASS_HUB) return T5_USB_HUB;
    int off = 0;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
    t5_usb_class_t best = T5_USB_OTHER;
    while ((d = usb_parse_next_descriptor_of_type(d, cfg->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE, &off))) {
        const usb_intf_desc_t *i = (const usb_intf_desc_t *)d;
        switch (i->bInterfaceClass) {
        case USB_CLASS_HID:
            if (i->bInterfaceProtocol == 1) return T5_USB_KEYBOARD;
            if (i->bInterfaceProtocol == 2) best = T5_USB_MOUSE;
            break;
        case USB_CLASS_MASS_STORAGE: return T5_USB_STORAGE;
        case USB_CLASS_VIDEO: return T5_USB_CAMERA;
        case USB_CLASS_AUDIO:
            if (i->bInterfaceSubClass == USB_SUBCLASS_MIDISTREAMING) return T5_USB_MIDI;
            if (best == T5_USB_OTHER) best = T5_USB_AUDIO;
            break;
        case USB_CLASS_COMM:
        case USB_CLASS_CDC_DATA:
            if (best == T5_USB_OTHER) best = T5_USB_SERIAL;
            break;
        default: break;
        }
    }
    return best;
}

static void dev_removed(uint8_t addr)
{
    t5_ev_usb_t ev = { .addr = addr };
    bool found = false;
    t5_mutex_lock(s_mx);
    for (int i = 0; i < s_ndevs; i++) {
        if (s_devs[i].addr == addr) {
            ev.cls = s_devs[i].cls;
            ev.vid = s_devs[i].vid;
            ev.pid = s_devs[i].pid;
            memmove(&s_devs[i], &s_devs[i + 1], (size_t)(s_ndevs - i - 1) * sizeof s_devs[0]);
            s_ndevs--;
            found = true;
            break;
        }
    }
    t5_mutex_unlock(s_mx);
    if (!found) return;
    s_st.detached++;
    T5_LOGI(TAG, "detached addr %u", addr);
    t5_event_post(T5_EV_USB_DETACHED, &ev, sizeof ev);
}

static void client_event(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        uint8_t addr = msg->new_dev.address;
        usb_device_handle_t h;
        if (usb_host_device_open(s_client, addr, &h) != ESP_OK) return;
        const usb_device_desc_t *dd;
        const usb_config_desc_t *cd;
        usb_device_info_t info;
        t5_usb_dev_t d = { .addr = addr };
        if (usb_host_get_device_descriptor(h, &dd) == ESP_OK && usb_host_get_active_config_descriptor(h, &cd) == ESP_OK &&
            usb_host_device_info(h, &info) == ESP_OK) {
            d.vid = dd->idVendor;
            d.pid = dd->idProduct;
            d.cls = (uint8_t)guess_class(cd, dd);
            d.speed = info.speed == USB_SPEED_HIGH ? 2 : info.speed == USB_SPEED_FULL ? 1 : 0;
            if (info.str_desc_product) {
                const usb_str_desc_t *s = info.str_desc_product;
                int n = (s->bLength - 2) / 2;
                int k = 0;
                for (int i = 0; i < n && k < (int)sizeof d.product - 1; i++) {
                    uint16_t ch = s->wData[i];
                    d.product[k++] = ch < 0x80 ? (char)ch : '?';
                }
                d.product[k] = 0;
            }
            usb_midi_check(h, addr, cd);    /* claims the interface if it is MIDI */
        }
        usb_midi_release_if_unused(h, addr);
        t5_mutex_lock(s_mx);
        if (s_ndevs < T5_USB_MAX_DEVICES && !dev_find(addr)) s_devs[s_ndevs++] = d;
        t5_mutex_unlock(s_mx);
        s_st.attached++;
        T5_LOGI(TAG, "attached %04x:%04x %s (%s, addr %u)", d.vid, d.pid, d.product, t5_usb_class_name(d.cls), addr);
        t5_ev_usb_t ev = { .addr = addr, .cls = d.cls, .vid = d.vid, .pid = d.pid };
        t5_event_post(T5_EV_USB_ATTACHED, &ev, sizeof ev);
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        /* Only devices this client still holds open (MIDI) report DEV_GONE. */
        usb_device_info_t info;
        uint8_t addr = 0;
        if (usb_host_device_info(msg->dev_gone.dev_hdl, &info) == ESP_OK) addr = info.dev_addr;
        usb_midi_gone(msg->dev_gone.dev_hdl);
        usbraw_gone(msg->dev_gone.dev_hdl);
        dev_removed(addr);
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_REMOVED) {
        /* Everything else: the client closed it after reading descriptors. */
        dev_removed(msg->dev_removed.address);
    }
}

usb_host_client_handle_t usb_client(void)
{
    return s_client;
}

static void client_task(void *arg)
{
    (void)arg;
    usb_host_client_config_t cc = {
        .is_synchronous = false, .max_num_event_msg = 8,
        .flags = { .notify_dev_removed = 1 },    /* we close most devices, so DEV_GONE never comes */
        .async = { .client_event_callback = client_event, .callback_arg = NULL },
    };
    if (usb_host_client_register(&cc, &s_client) != ESP_OK) {
        T5_LOGE(TAG, "client register failed");
        vTaskDelete(NULL);
    }
    for (;;) usb_host_client_handle_events(s_client, portMAX_DELAY);
}

static void lib_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

/* The HID interfaces the OS has open (boot keyboards and mice), so an app
 * can take one over; and what apps took over, to say where it came from. */
#define MAX_HID_IFS 8
static struct {
    hid_host_device_handle_t h;
    uint8_t addr, iface;
} s_hidifs[MAX_HID_IFS];
static struct {
    uint8_t addr, iface, owner;
} s_taken[8];
static msc_host_device_handle_t s_msc;
static uint8_t s_msc_addr;

static void hidif_set(hid_host_device_handle_t h, uint8_t addr, uint8_t iface, bool on)
{
    t5_mutex_lock(s_mx);
    for (int i = 0; i < MAX_HID_IFS; i++) {
        if (on && !s_hidifs[i].h) {
            s_hidifs[i].h = h;
            s_hidifs[i].addr = addr;
            s_hidifs[i].iface = iface;
            break;
        }
        if (!on && s_hidifs[i].h == h) s_hidifs[i].h = NULL;
    }
    t5_mutex_unlock(s_mx);
}

static hid_host_device_handle_t hidif_find(uint8_t addr, uint8_t iface)
{
    hid_host_device_handle_t h = NULL;
    t5_mutex_lock(s_mx);
    for (int i = 0; i < MAX_HID_IFS && !h; i++) {
        if (s_hidifs[i].h && s_hidifs[i].addr == addr && s_hidifs[i].iface == iface) h = s_hidifs[i].h;
    }
    t5_mutex_unlock(s_mx);
    return h;
}

usb_owner_t usb_iface_owner(uint8_t addr, uint8_t iface)
{
    if (hidif_find(addr, iface)) return USB_OWNER_HID;
    if (usb_midi_owns(addr, iface)) return USB_OWNER_MIDI;
    t5_mutex_lock(s_mx);
    t5_usb_dev_t *d = dev_find(addr);
    bool busy = (s_msc && s_msc_addr == addr) || (d && d->driver && (d->cls == T5_USB_SERIAL || d->cls == T5_USB_CAMERA));
    t5_mutex_unlock(s_mx);
    return busy ? USB_OWNER_BUSY : USB_OWNER_NONE;
}

usb_owner_t usb_iface_owner_was(uint8_t addr, uint8_t iface)
{
    usb_owner_t o = USB_OWNER_NONE;
    t5_mutex_lock(s_mx);
    for (int i = 0; i < 8; i++) {
        if (s_taken[i].owner && s_taken[i].addr == addr && s_taken[i].iface == iface) {
            o = (usb_owner_t)s_taken[i].owner;
            s_taken[i].owner = 0;
        }
    }
    t5_mutex_unlock(s_mx);
    return o;
}

t5_err_t usb_iface_detach(uint8_t addr, uint8_t iface, usb_owner_t owner)
{
    if (owner == USB_OWNER_HID) {
        hid_host_device_handle_t h = hidif_find(addr, iface);
        if (!h) return T5_ERR_STATE;
        /* The first close reports DISCONNECTED, and hid_iface_event closes
         * again, which takes the interface out of the HID driver. */
        if (hid_host_device_close(h) != ESP_OK) return T5_ERR_BUSY;
    } else if (owner == USB_OWNER_MIDI) {
        usb_midi_detach();
    } else {
        return T5_ERR_BUSY;
    }
    t5_mutex_lock(s_mx);
    for (int i = 0; i < 8; i++) {
        if (!s_taken[i].owner) {
            s_taken[i].addr = addr;
            s_taken[i].iface = iface;
            s_taken[i].owner = (uint8_t)owner;
            break;
        }
    }
    t5_mutex_unlock(s_mx);
    return T5_OK;
}

void usb_iface_reattach(usb_device_handle_t dev, uint8_t addr, usb_owner_t owner)
{
    (void)dev;
    if (owner == USB_OWNER_MIDI) {
        usb_device_handle_t h;
        const usb_config_desc_t *cd;
        if (usb_host_device_open(s_client, addr, &h) != ESP_OK) return;
        if (usb_host_get_active_config_descriptor(h, &cd) == ESP_OK) usb_midi_check(h, addr, cd);
        usb_midi_release_if_unused(h, addr);
    } else if (owner == USB_OWNER_HID) {
        /* The HID driver dropped the interface when it let go; it finds it
         * again when the device is plugged in again. */
        T5_LOGI(TAG, "addr %u: an app gave back a keyboard or mouse; unplug and replug it to use it again", addr);
    }
}

static void hid_iface_event(hid_host_device_handle_t h, const hid_host_interface_event_t ev, void *arg)
{
    (void)arg;
    hid_host_dev_params_t p;
    if (hid_host_device_get_params(h, &p) != ESP_OK) return;
    if (ev == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
        uint8_t data[64];
        size_t len = 0;
        if (hid_host_device_get_raw_input_report_data(h, data, sizeof data, &len) != ESP_OK) return;
        if (p.proto == HID_PROTOCOL_KEYBOARD) {
            usb_hid_keyboard_report(data, len);
            s_st.keys++;
        } else if (p.proto == HID_PROTOCOL_MOUSE) {
            usb_hid_mouse_report(data, len);
            s_st.mouse_reports++;
        }
    } else if (ev == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
        hidif_set(h, 0, 0, false);
        if (p.proto == HID_PROTOCOL_KEYBOARD) usb_hid_attached(T5_HID_KEYBOARD, false);
        if (p.proto == HID_PROTOCOL_MOUSE) usb_hid_attached(T5_HID_MOUSE, false);
        hid_host_device_close(h);
    } else if (ev == HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR) {
        s_st.errors++;
    }
}

static void hid_driver_event(hid_host_device_handle_t h, const hid_host_driver_event_t ev, void *arg)
{
    (void)arg;
    if (ev != HID_HOST_DRIVER_EVENT_CONNECTED) return;
    work_t w = { .kind = W_HID_CONNECTED, .handle = h };
    xQueueSend(s_work, &w, 0);
}

static void hid_open(hid_host_device_handle_t h)
{
    hid_host_dev_params_t p;
    if (hid_host_device_get_params(h, &p) != ESP_OK) return;
    /* Only boot keyboards and mice: other HID devices (gamepads, macro
     * pads, vendor ones) stay free for apps to claim (t5_usb_claim). */
    if (p.proto != HID_PROTOCOL_KEYBOARD && p.proto != HID_PROTOCOL_MOUSE) return;
    const hid_host_device_config_t dc = { .callback = hid_iface_event, .callback_arg = NULL };
    if (hid_host_device_open(h, &dc) != ESP_OK) return;
    if (p.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
        hid_class_request_set_protocol(h, HID_REPORT_PROTOCOL_BOOT);
        if (p.proto == HID_PROTOCOL_KEYBOARD) hid_class_request_set_idle(h, 0, 0);
    }
    if (hid_host_device_start(h) != ESP_OK) return;
    hidif_set(h, p.addr, p.iface_num, true);
    if (p.proto == HID_PROTOCOL_KEYBOARD) {
        usb_dev_set_class(p.addr, T5_USB_KEYBOARD, true);
        usb_hid_attached(T5_HID_KEYBOARD, true);
    } else if (p.proto == HID_PROTOCOL_MOUSE) {
        usb_dev_set_class(p.addr, T5_USB_MOUSE, true);
        usb_hid_attached(T5_HID_MOUSE, true);
    }
}

static msc_host_vfs_handle_t s_msc_vfs;

static void msc_event(const msc_host_event_t *e, void *arg)
{
    (void)arg;
    work_t w = { 0 };
    if (e->event == MSC_DEVICE_CONNECTED) {
        w.kind = W_MSC_CONNECTED;
        w.addr = e->device.address;
    } else if (e->event == MSC_DEVICE_DISCONNECTED) {
        w.kind = W_MSC_GONE;
        w.handle = e->device.handle;
    } else {
        return;
    }
    xQueueSend(s_work, &w, 0);
}

static void msc_unmount(void)
{
    if (s_msc_vfs) {
        msc_host_vfs_unregister(s_msc_vfs);
        s_msc_vfs = NULL;
        t5_ev_usb_t ev = { .mounted = 0 };
        t5_event_post(T5_EV_USB_STORAGE, &ev, sizeof ev);
    }
    if (s_msc) {
        msc_host_uninstall_device(s_msc);
        s_msc = NULL;
    }
}

static void msc_mount(uint8_t addr)
{
    if (s_msc) return;                              /* one drive at /usb */
    if (msc_host_install_device(addr, &s_msc) != ESP_OK) {
        s_msc = NULL;
        return;
    }
    const esp_vfs_fat_mount_config_t mc = { .format_if_mount_failed = false, .max_files = 8,
                                            .allocation_unit_size = 0 };
    if (msc_host_vfs_register(s_msc, T5_USB_PATH, &mc, &s_msc_vfs) != ESP_OK) {
        T5_LOGW(TAG, "USB drive at %u: no FAT filesystem", addr);
        msc_host_uninstall_device(s_msc);
        s_msc = NULL;
        return;
    }
    s_msc_addr = addr;
    usb_dev_set_class(addr, T5_USB_STORAGE, true);
    T5_LOGI(TAG, "USB drive mounted at %s", T5_USB_PATH);
    t5_ev_usb_t ev = { .addr = addr, .cls = T5_USB_STORAGE, .mounted = 1 };
    t5_event_post(T5_EV_USB_STORAGE, &ev, sizeof ev);
}

bool t5_usb_storage_mounted(void)
{
    return s_msc_vfs != NULL;
}

t5_err_t t5_usb_storage_format(void)
{
    if (!s_msc || !s_msc_vfs) return T5_ERR_STATE;
    const esp_vfs_fat_mount_config_t mc = { .format_if_mount_failed = false, .max_files = 8,
                                            .allocation_unit_size = 32 * 1024 };
    esp_err_t e = msc_host_vfs_format(s_msc, &mc, s_msc_vfs);
    T5_LOGW(TAG, "USB drive format: %s", esp_err_to_name(e));
    return e == ESP_OK ? T5_OK : T5_ERR_IO;
}

t5_err_t t5_usb_storage_eject(void)
{
    if (!s_msc_vfs) return T5_ERR_STATE;
    work_t w = { .kind = W_MSC_GONE, .handle = s_msc };
    return xQueueSend(s_work, &w, pdMS_TO_TICKS(100)) == pdTRUE ? T5_OK : T5_ERR_BUSY;
}

struct t5_usb_serial {
    uint32_t magic;
    cdc_acm_dev_hdl_t h;
    StreamBufferHandle_t rx;
    volatile bool gone;
};
#define SERIAL_MAGIC 0x55534552u

static volatile bool s_cdc_present;
static t5_usb_serial_t *s_serial;

static void cdc_new_dev(usb_device_handle_t dev)
{
    usb_device_info_t info;
    if (usb_host_device_info(dev, &info) == ESP_OK) {
        s_cdc_present = true;
        work_t w = { .kind = W_CDC_NEW, .addr = info.dev_addr };
        xQueueSend(s_work, &w, 0);
    }
}

static bool cdc_rx(const uint8_t *data, size_t len, void *arg)
{
    t5_usb_serial_t *s = arg;
    if (s && s->rx) xStreamBufferSend(s->rx, data, len, 0);
    return true;
}

static void cdc_event(const cdc_acm_host_dev_event_data_t *e, void *arg)
{
    t5_usb_serial_t *s = arg;
    if (e->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) {
        s_cdc_present = false;
        if (s) s->gone = true;
    }
}

bool t5_usb_serial_present(void)
{
    return s_cdc_present;
}

static void serial_release(void *h)
{
    t5_usb_serial_close(h);
}

t5_err_t t5_usb_serial_open(uint32_t baud, t5_usb_serial_t **out)
{
    if (!s_installed) return T5_ERR_STATE;
    if (s_serial) return T5_ERR_BUSY;
    t5_usb_serial_t *s = calloc(1, sizeof *s);
    if (!s) return T5_ERR_NO_MEM;
    s->magic = SERIAL_MAGIC;
    s->rx = xStreamBufferCreateWithCaps(8192, 1, MALLOC_CAP_SPIRAM);
    const cdc_acm_host_open_config_t oc = {
        .vid = CDC_HOST_ANY_VID, .pid = CDC_HOST_ANY_PID, .interface_idx = 0, .dev_addr = CDC_HOST_ANY_DEV_ADDR,
        .connection_timeout_ms = 1000, .out_buffer_size = 512, .in_buffer_size = 512,
        .event_cb = cdc_event, .data_cb = cdc_rx, .user_arg = s,
    };
    if (!s->rx || cdc_acm_host_open_v2(&oc, &s->h) != ESP_OK) {
        if (s->rx) vStreamBufferDeleteWithCaps(s->rx);
        free(s);
        return T5_ERR_NOT_FOUND;
    }
    cdc_acm_line_coding_t lc = { .dwDTERate = baud ? baud : 115200, .bCharFormat = 0, .bParityType = 0, .bDataBits = 8 };
    cdc_acm_host_line_coding_set(s->h, &lc);
    cdc_acm_host_set_control_line_state(s->h, true, false);
    s_serial = s;
    t5_res_add(T5_RES_OTHER, s, serial_release);
    *out = s;
    return T5_OK;
}

int t5_usb_serial_write(t5_usb_serial_t *s, const void *data, size_t n, uint32_t timeout_ms)
{
    if (!s || s->magic != SERIAL_MAGIC || s->gone) return T5_ERR_STATE;
    return cdc_acm_host_data_tx_blocking(s->h, data, n, timeout_ms) == ESP_OK ? (int)n : T5_ERR_IO;
}

int t5_usb_serial_read(t5_usb_serial_t *s, void *data, size_t n, uint32_t timeout_ms)
{
    if (!s || s->magic != SERIAL_MAGIC) return T5_ERR_STATE;
    size_t got = xStreamBufferReceive(s->rx, data, n, pdMS_TO_TICKS(timeout_ms));
    if (!got && s->gone) return T5_ERR_STATE;
    return (int)got;
}

t5_err_t t5_usb_serial_close(t5_usb_serial_t *s)
{
    if (!s || s->magic != SERIAL_MAGIC) return T5_ERR_INVALID_ARG;
    t5_res_remove(s);
    cdc_acm_host_close(s->h);
    s->magic = 0;
    vStreamBufferDeleteWithCaps(s->rx);
    if (s_serial == s) s_serial = NULL;
    free(s);
    return T5_OK;
}

struct t5_usb_uvc {
    uint32_t magic;
    uvc_host_stream_hdl_t h;
    t5_usb_uvc_frame_cb_t cb;
    void *arg;
};
#define UVC_MAGIC 0x55564331u
static volatile bool s_uvc_present;
static t5_usb_uvc_t *s_uvc;

static void uvc_driver_event(const uvc_host_driver_event_data_t *e, void *ctx)
{
    (void)ctx;
    if (e->type == UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) {
        s_uvc_present = true;
        usb_dev_set_class(e->device_connected.dev_addr, T5_USB_CAMERA, true);
    }
}

static void uvc_stream_event(const uvc_host_stream_event_data_t *e, void *ctx)
{
    t5_usb_uvc_t *u = ctx;
    if (e->type == UVC_HOST_DEVICE_DISCONNECTED) {
        s_uvc_present = false;
        if (u && u->h) {
            uvc_host_stream_close(e->device_disconnected.stream_hdl);
            u->h = NULL;
        }
    } else if (e->type == UVC_HOST_TRANSFER_ERROR || e->type == UVC_HOST_FRAME_BUFFER_OVERFLOW) {
        s_st.uvc_errors++;
    }
}

static bool uvc_frame(const uvc_host_frame_t *f, void *ctx)
{
    t5_usb_uvc_t *u = ctx;
    s_st.uvc_frames++;
    if (u && u->cb && f->data_len) u->cb(f->data, f->data_len, u->arg);
    return true;                                     /* return the buffer to the driver */
}

bool t5_usb_uvc_present(void)
{
    return s_uvc_present;
}

static void uvc_release(void *h)
{
    t5_usb_uvc_close(h);
}

t5_err_t t5_usb_uvc_open(uint16_t w, uint16_t h, uint8_t fps, t5_usb_uvc_frame_cb_t cb, void *arg,
                         t5_usb_uvc_t **out)
{
    if (!s_installed || !cb || !out) return T5_ERR_INVALID_ARG;
    int pe = t5_core_perm_check(T5_CORE_PERM_CAMERA);
    if (pe != T5_OK) return (t5_err_t)pe;
    if (s_uvc) return T5_ERR_BUSY;
    t5_usb_uvc_t *u = calloc(1, sizeof *u);
    if (!u) return T5_ERR_NO_MEM;
    u->magic = UVC_MAGIC;
    u->cb = cb;
    u->arg = arg;
    uvc_host_stream_config_t sc = {
        .event_cb = uvc_stream_event, .frame_cb = uvc_frame, .user_ctx = u,
        .usb = { .dev_addr = 0, .vid = 0, .pid = 0, .uvc_stream_index = 0 },
        .vs_format = { .h_res = w ? w : 1280, .v_res = h ? h : 720, .fps = fps ? fps : 30, .format = UVC_VS_FORMAT_MJPEG },
        .advanced = {
            .number_of_frame_buffers = 3, .frame_size = 512 * 1024, .frame_heap_caps = MALLOC_CAP_SPIRAM,
            .number_of_urbs = 3, .urb_size = 10 * 1024,
        },
    };
    esp_err_t e = uvc_host_stream_open(&sc, 2000, &u->h);
    if (e == ESP_OK) e = uvc_host_stream_start(u->h);
    if (e != ESP_OK) {
        if (u->h) uvc_host_stream_close(u->h);
        free(u);
        T5_LOGW(TAG, "UVC open %ux%u@%u: %s", w, h, fps, esp_err_to_name(e));
        return e == ESP_ERR_NOT_FOUND ? T5_ERR_NOT_FOUND : T5_ERR_NOT_SUPPORTED;
    }
    s_uvc = u;
    t5_res_add(T5_RES_OTHER, u, uvc_release);
    t5_ev_media_t ev = { .on = true };
    snprintf(ev.owner, sizeof ev.owner, "%s", t5_caller_id());
    t5_event_post(T5_EV_CAMERA_STATE, &ev, sizeof ev);
    *out = u;
    return T5_OK;
}

t5_err_t t5_usb_uvc_close(t5_usb_uvc_t *u)
{
    if (!u || u->magic != UVC_MAGIC) return T5_ERR_INVALID_ARG;
    t5_res_remove(u);
    if (u->h) {
        uvc_host_stream_stop(u->h);
        uvc_host_stream_close(u->h);
    }
    u->magic = 0;
    if (s_uvc == u) s_uvc = NULL;
    free(u);
    t5_ev_media_t ev = { .on = false };
    t5_event_post(T5_EV_CAMERA_STATE, &ev, sizeof ev);
    return T5_OK;
}

static void work_task(void *arg)
{
    (void)arg;
    work_t w;
    for (;;) {
        if (xQueueReceive(s_work, &w, portMAX_DELAY) != pdTRUE) continue;
        switch (w.kind) {
        case W_MSC_CONNECTED: msc_mount(w.addr); break;
        case W_MSC_GONE:
            if (!w.handle || w.handle == s_msc) msc_unmount();
            break;
        case W_HID_CONNECTED: hid_open(w.handle); break;
        case W_CDC_NEW: usb_dev_set_class(w.addr, T5_USB_SERIAL, true); break;
        }
    }
}

static t5_err_t install(void)
{
    if (s_installed) return T5_OK;
    const usb_host_config_t hc = { .skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LOWMED };
    if (usb_host_install(&hc) != ESP_OK) return T5_ERR_IO;
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;   /* keep internal RAM for DMA */
    xTaskCreatePinnedToCoreWithCaps(lib_task, "usb.lib", 4096, NULL, 10, NULL, 0, caps);
    xTaskCreatePinnedToCoreWithCaps(client_task, "usb.t5", 4096, NULL, 9, NULL, 0, caps);
    xTaskCreatePinnedToCoreWithCaps(work_task, "usb.work", 4096, NULL, 8, NULL, 0, caps);
    /* The class drivers' tasks mostly wait; they would hold 18 KB of internal
     * RAM. They live as long as the OS. */
    t5_task_stacks_in_psram(true);
    const hid_host_driver_config_t hid = {
        .create_background_task = true, .task_priority = 9, .stack_size = 4096, .core_id = 0,
        .callback = hid_driver_event, .callback_arg = NULL,
    };
    if (hid_host_install(&hid) != ESP_OK) T5_LOGW(TAG, "HID driver failed");
    const msc_host_driver_config_t msc = {
        .create_backround_task = true, .task_priority = 8, .stack_size = 4096, .core_id = 0,
        .callback = msc_event, .callback_arg = NULL,
    };
    if (msc_host_install(&msc) != ESP_OK) T5_LOGW(TAG, "MSC driver failed");
    const cdc_acm_host_driver_config_t cdc = {
        .driver_task_stack_size = 4096, .driver_task_priority = 9, .xCoreID = 0, .new_dev_cb = cdc_new_dev,
    };
    if (cdc_acm_host_install(&cdc) != ESP_OK) T5_LOGW(TAG, "CDC-ACM driver failed");
    const uvc_host_driver_config_t uvc = {
        .driver_task_stack_size = 6144, .driver_task_priority = 9, .xCoreID = 0, .create_background_task = true,
        .event_cb = uvc_driver_event, .user_ctx = NULL,
    };
    if (uvc_host_install(&uvc) != ESP_OK) T5_LOGW(TAG, "UVC driver failed");
    t5_task_stacks_in_psram(false);
    s_installed = true;
    s_st.installed = true;
    return T5_OK;
}

static t5_err_t power_on(bool on)
{
    if (on && !s_rail) return t5_rail_acquire_system(T5_RAIL_USB5V, "usb", 0, &s_rail);
    if (!on && s_rail) {
        T5_LOG_ON_ERR(TAG, t5_rail_release(s_rail));
        s_rail = NULL;
    }
    return T5_OK;
}

t5_err_t t5_usb_host_enable(bool on)
{
    if (!s_inited) return T5_ERR_STATE;
    t5_mutex_lock(s_mx);
    s_enabled = on;
    t5_err_t e = T5_OK;
    if (on) {
        /* 5 V first, then the root port: a device that powers up while the
         * port is mid-reset fails every reset ("Root port reset failed"). */
        e = power_on(true);
        if (e == T5_OK) {
            t5_delay_ms(200);
            if (!s_installed) e = install();
            else usb_host_lib_set_root_port_power(true);
        }
    } else {
        msc_unmount();
        if (s_installed) usb_host_lib_set_root_port_power(false);   /* devices detach; drivers clean up */
        power_on(false);
    }
    s_st.host_on = on && e == T5_OK;
    t5_mutex_unlock(s_mx);
    return e;
}

bool t5_usb_host_enabled(void)
{
    return s_enabled;
}

void t5_usb_stats(t5_usb_stats_t *out)
{
    *out = s_st;
    out->devices = (uint8_t)s_ndevs;
}

static t5_err_t save(void *arg)
{
    (void)arg;
    if (s_enabled == s_saved_enabled) return T5_OK;
    uint8_t v = s_enabled;
    char path[96];
    T5_RETURN_ON_ERR(t5_sys_path(path, sizeof path, SETTINGS_FILE));
    t5_err_t e = t5_storage_blob_write(path, 1, &v, 1);
    if (e == T5_OK) s_saved_enabled = s_enabled;
    return e;
}

static t5_err_t usb_suspend(void *arg)
{
    (void)arg;
    /* The rails hook cuts USB 5V in Sleep anyway; unmount first so a drive
     * isn't yanked mid-write. */
    msc_unmount();
    return T5_OK;
}

t5_err_t t5_usb_init(void)
{
    if (s_inited) return T5_OK;
    s_mx = t5_mutex_create();
    s_work = xQueueCreate(8, sizeof(work_t));
    if (!s_mx || !s_work) return T5_ERR_NO_MEM;
    uint8_t v = 1;
    size_t len = 0;
    char path[96];
    if (t5_sys_path(path, sizeof path, SETTINGS_FILE) == T5_OK && t5_storage_blob_read(path, 1, &v, 1, &len) == T5_OK &&
        len == 1) {
        s_enabled = s_saved_enabled = v;
    }
    usb_midi_init();
    usbc_init();
    s_inited = true;
    const t5_suspend_hook_t hook = {
        .name = "usb", .prio = T5_SUSPEND_PRIO_MEDIA, .budget_ms = 200, .suspend = usb_suspend,
    };
    T5_LOG_ON_ERR(TAG, t5_power_register_hook(&hook));
    T5_LOG_ON_ERR(TAG, t5_autosave_register("usb", save, NULL));
    t5_err_t e = s_enabled ? t5_usb_host_enable(true) : T5_OK;
    T5_LOGI(TAG, "USB-A host %s", s_enabled ? (e == T5_OK ? "on" : "failed") : "off");
    return e;
}
