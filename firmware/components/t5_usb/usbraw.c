/* SPDX-License-Identifier: Apache-2.0
 * Raw USB-A access for apps (the "raw access" part of t5_usb/usb.h).
 *
 * Handles are device opens through the usb.t5 client, so their transfer
 * callbacks run on its task, and one client owns every interface an app
 * claims. Every transfer carries a small context whose state settles who
 * frees it: the waiter if the transfer completed in time, the callback if
 * the waiter gave up (a control transfer can't be cancelled).
 */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_core/res.h"
#include "usb_priv.h"

static const char *TAG = "t5.usbraw";

#define MAGIC 0x55524157u      /* "URAW" */
#define MAX_STREAMS 4
#define STREAM_QLEN 16
#define STREAM_ITEM_MAX 512

enum { X_PENDING, X_DONE, X_ORPHAN };

typedef struct {
    atomic_int state;
    SemaphoreHandle_t sem;     /* the waiter's; NULL for stream transfers */
    struct stream *stream;
} xctx_t;

typedef struct stream {
    struct t5_usb_handle *h;
    uint8_t ep;
    uint16_t mps;
    volatile bool on;
    uint8_t depth;
    atomic_int in_flight;
    usb_transfer_t *x[8];
    xctx_t ctx[8];
    QueueHandle_t q;           /* items: uint16_t length, then the bytes */
} stream_t;

struct t5_usb_handle {
    uint32_t magic;
    usb_device_handle_t dev;
    uint8_t addr;
    volatile bool gone;
    uint32_t claimed;          /* by interface number */
    uint32_t taken;            /* claimed from an OS driver */
    uint8_t alt[32];
    t5_mutex_t mx;             /* one synchronous transfer at a time per handle */
    stream_t streams[MAX_STREAMS];
};

static t5_usb_handle_t *s_h[T5_USB_MAX_HANDLES];
static t5_mutex_t s_mx;

static t5_mutex_t mx(void)
{
    return t5_mutex_once(&s_mx);
}

static bool valid(const t5_usb_handle_t *h)
{
    return h && h->magic == MAGIC;
}

/* ---- descriptors ---------------------------------------------------------- */
typedef struct {
    uint8_t iface, alt, attr;
    uint16_t mps;
} ep_info_t;

/* The endpoint ep in iface/alt of the active configuration (any alt when
 * want_alt is 0xff). */
static bool find_ep(t5_usb_handle_t *h, uint8_t ep, ep_info_t *out)
{
    const usb_config_desc_t *cd;
    if (usb_host_get_active_config_descriptor(h->dev, &cd) != ESP_OK) return false;
    const uint8_t *p = (const uint8_t *)cd;
    int iface = -1, alt = -1;
    for (int i = 0; i + 2 <= cd->wTotalLength && p[i] >= 2; i += p[i]) {
        if (p[i + 1] == USB_B_DESCRIPTOR_TYPE_INTERFACE && p[i] >= 9) {
            iface = p[i + 2];
            alt = p[i + 3];
        } else if (p[i + 1] == USB_B_DESCRIPTOR_TYPE_ENDPOINT && p[i] >= 7 && p[i + 2] == ep && iface >= 0 &&
                   iface < 32 && (h->claimed & (1u << iface)) && h->alt[iface] == alt) {
            *out = (ep_info_t){ (uint8_t)iface, (uint8_t)alt, p[i + 3], (uint16_t)((p[i + 4] | p[i + 5] << 8) & 0x7ff) };
            return true;
        }
    }
    return false;
}

static bool iface_exists(t5_usb_handle_t *h, uint8_t iface, uint8_t alt, uint8_t *cls)
{
    const usb_config_desc_t *cd;
    if (usb_host_get_active_config_descriptor(h->dev, &cd) != ESP_OK) return false;
    const uint8_t *p = (const uint8_t *)cd;
    for (int i = 0; i + 2 <= cd->wTotalLength && p[i] >= 2; i += p[i]) {
        if (p[i + 1] == USB_B_DESCRIPTOR_TYPE_INTERFACE && p[i] >= 9 && p[i + 2] == iface && p[i + 3] == alt) {
            *cls = p[i + 5];
            return true;
        }
    }
    return false;
}

