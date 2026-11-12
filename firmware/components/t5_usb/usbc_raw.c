/* SPDX-License-Identifier: Apache-2.0
 * Exclusive USB-C: a device of the app's own design (the raw part of
 * t5_usb/usbdev.h).
 *
 * A TinyUSB application class driver takes every interface of the app's
 * configuration (application drivers come before the built-in classes) and
 * opens their endpoints. Class requests reach its control callback; vendor
 * requests reach tud_vendor_control_xfer_cb (no vendor class is compiled
 * in). Requests the app must answer wait, deferred, until it replies: the
 * reply runs on TinyUSB's task through usbd_defer_func, as do endpoint
 * transfers, so TinyUSB is only ever driven from its own task.
 */
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "t5_core/excl_sys.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_usb/usb.h"
#include "t5_usb/usbdev.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#include "device/usbd_pvt.h"
#include "usbdev_priv.h"

static const char *TAG = "t5.usbcraw";

#define QLEN 16
#define EP_MAX 6               /* endpoint numbers 1..5 */
#define XFER_MAX 4096

typedef struct {
    t5_usbc_raw_ev_t ev;
    uint16_t len;
    uint8_t data[T5_USBC_RAW_CTRL_MAX];
} raw_item_t;

typedef struct {
    bool open;
    uint8_t *buf;              /* DMA-capable, XFER_MAX */
    volatile bool busy;
    volatile uint32_t done_len;
    volatile int done_err;
    SemaphoreHandle_t done;
} ep_t;

static struct {
    volatile bool on;          /* the claim's provider took USB-C */
    volatile bool running;     /* TinyUSB up with the app's descriptors */
    volatile bool mounted;
    QueueHandle_t q;
    tusb_desc_device_t dev;
    uint8_t *cfg, *bos;
    char str[T5_USBC_RAW_MAX_STRINGS][64];
    const char *str_ptrs[1 + T5_USBC_RAW_MAX_STRINGS];
    int nstr;
    /* the control request waiting for the app's answer */
    volatile bool pending;
    uint8_t rhport;
    tusb_control_request_t req;
    uint8_t ctrl_buf[T5_USBC_RAW_CTRL_MAX];
    uint16_t reply_len;
    bool reply_stall;
    ep_t ep[2][EP_MAX];        /* [dir: 0 OUT, 1 IN][number] */
} s;

static ep_t *ep_of(uint8_t addr)
{
    uint8_t n = addr & 0x0f;
    return n && n < EP_MAX ? &s.ep[addr & 0x80 ? 1 : 0][n] : NULL;
}

static void push(const t5_usbc_raw_ev_t *ev, const void *data, size_t len)
{
    raw_item_t *it = malloc(sizeof *it);
    if (!it) return;
    it->ev = *ev;
    it->len = (uint16_t)(len > sizeof it->data ? sizeof it->data : len);
    if (it->len) memcpy(it->data, data, it->len);
    if (s.q) xQueueSend(s.q, it, 0);
    free(it);
}

/* ---- the class driver (TinyUSB's task) ---------------------------------------- */
static void drv_init(void) {}

static bool drv_deinit(void)
{
    return true;
}

static void drv_reset(uint8_t rhport)
{
    (void)rhport;
    for (int d = 0; d < 2; d++) {
        for (int n = 0; n < EP_MAX; n++) {
            ep_t *e = &s.ep[d][n];
            e->open = false;
            if (e->busy) {
                e->busy = false;
                e->done_err = T5_ERR_STATE;
                if (e->done) xSemaphoreGive(e->done);
            }
        }
    }
    s.pending = false;
}

/* Take this interface (and its alternate settings) and open the endpoints
 * of alternate setting 0. */
static uint16_t drv_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len)
{
    const uint8_t *p = (const uint8_t *)itf;
    uint16_t len = 0;
    uint8_t alt = 0;
    while (len < max_len) {
        uint8_t dl = p[0], type = p[1];
        if (dl < 2 || len + dl > max_len) break;
        if (len && type == TUSB_DESC_INTERFACE_ASSOCIATION) break;
        if (len && type == TUSB_DESC_INTERFACE && ((tusb_desc_interface_t const *)p)->bInterfaceNumber != itf->bInterfaceNumber) {
            break;
        }
        if (type == TUSB_DESC_INTERFACE) alt = ((tusb_desc_interface_t const *)p)->bAlternateSetting;
        if (type == TUSB_DESC_ENDPOINT && alt == 0) {
            tusb_desc_endpoint_t const *ed = (tusb_desc_endpoint_t const *)p;
            ep_t *e = ep_of(ed->bEndpointAddress);
            if (e && usbd_edpt_open(rhport, ed)) e->open = true;
        }
        len += dl;
        p += dl;
    }
    return len;
}

