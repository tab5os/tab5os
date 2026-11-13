/* SPDX-License-Identifier: Apache-2.0
 * USB-C device mode for apps (t5_usb/usbdev.h): a composite TinyUSB device
 * of up to three HID interfaces and a MIDI port, described at open. usbc.c
 * owns the port (the PHY swap, audio pins, wake lock); this owns the
 * descriptors, the TinyUSB class callbacks and the app's handle.
 *
 * Tasks: TinyUSB's own task runs the callbacks below, which only queue
 * events. "usbdev.rel" sends the all-released HID reports when the owning
 * app is suspended, so the event task never waits on USB.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "t5_core/event.h"
#include "t5_core/io_hold.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_core/res.h"
#include "t5_usb/usbdev.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#include "usb_proto.h"
#include "usbdev_priv.h"

static const char *TAG = "t5.usbdev";

#define MAGIC 0x55534244u      /* "USBD" */
#define QLEN 32
#define MAX_IDS 8              /* report IDs with input reports, per interface */
#define STR_LEN 32
/* The P4's full-speed controller: endpoint 0 plus 4 IN endpoints in use at
 * once is what its FIFOs are sized for (3 HID + MIDI). */
#define MAX_IN_EPS 4

_Static_assert(CFG_TUD_HID >= T5_USBDEV_MAX_HID, "CONFIG_TINYUSB_HID_COUNT covers T5_USBDEV_MAX_HID");
_Static_assert(CFG_TUD_MIDI >= 1, "CONFIG_TINYUSB_MIDI_COUNT is at least 1");

typedef struct {
    t5_usbdev_ev_t ev;
    uint8_t len;
    uint8_t data[64];
} qev_t;

typedef struct {
    uint8_t *desc;
    uint16_t len;
    uint8_t boot;
    uint8_t n_ids;
    uint8_t id[MAX_IDS];       /* 0 when the descriptor has no IDs */
    uint8_t bytes[MAX_IDS];    /* input report size, without the ID */
} hid_t;

struct t5_usbdev {
    uint32_t magic;
    char app_id[32];
    char who[32];              /* the app's name, for the shell */
    bool app_owned;
    QueueHandle_t q;
    TaskHandle_t rel_task;
    SemaphoreHandle_t rel_done;
    volatile bool revoked;     /* the person stopped it, or the app closed it */
    volatile bool mounted, suspended;
    uint8_t hid_count;
    bool midi;
    hid_t hid[T5_USBDEV_MAX_HID];
    tusb_desc_device_t dev;
    uint8_t *cfg;
    char str[4][STR_LEN];
    const char *str_ptrs[4];
};

static t5_mutex_t s_mx;
static t5_usbdev_t *s_dev;     /* the open device; TinyUSB's callbacks read it */

static t5_mutex_t mx(void)
{
    return t5_mutex_once(&s_mx);
}

/* ---- HID descriptor reading ----------------------------------------------- */
/* The input report sizes by report ID, for the all-released reports. Items
 * are walked flatly (globals as they come), which is what every descriptor
 * t5_hid builds, and most others, need. */
static bool hid_scan(hid_t *h)
{
    int size = 0, count = 0, id = 0;
    uint16_t bits[256] = { 0 };
    bool any_id = false;
    for (int i = 0; i < h->len;) {
        uint8_t b = h->desc[i];
        if (b == 0xfe) return false;               /* long items: not supported */
        int len = b & 3;
        if (len == 3) len = 4;
        if (i + 1 + len > h->len) return false;
        uint32_t v = 0;
        for (int k = 0; k < len; k++) v |= (uint32_t)h->desc[i + 1 + k] << (8 * k);
        switch (b & 0xfc) {
        case 0x74: size = (int)v; break;
        case 0x94: count = (int)v; break;
        case 0x84:
            if (v == 0 || v > 255) return false;
            id = (int)v;
            any_id = true;
            break;
        case 0x80:
            if (bits[id] + size * count > 8 * (T5_USBDEV_HID_REPORT_MAX + 1)) return false;
            bits[id] = (uint16_t)(bits[id] + size * count);
            break;
        default: break;
        }
        i += 1 + len;
    }
    if (any_id && bits[0]) return false;            /* inputs before the first ID */
    h->n_ids = 0;
    for (int r = 0; r < 256; r++) {
        if (!bits[r]) continue;
        int bytes = (bits[r] + 7) / 8;
        if (h->n_ids == MAX_IDS || bytes > T5_USBDEV_HID_REPORT_MAX) return false;
        h->id[h->n_ids] = (uint8_t)r;
        h->bytes[h->n_ids++] = (uint8_t)bytes;
    }
    return h->n_ids > 0;
}