/* ---- synchronous transfers ------------------------------------------------ */
static void sync_done(usb_transfer_t *x)
{
    xctx_t *c = x->context;
    if (atomic_exchange(&c->state, X_DONE) == X_ORPHAN) {
        /* The waiter gave up: this side frees. */
        vSemaphoreDelete(c->sem);
        free(c);
        usb_host_transfer_free(x);
        return;
    }
    xSemaphoreGive(c->sem);
}

static t5_err_t status_err(usb_transfer_status_t st)
{
    switch (st) {
    case USB_TRANSFER_STATUS_COMPLETED: return T5_OK;
    case USB_TRANSFER_STATUS_STALL: return T5_ERR_IO;
    case USB_TRANSFER_STATUS_NO_DEVICE: return T5_ERR_STATE;
    case USB_TRANSFER_STATUS_TIMED_OUT:
    case USB_TRANSFER_STATUS_CANCELED: return T5_ERR_TIMEOUT;
    default: return T5_ERR_IO;
    }
}

/* Submit x and wait. On timeout a bulk/interrupt transfer is cancelled
 * (halt, flush, clear); a control one is left to the callback to free.
 * Returns the status; *x_out is NULL when the callback owns x now. */
static t5_err_t run(t5_usb_handle_t *h, usb_transfer_t **x_out, bool control, uint8_t ep, uint32_t timeout_ms)
{
    usb_transfer_t *x = *x_out;
    xctx_t *c = calloc(1, sizeof *c);
    if (!c) return T5_ERR_NO_MEM;
    c->sem = xSemaphoreCreateBinary();
    if (!c->sem) {
        free(c);
        return T5_ERR_NO_MEM;
    }
    atomic_init(&c->state, X_PENDING);
    x->context = c;
    x->callback = sync_done;
    x->device_handle = h->dev;
    esp_err_t e = control ? usb_host_transfer_submit_control(usb_client(), x) : usb_host_transfer_submit(x);
    if (e != ESP_OK) {
        vSemaphoreDelete(c->sem);
        free(c);
        return h->gone ? T5_ERR_STATE : T5_ERR_IO;
    }
    TickType_t t = timeout_ms == T5_USB_FOREVER ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(c->sem, t) != pdTRUE) {
        if (!control) {
            usb_host_endpoint_halt(h->dev, ep);
            usb_host_endpoint_flush(h->dev, ep);
            usb_host_endpoint_clear(h->dev, ep);
            if (xSemaphoreTake(c->sem, pdMS_TO_TICKS(500)) == pdTRUE) {
                vSemaphoreDelete(c->sem);
                free(c);
                return T5_ERR_TIMEOUT;
            }
        }
        if (atomic_exchange(&c->state, X_ORPHAN) == X_PENDING) {
            *x_out = NULL;                         /* sync_done frees it */
            return T5_ERR_TIMEOUT;
        }
        xSemaphoreTake(c->sem, portMAX_DELAY);     /* it finished just now */
    }
    vSemaphoreDelete(c->sem);
    free(c);
    return status_err(x->status);
}

int t5_usb_control(t5_usb_handle_t *h, uint8_t bm_request_type, uint8_t b_request, uint16_t w_value,
                   uint16_t w_index, void *data, uint16_t len, uint32_t timeout_ms)
{
    if (!valid(h) || (len && !data) || len > T5_USB_XFER_MAX) return T5_ERR_INVALID_ARG;
    if (h->gone) return T5_ERR_STATE;
    usb_transfer_t *x;
    if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + ((len + 63u) & ~63u), 0, &x) != ESP_OK) return T5_ERR_NO_MEM;
    usb_setup_packet_t *sp = (usb_setup_packet_t *)x->data_buffer;
    sp->bmRequestType = bm_request_type;
    sp->bRequest = b_request;
    sp->wValue = w_value;
    sp->wIndex = w_index;
    sp->wLength = len;
    bool in = bm_request_type & 0x80;
    if (!in && len) memcpy(x->data_buffer + sizeof *sp, data, len);
    x->bEndpointAddress = 0;
    x->num_bytes = (int)(sizeof *sp + len);
    t5_mutex_lock(h->mx);
    t5_err_t e = run(h, &x, true, 0, timeout_ms);
    t5_mutex_unlock(h->mx);
    if (!x) return e;
    int moved = x->actual_num_bytes > (int)sizeof *sp ? x->actual_num_bytes - (int)sizeof *sp : 0;
    if (moved > len) moved = len;
    if (e == T5_OK && in && moved) memcpy(data, x->data_buffer + sizeof *sp, (size_t)moved);
    usb_host_transfer_free(x);
    return e == T5_OK ? moved : e;
}