static bool on_request(uint8_t rhport, uint8_t stage, tusb_control_request_t const *r)
{
    bool in = r->bmRequestType_bit.direction == TUSB_DIR_IN;
    if (stage == CONTROL_STAGE_SETUP) {
        if (!in && r->wLength) {
            /* Data to the device: take it, then tell the app (acknowledged). */
            if (r->wLength > sizeof s.ctrl_buf) return false;
            return tud_control_xfer(rhport, r, s.ctrl_buf, r->wLength);
        }
        if (s.pending) return false;                  /* one at a time */
        s.pending = true;
        s.rhport = rhport;
        s.req = *r;
        t5_usbc_raw_ev_t ev = { .type = T5_USBC_RAW_SETUP, .bm_request_type = r->bmRequestType, .b_request = r->bRequest,
                                .w_value = r->wValue, .w_index = r->wIndex, .w_length = r->wLength, .needs_reply = true };
        push(&ev, NULL, 0);
        return true;                                  /* answered later (deferred) */
    }
    if (stage == CONTROL_STAGE_DATA && !in) {
        t5_usbc_raw_ev_t ev = { .type = T5_USBC_RAW_SETUP, .bm_request_type = r->bmRequestType, .b_request = r->bRequest,
                                .w_value = r->wValue, .w_index = r->wIndex, .w_length = r->wLength };
        push(&ev, s.ctrl_buf, r->wLength);
    }
    return true;
}

static bool drv_control(uint8_t rhport, uint8_t stage, tusb_control_request_t const *r)
{
    return s.running && on_request(rhport, stage, r);
}

static bool drv_xfer(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred)
{
    (void)rhport;
    ep_t *e = ep_of(ep_addr);
    if (!e || !e->busy) return true;
    e->done_len = xferred;
    e->done_err = result == XFER_RESULT_SUCCESS ? T5_OK : result == XFER_RESULT_STALLED ? T5_ERR_IO : T5_ERR_IO;
    e->busy = false;
    xSemaphoreGive(e->done);
    return true;
}

static const usbd_class_driver_t s_driver = {
    .name = "t5raw",
    .init = drv_init,
    .deinit = drv_deinit,
    .reset = drv_reset,
    .open = drv_open,
    .control_xfer_cb = drv_control,
    .xfer_cb = drv_xfer,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = s.running ? 1 : 0;
    return &s_driver;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
    return s.running && on_request(rhport, stage, request);
}

uint8_t const *tud_descriptor_bos_cb(void)
{
    return s.running ? s.bos : NULL;
}

/* ---- deferred to TinyUSB's task ----------------------------------------------- */
static void do_reply(void *arg)
{
    (void)arg;
    if (!s.pending) return;
    s.pending = false;
    if (s.reply_stall) {
        usbd_edpt_stall(s.rhport, 0x00);
        usbd_edpt_stall(s.rhport, 0x80);
        return;
    }
    uint16_t n = s.reply_len < s.req.wLength ? s.reply_len : s.req.wLength;
    tud_control_xfer(s.rhport, &s.req, n ? s.ctrl_buf : NULL, n);
}

typedef struct {
    uint8_t ep;
    uint16_t len;
} xfer_arg_t;

static void do_xfer(void *arg)
{
    xfer_arg_t *a = arg;
    ep_t *e = ep_of(a->ep);
    bool ok = e && e->open && usbd_edpt_claim(0, a->ep);
    if (ok && !usbd_edpt_xfer(0, a->ep, e->buf, a->len, false)) {
        usbd_edpt_release(0, a->ep);
        ok = false;
    }
    if (!ok && e) {
        e->done_err = T5_ERR_IO;
        e->busy = false;
        xSemaphoreGive(e->done);
    }
    free(a);
}

typedef struct {
    uint8_t ep;
    bool on;
} stall_arg_t;

static void do_stall(void *arg)
{
    stall_arg_t *a = arg;
    if (a->on) usbd_edpt_stall(0, a->ep);
    else usbd_edpt_clear_stall(0, a->ep);
    free(a);
}

/* ---- the API ------------------------------------------------------------------ */
static void on_usb_event(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    t5_usbc_raw_ev_t ev = { 0 };
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED: s.mounted = true; ev.type = T5_USBC_RAW_MOUNTED; break;
    case TINYUSB_EVENT_DETACHED: s.mounted = false; ev.type = T5_USBC_RAW_UNMOUNTED; break;
    case TINYUSB_EVENT_SUSPENDED: ev.type = T5_USBC_RAW_SUSPENDED; break;
    case TINYUSB_EVENT_RESUMED: ev.type = T5_USBC_RAW_RESUMED; break;
    default: return;
    }
    push(&ev, NULL, 0);
}