/* ---- descriptors ---------------------------------------------------------- */
static int config_len(const t5_usbdev_t *d)
{
    return TUD_CONFIG_DESC_LEN + d->hid_count * TUD_HID_DESC_LEN + (d->midi ? TUD_MIDI_DESC_LEN : 0);
}

/* Interfaces: HID 0..n-1 on IN endpoints 1..n, then MIDI's two interfaces
 * on endpoint n+1 (OUT and IN). */
static bool build_config(t5_usbdev_t *d)
{
    int total = config_len(d);
    d->cfg = malloc((size_t)total);
    if (!d->cfg) return false;
    uint8_t itfs = (uint8_t)(d->hid_count + (d->midi ? 2 : 0));
    const uint8_t head[] = { TUD_CONFIG_DESCRIPTOR(1, itfs, 0, total, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100) };
    int n = 0;
    memcpy(d->cfg + n, head, sizeof head);
    n += sizeof head;
    for (uint8_t i = 0; i < d->hid_count; i++) {
        const hid_t *h = &d->hid[i];
        uint8_t proto = h->boot == T5_USBDEV_BOOT_KEYBOARD ? HID_ITF_PROTOCOL_KEYBOARD
                        : h->boot == T5_USBDEV_BOOT_MOUSE  ? HID_ITF_PROTOCOL_MOUSE
                                                           : HID_ITF_PROTOCOL_NONE;
        /* The largest input report plus its ID byte, rounded to a valid
         * full-speed interrupt size. */
        uint8_t ep_size = 8;
        for (int k = 0; k < h->n_ids; k++) {
            int need = h->bytes[k] + (h->id[k] ? 1 : 0);
            while (ep_size < need) ep_size = (uint8_t)(ep_size * 2);
        }
        /* bInterval (the last byte) is set by set_intervals. */
        const uint8_t hd[] = { TUD_HID_DESCRIPTOR(i, 0, proto, h->len, (uint8_t)(0x81 + i), ep_size, 5) };
        memcpy(d->cfg + n, hd, sizeof hd);
        n += sizeof hd;
    }
    if (d->midi) {
        uint8_t ep = (uint8_t)(d->hid_count + 1);
        const uint8_t md[] = { TUD_MIDI_DESCRIPTOR(d->hid_count, 0, ep, (uint8_t)(0x80 | ep), 64) };
        memcpy(d->cfg + n, md, sizeof md);
        n += sizeof md;
    }
    return n == total;
}

static void set_intervals(t5_usbdev_t *d, const t5_usbdev_cfg_t *cfg)
{
    int n = TUD_CONFIG_DESC_LEN;
    for (uint8_t i = 0; i < d->hid_count; i++) {
        uint8_t ms = cfg->hid[i].poll_ms ? cfg->hid[i].poll_ms : 5;
        d->cfg[n + TUD_HID_DESC_LEN - 1] = ms;
        n += TUD_HID_DESC_LEN;
    }
}

/* esp_tinyusb's convention for a PID from the classes present (MSC alone is
 * 0x4002): bit 2 HID, bit 3 MIDI. */
static uint16_t default_pid(const t5_usbdev_t *d)
{
    return (uint16_t)(0x4000 | (d->hid_count ? 1u << 2 : 0) | (d->midi ? 1u << 3 : 0));
}