int t5_usb_transfer(t5_usb_handle_t *h, uint8_t ep, void *data, size_t len, uint32_t timeout_ms)
{
    if (!valid(h) || !data || !len || len > T5_USB_XFER_MAX || !(ep & 0x0f)) return T5_ERR_INVALID_ARG;
    if (h->gone) return T5_ERR_STATE;
    ep_info_t ei;
    if (!find_ep(h, ep, &ei)) return T5_ERR_NOT_FOUND;     /* not on a claimed interface */
    uint8_t type = ei.attr & 0x03;
    if (type != USB_BM_ATTRIBUTES_XFER_BULK && type != USB_BM_ATTRIBUTES_XFER_INT) return T5_ERR_NOT_SUPPORTED;
    bool in = ep & 0x80;
    /* IN transfers are whole packets. */
    size_t n = in ? (len + ei.mps - 1) / ei.mps * ei.mps : len;
    usb_transfer_t *x;
    if (usb_host_transfer_alloc(n, 0, &x) != ESP_OK) return T5_ERR_NO_MEM;
    if (!in) memcpy(x->data_buffer, data, len);
    x->bEndpointAddress = ep;
    x->num_bytes = (int)n;
    t5_mutex_lock(h->mx);
    t5_err_t e = run(h, &x, false, ep, timeout_ms);
    t5_mutex_unlock(h->mx);
    if (!x) return e;
    int moved = x->actual_num_bytes < (int)len ? x->actual_num_bytes : (int)len;
    if (e == T5_OK && in) memcpy(data, x->data_buffer, (size_t)moved);
    usb_host_transfer_free(x);
    return e == T5_OK ? moved : e;
}

int t5_usb_string(t5_usb_handle_t *h, uint8_t index, char *out, size_t cap)
{
    if (!valid(h) || !out || !cap || !index) return T5_ERR_INVALID_ARG;
    uint8_t buf[255];
    int n = t5_usb_control(h, 0x80, 6, (uint16_t)(3u << 8 | index), 0x0409, buf, sizeof buf, 1000);
    if (n < 0) return n;
    if (n < 2 || buf[1] != 3) return T5_ERR_IO;
    if (buf[0] < n) n = buf[0];
    size_t k = 0;
    for (int i = 2; i + 1 < n; i += 2) {
        uint32_t ch = (uint32_t)(buf[i] | buf[i + 1] << 8);
        if (ch >= 0xd800 && ch <= 0xdfff) ch = 0xfffd;        /* surrogates: not worth pairing here */
        uint8_t u[3];
        int ul = ch < 0x80 ? 1 : ch < 0x800 ? 2 : 3;
        if (ul == 1) u[0] = (uint8_t)ch;
        else if (ul == 2) {
            u[0] = (uint8_t)(0xc0 | ch >> 6);
            u[1] = (uint8_t)(0x80 | (ch & 0x3f));
        } else {
            u[0] = (uint8_t)(0xe0 | ch >> 12);
            u[1] = (uint8_t)(0x80 | ((ch >> 6) & 0x3f));
            u[2] = (uint8_t)(0x80 | (ch & 0x3f));
        }
        if (k + (size_t)ul + 1 > cap) break;
        memcpy(out + k, u, (size_t)ul);
        k += (size_t)ul;
    }
    out[k] = 0;
    return (int)k;
}

int t5_usb_device_desc(t5_usb_handle_t *h, void *buf, size_t cap)
{
    if (!valid(h) || (cap && !buf)) return T5_ERR_INVALID_ARG;
    if (h->gone) return T5_ERR_STATE;
    const usb_device_desc_t *dd;
    if (usb_host_get_device_descriptor(h->dev, &dd) != ESP_OK) return T5_ERR_IO;
    size_t n = sizeof *dd < cap ? sizeof *dd : cap;
    memcpy(buf, dd, n);
    return (int)sizeof *dd;
}