static void free_desc(void)
{
    free(s.cfg);
    free(s.bos);
    s.cfg = s.bos = NULL;
    for (int d = 0; d < 2; d++) {
        for (int n = 0; n < EP_MAX; n++) {
            ep_t *e = &s.ep[d][n];
            heap_caps_free(e->buf);
            if (e->done) vSemaphoreDelete(e->done);
            memset(e, 0, sizeof *e);
        }
    }
}

/* Check the configuration against what the controller does: endpoints
 * 1..5, interrupt or bulk, full-speed packet sizes. */
static bool config_ok(const uint8_t *cfg, uint16_t total)
{
    if (total < 9 || cfg[1] != TUSB_DESC_CONFIGURATION) return false;
    for (uint16_t i = 0; i + 2 <= total;) {
        uint8_t dl = cfg[i];
        if (dl < 2 || i + dl > total) return false;
        if (cfg[i + 1] == TUSB_DESC_ENDPOINT) {
            if (dl < 7) return false;
            uint8_t n = cfg[i + 2] & 0x0f, type = cfg[i + 3] & 0x03;
            uint16_t mps = (uint16_t)(cfg[i + 4] | cfg[i + 5] << 8);
            if (!n || n >= EP_MAX || (type != TUSB_XFER_BULK && type != TUSB_XFER_INTERRUPT) || mps > 64) return false;
        }
        i += dl;
    }
    return true;
}