static void copy_str(char *dst, const char *src, const char *dflt)
{
    snprintf(dst, STR_LEN, "%s", src ? src : dflt);
}

/* ---- events --------------------------------------------------------------- */
static void push(t5_usbdev_t *d, const t5_usbdev_ev_t *ev, const void *data, size_t len)
{
    qev_t q = { .ev = *ev, .len = (uint8_t)(len > sizeof q.data ? sizeof q.data : len) };
    if (q.len) memcpy(q.data, data, q.len);
    if (d->q && xQueueSend(d->q, &q, 0) != pdTRUE) T5_LOGD(TAG, "event queue full: dropped %d", ev->type);
}

/* TinyUSB's task (esp_tinyusb's event callback). */
static void on_usb_event(tinyusb_event_t *event, void *arg)
{
    t5_usbdev_t *d = arg;
    t5_usbdev_ev_t ev = { 0 };
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        d->mounted = true;
        d->suspended = false;
        ev.type = T5_USBDEV_MOUNTED;
        break;
    case TINYUSB_EVENT_DETACHED:
        d->mounted = false;
        ev.type = T5_USBDEV_UNMOUNTED;
        break;
    case TINYUSB_EVENT_SUSPENDED:
        d->suspended = true;
        ev.type = T5_USBDEV_SUSPENDED;
        ev.remote_wakeup = event->suspended.remote_wakeup;
        break;
    case TINYUSB_EVENT_RESUMED:
        d->suspended = false;
        ev.type = T5_USBDEV_RESUMED;
        break;
    default: return;
    }
    push(d, &ev, NULL, 0);
}

/* TinyUSB's class callbacks. They run only while a device is open: storage
 * mode has no HID or MIDI interfaces. */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    t5_usbdev_t *d = s_dev;
    return d && instance < d->hid_count ? d->hid[instance].desc : NULL;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;   /* STALL: input reports go out on the interrupt endpoint */
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize)
{
    t5_usbdev_t *d = s_dev;
    if (!d || report_type == HID_REPORT_TYPE_FEATURE) return;
    t5_usbdev_ev_t ev = { .type = T5_USBDEV_HID_OUTPUT, .hid = instance, .report_id = report_id };
    push(d, &ev, buffer, bufsize);
}

void tud_hid_set_protocol_cb(uint8_t instance, uint8_t protocol)
{
    t5_usbdev_t *d = s_dev;
    if (!d) return;
    t5_usbdev_ev_t ev = { .type = T5_USBDEV_HID_PROTOCOL, .hid = instance, .boot_protocol = protocol == HID_PROTOCOL_BOOT };
    push(d, &ev, NULL, 0);
}

void tud_midi_rx_cb(uint8_t itf)
{
    t5_usbdev_t *d = s_dev;
    uint8_t pkt[4], msg[3];
    while (tud_midi_n_packet_read(itf, pkt)) {
        size_t n = usb_midi_parse(pkt, msg);
        if (!d || !n) continue;
        t5_usbdev_ev_t ev = { .type = T5_USBDEV_MIDI_IN };
        push(d, &ev, msg, n);
    }
}

/* ---- releasing keys ------------------------------------------------------- */
static bool wait_ready(t5_usbdev_t *d, int hid, uint32_t timeout_ms)
{
    int64_t until = t5_time_us() + (int64_t)timeout_ms * 1000;
    while (!tud_hid_n_ready((uint8_t)hid)) {
        if (d->revoked || !tud_mounted() || tud_suspended()) return false;
        if (timeout_ms != T5_USBDEV_FOREVER && t5_time_us() >= until) return false;
        vTaskDelay(1);
    }
    return true;
}

static void release_all(t5_usbdev_t *d)
{
    static const uint8_t zero[T5_USBDEV_HID_REPORT_MAX];
    for (int i = 0; i < d->hid_count; i++) {
        for (int k = 0; k < d->hid[i].n_ids; k++) {
            if (!wait_ready(d, i, 100)) break;
            T5_IGNORE(tud_hid_n_report((uint8_t)i, d->hid[i].id[k], zero, d->hid[i].bytes[k]) ? T5_OK : T5_ERR_IO);
        }
    }
}