int t5_usb_config_desc(t5_usb_handle_t *h, void *buf, size_t cap)
{
    if (!valid(h) || (cap && !buf)) return T5_ERR_INVALID_ARG;
    if (h->gone) return T5_ERR_STATE;
    const usb_config_desc_t *cd;
    if (usb_host_get_active_config_descriptor(h->dev, &cd) != ESP_OK) return T5_ERR_IO;
    size_t n = cd->wTotalLength < cap ? cd->wTotalLength : cap;
    memcpy(buf, cd, n);
    return cd->wTotalLength;
}

/* ---- claims --------------------------------------------------------------- */
t5_err_t t5_usb_claim(t5_usb_handle_t *h, uint8_t iface, uint8_t alt, uint32_t flags)
{
    if (!valid(h) || iface >= 32 || (flags & ~(uint32_t)T5_USB_CLAIM_TAKE_OVER)) return T5_ERR_INVALID_ARG;
    if (h->gone) return T5_ERR_STATE;
    if (h->claimed & (1u << iface)) return h->alt[iface] == alt ? T5_OK : T5_ERR_BUSY;
    uint8_t cls;
    if (!iface_exists(h, iface, alt, &cls)) return T5_ERR_NOT_FOUND;
    if (cls == USB_CLASS_HUB) return T5_ERR_NOT_SUPPORTED;
    if (usb_host_interface_claim(usb_client(), h->dev, iface, alt) == ESP_OK) {
        h->claimed |= 1u << iface;
        h->alt[iface] = alt;
        return T5_OK;
    }
    /* Someone has it: one of the OS's drivers, or another app. */
    usb_owner_t owner = usb_iface_owner(h->addr, iface);
    if (owner == USB_OWNER_NONE || owner == USB_OWNER_BUSY) return T5_ERR_BUSY;
    if (!(flags & T5_USB_CLAIM_TAKE_OVER)) return T5_ERR_BUSY;
    int pe = t5_core_perm_check(T5_CORE_CAP_USB_TAKEOVER);
    if (pe != T5_OK) return (t5_err_t)pe;
    T5_RETURN_ON_ERR(usb_iface_detach(h->addr, iface, owner));
    if (usb_host_interface_claim(usb_client(), h->dev, iface, alt) != ESP_OK) {
        usb_iface_reattach(h->dev, h->addr, owner);
        return T5_ERR_BUSY;
    }
    T5_LOGI(TAG, "addr %u interface %u taken over from the OS", h->addr, iface);
    h->claimed |= 1u << iface;
    h->taken |= 1u << iface;
    h->alt[iface] = alt;
    return T5_OK;
}

static void stream_stop(stream_t *s);

t5_err_t t5_usb_release(t5_usb_handle_t *h, uint8_t iface)
{
    if (!valid(h) || iface >= 32) return T5_ERR_INVALID_ARG;
    if (!(h->claimed & (1u << iface))) return T5_ERR_STATE;
    for (int i = 0; i < MAX_STREAMS; i++) {
        stream_t *s = &h->streams[i];
        ep_info_t ei;
        if (s->q && find_ep(h, s->ep, &ei) && ei.iface == iface) stream_stop(s);
    }
    if (!h->gone) usb_host_interface_release(usb_client(), h->dev, iface);
    h->claimed &= ~(1u << iface);
    if (h->taken & (1u << iface)) {
        h->taken &= ~(1u << iface);
        if (!h->gone) usb_iface_reattach(h->dev, h->addr, usb_iface_owner_was(h->addr, iface));
    }
    return T5_OK;
}