t5_err_t t5_usbc_raw_start(t5_excl_t *h, const t5_usbc_raw_desc_t *d)
{
    if (!d || d->size < sizeof *d || !d->device || !d->config || d->string_count > T5_USBC_RAW_MAX_STRINGS) {
        return T5_ERR_INVALID_ARG;
    }
    if (!t5_excl_holds(h, T5_EXCL_USBC) || !s.on) return T5_ERR_STATE;
    if (s.running) return T5_ERR_BUSY;
    if (d->device[0] != 18 || d->device[1] != TUSB_DESC_DEVICE) return T5_ERR_INVALID_ARG;
    uint16_t total = (uint16_t)(d->config[2] | d->config[3] << 8);
    if (!config_ok(d->config, total)) return T5_ERR_INVALID_ARG;
    memcpy(&s.dev, d->device, sizeof s.dev);
    s.dev.bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE;
    s.cfg = malloc(total);
    if (d->bos) {
        uint16_t bl = (uint16_t)(d->bos[2] | d->bos[3] << 8);
        s.bos = malloc(bl);
        if (s.bos) memcpy(s.bos, d->bos, bl);
    }
    t5_err_t e = s.cfg && (!d->bos || s.bos) ? T5_OK : T5_ERR_NO_MEM;
    if (e == T5_OK) memcpy(s.cfg, d->config, total);
    for (int dir = 0; dir < 2 && e == T5_OK; dir++) {
        for (int n = 1; n < EP_MAX && e == T5_OK; n++) {
            ep_t *ep = &s.ep[dir][n];
            ep->buf = heap_caps_malloc(XFER_MAX, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
            ep->done = xSemaphoreCreateBinary();
            if (!ep->buf || !ep->done) e = T5_ERR_NO_MEM;
        }
    }
    static const char lang[] = { 0x09, 0x04 };
    s.str_ptrs[0] = lang;
    s.nstr = 1 + d->string_count;
    for (int i = 0; i < d->string_count; i++) {
        snprintf(s.str[i], sizeof s.str[i], "%s", d->strings && d->strings[i] ? d->strings[i] : "");
        s.str_ptrs[1 + i] = d->strings && d->strings[i] ? s.str[i] : NULL;
    }
    if (e != T5_OK) {
        free_desc();
        return e;
    }
    if (!s.q) s.q = xQueueCreate(QLEN, sizeof(raw_item_t));
    if (!s.q) {
        free_desc();
        return T5_ERR_NO_MEM;
    }
    xQueueReset(s.q);
    s.running = true;                                 /* before install: TinyUSB asks for the driver then */
    tinyusb_config_t tc = TINYUSB_DEFAULT_CONFIG();
    tc.port = TINYUSB_PORT_FULL_SPEED_0;
    tc.descriptor.device = &s.dev;
    tc.descriptor.full_speed_config = s.cfg;
    tc.descriptor.string = s.str_ptrs;
    tc.descriptor.string_count = s.nstr;
    tc.event_cb = on_usb_event;
    e = usbc_device_begin(&tc);
    if (e != T5_OK) {
        s.running = false;
        free_desc();
        return e;
    }
    T5_LOGI(TAG, "raw device %04x:%04x up", s.dev.idVendor, s.dev.idProduct);
    return T5_OK;
}

int t5_usbc_raw_next(t5_excl_t *h, t5_usbc_raw_ev_t *ev, void *buf, size_t cap, uint32_t timeout_ms)
{
    if (!ev || (cap && !buf)) return T5_ERR_INVALID_ARG;
    if (!t5_excl_holds(h, T5_EXCL_USBC) || !s.q) return T5_ERR_STATE;
    raw_item_t *it = malloc(sizeof *it);
    if (!it) return T5_ERR_NO_MEM;
    TickType_t t = timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    int r = T5_ERR_TIMEOUT;
    if (xQueueReceive(s.q, it, t) == pdTRUE) {
        *ev = it->ev;
        size_t k = it->len < cap ? it->len : cap;
        if (k) memcpy(buf, it->data, k);
        r = (int)k;
    } else if (!t5_excl_holds(h, T5_EXCL_USBC)) {
        r = T5_ERR_STATE;
    }
    free(it);
    return r;
}

t5_err_t t5_usbc_raw_ctrl_reply(t5_excl_t *h, const void *data, size_t len)
{
    if ((len && !data) || len > T5_USBC_RAW_CTRL_MAX) return T5_ERR_INVALID_ARG;
    if (!t5_excl_holds(h, T5_EXCL_USBC) || !s.running) return T5_ERR_STATE;
    if (!s.pending) return T5_ERR_STATE;
    if (len) memcpy(s.ctrl_buf, data, len);
    s.reply_len = (uint16_t)len;
    s.reply_stall = false;
    usbd_defer_func(do_reply, NULL, false);
    return T5_OK;
}

t5_err_t t5_usbc_raw_ctrl_stall(t5_excl_t *h)
{
    if (!t5_excl_holds(h, T5_EXCL_USBC) || !s.running || !s.pending) return T5_ERR_STATE;
    s.reply_stall = true;
    usbd_defer_func(do_reply, NULL, false);
    return T5_OK;
}

int t5_usbc_raw_xfer(t5_excl_t *h, uint8_t ep, void *buf, size_t len, uint32_t timeout_ms)
{
    ep_t *e = ep_of(ep);
    if (!e || !buf || !len || len > XFER_MAX) return T5_ERR_INVALID_ARG;
    if (!t5_excl_holds(h, T5_EXCL_USBC) || !s.running || !s.mounted) return T5_ERR_STATE;
    if (!e->open) return T5_ERR_NOT_FOUND;
    bool in_dev = ep & 0x80;                              /* device to computer */
    if (!e->busy) {
        if (in_dev) memcpy(e->buf, buf, len);
        xSemaphoreTake(e->done, 0);
        e->busy = true;
        xfer_arg_t *a = malloc(sizeof *a);
        if (!a) {
            e->busy = false;
            return T5_ERR_NO_MEM;
        }
        *a = (xfer_arg_t){ ep, (uint16_t)len };
        usbd_defer_func(do_xfer, a, false);
    }
    TickType_t t = timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(e->done, t) != pdTRUE) return T5_ERR_TIMEOUT;   /* still queued: call again */
    if (e->done_err != T5_OK) return e->done_err;
    size_t n = e->done_len < len ? e->done_len : len;
    if (!in_dev) memcpy(buf, e->buf, n);
    return (int)n;
}

t5_err_t t5_usbc_raw_stall(t5_excl_t *h, uint8_t ep, bool on)
{
    if (!ep_of(ep)) return T5_ERR_INVALID_ARG;
    if (!t5_excl_holds(h, T5_EXCL_USBC) || !s.running) return T5_ERR_STATE;
    stall_arg_t *a = malloc(sizeof *a);
    if (!a) return T5_ERR_NO_MEM;
    *a = (stall_arg_t){ ep, on };
    usbd_defer_func(do_stall, a, false);
    return T5_OK;
}

/* ---- the claim's provider ----------------------------------------------------- */
static t5_err_t excl_take(void)
{
    if (t5_usbc_mode() != T5_USBC_CONSOLE) return T5_ERR_BUSY;   /* mass storage or an app's device mode */
    s.on = true;
    return T5_OK;
}

static void excl_give(void)
{
    s.on = false;
    if (s.running) {
        usbc_device_end();                                       /* TinyUSB stops; the console comes back */
        s.running = false;
        s.mounted = false;
        s.pending = false;
        free_desc();
    }
    t5_usbc_raw_ev_t ev = { .type = T5_USBC_RAW_UNMOUNTED };
    push(&ev, NULL, 0);
}

void usbc_raw_init(void)
{
    static const t5_excl_provider_t prov = { excl_take, excl_give };
    t5_excl_set_provider(T5_EXCL_USBC, &prov);
}