static void rel_task(void *arg)
{
    t5_usbdev_t *d = arg;
    for (;;) {
        uint32_t why = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (why & 0x80000000u) break;              /* closing */
        if (!d->revoked && tud_mounted()) release_all(d);
    }
    xSemaphoreGive(d->rel_done);
    vTaskDelete(NULL);
}

static void on_app_event(t5_event_id_t id, const void *data, size_t len, void *arg)
{
    (void)id; (void)arg;
    const t5_ev_app_t *ev = data;
    t5_mutex_lock(mx());
    t5_usbdev_t *d = s_dev;
    if (d && d->app_owned && len >= sizeof *ev && !strncmp(ev->id, d->app_id, sizeof ev->id) && d->rel_task) {
        xTaskNotify(d->rel_task, 1, eSetBits);
    }
    t5_mutex_unlock(mx());
}

/* ---- the API -------------------------------------------------------------- */
static bool valid(const t5_usbdev_t *d)
{
    return d && d->magic == MAGIC;
}

static void dev_free(t5_usbdev_t *d)
{
    for (int i = 0; i < T5_USBDEV_MAX_HID; i++) free(d->hid[i].desc);
    free(d->cfg);
    if (d->q) vQueueDelete(d->q);
    if (d->rel_done) vSemaphoreDelete(d->rel_done);
    d->magic = 0;
    free(d);
}

/* Stop the release task and give the port back. Called once per device, by
 * close or by revoke (whichever comes first). */
static void stop_rel_task(t5_usbdev_t *d)
{
    if (!d->rel_task) return;
    xTaskNotify(d->rel_task, 0x80000000u, eSetBits);
    xSemaphoreTake(d->rel_done, pdMS_TO_TICKS(1000));
    d->rel_task = NULL;
}

static void stop_port(t5_usbdev_t *d)
{
    stop_rel_task(d);
    usbc_device_end();
}

static void release_handle(void *h)
{
    T5_IGNORE(t5_usbdev_close(h));
}