/* ---- streams -------------------------------------------------------------- */
static void stream_done(usb_transfer_t *x)
{
    xctx_t *c = x->context;
    stream_t *s = c->stream;
    if (x->status == USB_TRANSFER_STATUS_COMPLETED && s->on) {
        uint8_t item[sizeof(uint16_t) + STREAM_ITEM_MAX];
        uint16_t n = (uint16_t)(x->actual_num_bytes < STREAM_ITEM_MAX ? x->actual_num_bytes : STREAM_ITEM_MAX);
        memcpy(item, &n, sizeof n);
        memcpy(item + sizeof n, x->data_buffer, n);
        xQueueSend(s->q, item, 0);                 /* full: dropped */
        if (s->on && !s->h->gone && usb_host_transfer_submit(x) == ESP_OK) return;
    }
    atomic_fetch_sub(&s->in_flight, 1);
}

t5_err_t t5_usb_stream_start(t5_usb_handle_t *h, uint8_t ep, uint8_t depth)
{
    if (!valid(h) || !(ep & 0x80) || !depth || depth > 8) return T5_ERR_INVALID_ARG;
    if (h->gone) return T5_ERR_STATE;
    ep_info_t ei;
    if (!find_ep(h, ep, &ei)) return T5_ERR_NOT_FOUND;
    uint8_t type = ei.attr & 0x03;
    if (type != USB_BM_ATTRIBUTES_XFER_BULK && type != USB_BM_ATTRIBUTES_XFER_INT) return T5_ERR_NOT_SUPPORTED;
    if (ei.mps > STREAM_ITEM_MAX) return T5_ERR_NOT_SUPPORTED;
    stream_t *s = NULL;
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (h->streams[i].q && h->streams[i].ep == ep) return T5_ERR_BUSY;
        if (!h->streams[i].q && !s) s = &h->streams[i];
    }
    if (!s) return T5_ERR_NO_MEM;
    *s = (stream_t){ .h = h, .ep = ep, .mps = ei.mps, .depth = depth };
    atomic_init(&s->in_flight, 0);
    s->q = xQueueCreate(STREAM_QLEN, sizeof(uint16_t) + STREAM_ITEM_MAX);
    if (!s->q) return T5_ERR_NO_MEM;
    s->on = true;
    for (int i = 0; i < depth; i++) {
        if (usb_host_transfer_alloc(ei.mps, 0, &s->x[i]) != ESP_OK) break;
        s->ctx[i].stream = s;
        s->x[i]->context = &s->ctx[i];
        s->x[i]->callback = stream_done;
        s->x[i]->device_handle = h->dev;
        s->x[i]->bEndpointAddress = ep;
        s->x[i]->num_bytes = ei.mps;
        atomic_fetch_add(&s->in_flight, 1);
        if (usb_host_transfer_submit(s->x[i]) != ESP_OK) atomic_fetch_sub(&s->in_flight, 1);
    }
    if (atomic_load(&s->in_flight) == 0) {
        stream_stop(s);
        return T5_ERR_IO;
    }
    return T5_OK;
}

static void stream_stop(stream_t *s)
{
    if (!s->q) return;
    s->on = false;
    t5_usb_handle_t *h = s->h;
    if (atomic_load(&s->in_flight) > 0 && !h->gone) {
        usb_host_endpoint_halt(h->dev, s->ep);
        usb_host_endpoint_flush(h->dev, s->ep);
        usb_host_endpoint_clear(h->dev, s->ep);
    }
    /* The cancelled transfers come back through the client task. */
    for (int i = 0; i < 100 && atomic_load(&s->in_flight) > 0; i++) t5_delay_ms(10);
    if (atomic_load(&s->in_flight) > 0) {
        T5_LOGW(TAG, "stream 0x%02x: %d transfers didn't come back; leaking them", s->ep, atomic_load(&s->in_flight));
    } else {
        for (int i = 0; i < 8; i++) {
            if (s->x[i]) usb_host_transfer_free(s->x[i]);
        }
        vQueueDelete(s->q);
    }
    memset(s->x, 0, sizeof s->x);
    s->q = NULL;
}

static stream_t *stream_of(t5_usb_handle_t *h, uint8_t ep)
{
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (h->streams[i].q && h->streams[i].ep == ep) return &h->streams[i];
    }
    return NULL;
}