t5_err_t t5_usbdev_open(const t5_usbdev_cfg_t *cfg, t5_usbdev_t **out)
{
    if (!cfg || !out || cfg->size < sizeof *cfg || cfg->hid_count > T5_USBDEV_MAX_HID) return T5_ERR_INVALID_ARG;
    if (!cfg->hid_count && !cfg->midi) return T5_ERR_INVALID_ARG;
    if (cfg->hid_count + (cfg->midi ? 1 : 0) > MAX_IN_EPS) return T5_ERR_INVALID_ARG;
    *out = NULL;

    t5_usbdev_t *d = calloc(1, sizeof *d);
    if (!d) return T5_ERR_NO_MEM;
    d->magic = MAGIC;
    d->hid_count = cfg->hid_count;
    d->midi = cfg->midi;
    t5_err_t e = T5_OK;
    for (int i = 0; i < d->hid_count && e == T5_OK; i++) {
        const t5_usbdev_hid_t *h = &cfg->hid[i];
        if (!h->report_desc || !h->desc_len || h->desc_len > T5_USBDEV_HID_DESC_MAX || h->boot > T5_USBDEV_BOOT_MOUSE) {
            e = T5_ERR_INVALID_ARG;
            break;
        }
        d->hid[i].desc = malloc(h->desc_len);
        if (!d->hid[i].desc) {
            e = T5_ERR_NO_MEM;
            break;
        }
        memcpy(d->hid[i].desc, h->report_desc, h->desc_len);
        d->hid[i].len = h->desc_len;
        d->hid[i].boot = h->boot;
        if (!hid_scan(&d->hid[i])) e = T5_ERR_INVALID_ARG;
        /* Boot protocol has no report IDs. */
        if (e == T5_OK && h->boot && d->hid[i].id[0]) e = T5_ERR_INVALID_ARG;
    }
    if (e != T5_OK) {
        dev_free(d);
        return e;
    }

    /* Ask the person (blocks; on the UI task it asks and says no for now). */
    int pe = t5_core_perm_check(T5_CORE_CAP_USB_DEVICE);
    if (pe != T5_OK) {
        dev_free(d);
        return (t5_err_t)pe;
    }

    const t5_app_ctx_hdr_t *app = t5_app_ctx_get();
    if (app && app->magic == T5_APP_CTX_MAGIC) {
        d->app_owned = true;
        snprintf(d->app_id, sizeof d->app_id, "%s", app->id);
        snprintf(d->who, sizeof d->who, "%s", app->name && app->name[0] ? app->name : app->id);
    }
    copy_str(d->str[1], cfg->manufacturer, "Tab5 OS");
    copy_str(d->str[2], cfg->product, d->app_owned && app->name && app->name[0] ? app->name : "Tab5");
    if (cfg->serial) copy_str(d->str[3], cfg->serial, "");
    static const char lang[] = { 0x09, 0x04 };
    d->str_ptrs[0] = lang;
    d->str_ptrs[1] = d->str[1];
    d->str_ptrs[2] = d->str[2];
    d->str_ptrs[3] = cfg->serial ? d->str[3] : NULL;   /* NULL: esp_tinyusb uses the chip's MAC */

    d->dev = (tusb_desc_device_t){
        .bLength = sizeof(tusb_desc_device_t),
        .bDescriptorType = TUSB_DESC_DEVICE,
        .bcdUSB = 0x0200,
        /* MIDI's audio-class pair needs no IAD; HID interfaces stand alone. */
        .bDeviceClass = 0,
        .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
        .idVendor = cfg->vid ? cfg->vid : TINYUSB_ESPRESSIF_VID,
        .idProduct = cfg->vid ? cfg->pid : default_pid(d),
        .bcdDevice = 0x0100,
        .iManufacturer = 1,
        .iProduct = 2,
        .iSerialNumber = 3,
        .bNumConfigurations = 1,
    };
    d->q = xQueueCreate(QLEN, sizeof(qev_t));
    d->rel_done = xSemaphoreCreateBinary();
    if (!d->q || !d->rel_done || !build_config(d)) {
        dev_free(d);
        return T5_ERR_NO_MEM;
    }
    set_intervals(d, cfg);

    t5_mutex_lock(mx());
    if (s_dev) {
        t5_mutex_unlock(mx());
        dev_free(d);
        return T5_ERR_BUSY;
    }
    s_dev = d;
    t5_mutex_unlock(mx());

    if (d->hid_count && xTaskCreatePinnedToCore(rel_task, "usbdev.rel", 3072, d, 5, &d->rel_task, 1) != pdPASS) {
        d->rel_task = NULL;
    }
    tinyusb_config_t tc = TINYUSB_DEFAULT_CONFIG();
    tc.port = TINYUSB_PORT_FULL_SPEED_0;
    tc.descriptor.device = &d->dev;
    tc.descriptor.full_speed_config = d->cfg;
    tc.descriptor.string = d->str_ptrs;
    tc.descriptor.string_count = 4;
    tc.event_cb = on_usb_event;
    tc.event_arg = d;
    e = usbc_device_begin(&tc);
    if (e != T5_OK) {
        d->revoked = true;
        stop_rel_task(d);
        t5_mutex_lock(mx());
        s_dev = NULL;
        t5_mutex_unlock(mx());
        dev_free(d);
        return e;
    }
    T5_LOGI(TAG, "open: %d HID%s, %04x:%04x \"%s\"", d->hid_count, d->midi ? " + MIDI" : "", d->dev.idVendor,
            d->dev.idProduct, d->str[2]);
    t5_io_hold_set(T5_IO_USBC_DEVICE, d->who, usbdev_revoke);
    t5_res_add(T5_RES_OTHER, d, release_handle);
    *out = d;
    return T5_OK;
}