int t5_usb_stream_next(t5_usb_handle_t *h, uint8_t ep, void *buf, size_t cap, uint32_t timeout_ms)
{
    if (!valid(h) || (cap && !buf)) return T5_ERR_INVALID_ARG;
    stream_t *s = stream_of(h, ep);
    if (!s) return T5_ERR_NOT_FOUND;
    uint8_t item[sizeof(uint16_t) + STREAM_ITEM_MAX];
    TickType_t t = timeout_ms == T5_USB_FOREVER ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (h->gone) t = 0;
    if (xQueueReceive(s->q, item, t) != pdTRUE) return h->gone ? T5_ERR_STATE : T5_ERR_TIMEOUT;
    uint16_t n;
    memcpy(&n, item, sizeof n);
    size_t k = n < cap ? n : cap;
    memcpy(buf, item + sizeof n, k);
    return (int)k;
}

t5_err_t t5_usb_stream_stop(t5_usb_handle_t *h, uint8_t ep)
{
    if (!valid(h)) return T5_ERR_INVALID_ARG;
    stream_t *s = stream_of(h, ep);
    if (!s) return T5_ERR_NOT_FOUND;
    stream_stop(s);
    return T5_OK;
}

/* ---- open and close ------------------------------------------------------- */
static void release_handle(void *h)
{
    T5_IGNORE(t5_usb_close(h));
}

t5_err_t t5_usb_open(uint8_t addr, t5_usb_handle_t **out)
{
    if (!out || !addr) return T5_ERR_INVALID_ARG;
    *out = NULL;
    if (!usb_client()) return T5_ERR_STATE;
    t5_usb_handle_t *h = calloc(1, sizeof *h);
    if (!h) return T5_ERR_NO_MEM;
    h->magic = MAGIC;
    h->addr = addr;
    h->mx = t5_mutex_create();
    if (!h->mx) {
        free(h);
        return T5_ERR_NO_MEM;
    }
    if (usb_host_device_open(usb_client(), addr, &h->dev) != ESP_OK) {
        t5_mutex_delete(h->mx);
        free(h);
        return T5_ERR_NOT_FOUND;
    }
    t5_mutex_lock(mx());
    int slot = -1;
    for (int i = 0; i < T5_USB_MAX_HANDLES && slot < 0; i++) {
        if (!s_h[i]) slot = i;
    }
    if (slot >= 0) s_h[slot] = h;
    t5_mutex_unlock(mx());
    if (slot < 0) {
        usb_host_device_close(usb_client(), h->dev);
        t5_mutex_delete(h->mx);
        free(h);
        return T5_ERR_BUSY;
    }
    t5_res_add(T5_RES_OTHER, h, release_handle);
    *out = h;
    return T5_OK;
}

bool t5_usb_handle_up(t5_usb_handle_t *h)
{
    return valid(h) && !h->gone;
}

t5_err_t t5_usb_close(t5_usb_handle_t *h)
{
    if (!valid(h)) return T5_ERR_INVALID_ARG;
    t5_res_remove(h);
    for (int i = 0; i < MAX_STREAMS; i++) stream_stop(&h->streams[i]);
    for (uint8_t i = 0; i < 32; i++) {
        if (h->claimed & (1u << i)) T5_IGNORE(t5_usb_release(h, i));
    }
    t5_mutex_lock(mx());
    for (int i = 0; i < T5_USB_MAX_HANDLES; i++) {
        if (s_h[i] == h) s_h[i] = NULL;
    }
    t5_mutex_unlock(mx());
    t5_mutex_lock(h->mx);                      /* no transfer of ours still running */
    t5_mutex_unlock(h->mx);
    usb_host_device_close(usb_client(), h->dev);
    t5_mutex_delete(h->mx);
    h->magic = 0;
    free(h);
    return T5_OK;
}

/* The usb.t5 client task: a device we hold open is gone. */
void usbraw_gone(usb_device_handle_t dev)
{
    t5_mutex_lock(mx());
    for (int i = 0; i < T5_USB_MAX_HANDLES; i++) {
        t5_usb_handle_t *h = s_h[i];
        if (h && h->dev == dev && !h->gone) {
            h->gone = true;
            for (int k = 0; k < MAX_STREAMS; k++) h->streams[k].on = false;
            T5_LOGI(TAG, "addr %u unplugged with an app's handle open", h->addr);
        }
    }
    t5_mutex_unlock(mx());
}