bool t5_usbdev_mounted(t5_usbdev_t *d)
{
    return valid(d) && !d->revoked && d->mounted && !d->suspended;
}

t5_err_t t5_usbdev_hid_send(t5_usbdev_t *d, int hid, uint8_t report_id, const void *data, size_t len,
                            uint32_t timeout_ms)
{
    if (!valid(d) || hid < 0 || hid >= d->hid_count || (len && !data) || len > T5_USBDEV_HID_REPORT_MAX) {
        return T5_ERR_INVALID_ARG;
    }
    if (d->revoked || !d->mounted || d->suspended || !t5_core_app_foreground()) return T5_ERR_STATE;
    if (!wait_ready(d, hid, timeout_ms)) return d->revoked || !tud_mounted() ? T5_ERR_STATE : T5_ERR_TIMEOUT;
    return tud_hid_n_report((uint8_t)hid, report_id, data, (uint16_t)len) ? T5_OK : T5_ERR_IO;
}

t5_err_t t5_usbdev_midi_send(t5_usbdev_t *d, const uint8_t *msg, size_t len)
{
    if (!valid(d) || !msg || !len || len > 3) return T5_ERR_INVALID_ARG;
    if (!d->midi) return T5_ERR_NOT_SUPPORTED;
    if (d->revoked || !d->mounted) return T5_ERR_STATE;
    uint8_t pkt[4];
    if (!usb_midi_pack(msg, len, pkt)) return T5_ERR_INVALID_ARG;
    return tud_midi_n_packet_write(0, pkt) ? T5_OK : T5_ERR_BUSY;
}

t5_err_t t5_usbdev_wakeup(t5_usbdev_t *d)
{
    if (!valid(d)) return T5_ERR_INVALID_ARG;
    if (d->revoked || !d->mounted) return T5_ERR_STATE;
    return tud_remote_wakeup() ? T5_OK : T5_ERR_STATE;
}

int t5_usbdev_next(t5_usbdev_t *d, t5_usbdev_ev_t *ev, void *buf, size_t cap, uint32_t timeout_ms)
{
    if (!valid(d) || !ev || (cap && !buf)) return T5_ERR_INVALID_ARG;
    qev_t q;
    TickType_t t = timeout_ms == T5_USBDEV_FOREVER ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xQueueReceive(d->q, &q, t) != pdTRUE) return d->revoked ? T5_ERR_STATE : T5_ERR_TIMEOUT;
    *ev = q.ev;
    size_t n = q.len < cap ? q.len : cap;
    if (n) memcpy(buf, q.data, n);
    return (int)n;
}

/* Ends the port for whoever has it; the handle stays until closed. */
void usbdev_revoke(void)
{
    t5_mutex_lock(mx());
    t5_usbdev_t *d = s_dev;
    if (d && !d->revoked) {
        T5_LOGI(TAG, "device mode stopped for %s", d->app_owned ? d->app_id : "the system");
        d->revoked = true;
        stop_port(d);
        t5_io_hold_set(T5_IO_USBC_DEVICE, NULL, NULL);
        d->mounted = false;
        t5_usbdev_ev_t ev = { .type = T5_USBDEV_UNMOUNTED };
        push(d, &ev, NULL, 0);
    }
    s_dev = d && d->revoked ? NULL : s_dev;
    t5_mutex_unlock(mx());
}

t5_err_t t5_usbdev_close(t5_usbdev_t *d)
{
    if (!valid(d)) return T5_ERR_INVALID_ARG;
    t5_res_remove(d);
    t5_mutex_lock(mx());
    if (!d->revoked) {
        d->revoked = true;
        stop_port(d);
        t5_io_hold_set(T5_IO_USBC_DEVICE, NULL, NULL);
    }
    if (s_dev == d) s_dev = NULL;
    t5_mutex_unlock(mx());
    dev_free(d);
    return T5_OK;
}

void usbdev_init(void)
{
    T5_LOG_ON_ERR(TAG, t5_event_subscribe(T5_EV_APP_SUSPENDED, on_app_event, NULL));
}
