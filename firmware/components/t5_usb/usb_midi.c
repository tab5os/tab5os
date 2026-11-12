/* SPDX-License-Identifier: Apache-2.0
 * USB MIDI 1.0 (plan 7.7): no ESP-IDF class driver exists, so this claims
 * the MIDI streaming interface itself (class 1, subclass 3) and runs one
 * bulk IN transfer at a time. Events are 4-byte packets: cable and code
 * index (CIN), then up to 3 MIDI bytes.
 */
#include <string.h>
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "usb_priv.h"

static const char *TAG = "t5.usb";

typedef struct {
    usb_device_handle_t dev;
    uint8_t addr, iface;
    uint8_t ep_in, ep_out;
    uint16_t mps_in, mps_out;
    usb_transfer_t *in, *out;
    bool claimed;
} midi_t;

static midi_t s_m;
static t5_usb_midi_cb_t s_cb;
static void *s_cb_arg;
static uint32_t s_in_msgs, s_out_msgs;

static void in_done(usb_transfer_t *t)
{
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        for (int i = 0; i + 4 <= t->actual_num_bytes; i += 4) {
            uint8_t msg[3];
            size_t n = usb_midi_parse(&t->data_buffer[i], msg);
            if (n) {
                s_in_msgs++;
                if (s_cb) s_cb(msg, n, s_cb_arg);
            }
        }
    }
    if (s_m.claimed && t->status != USB_TRANSFER_STATUS_NO_DEVICE && t->status != USB_TRANSFER_STATUS_CANCELED) {
        usb_host_transfer_submit(t);
    }
}

static void out_done(usb_transfer_t *t)
{
    (void)t;
}

void usb_midi_check(usb_device_handle_t h, uint8_t addr, const usb_config_desc_t *cfg)
{
    if (s_m.claimed) return;                    /* one MIDI device */
    int off = 0;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
    while ((d = usb_parse_next_descriptor_of_type(d, cfg->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE, &off))) {
        const usb_intf_desc_t *i = (const usb_intf_desc_t *)d;
        if (i->bInterfaceClass != USB_CLASS_AUDIO || i->bInterfaceSubClass != USB_SUBCLASS_MIDISTREAMING) continue;
        midi_t m = { .dev = h, .addr = addr, .iface = i->bInterfaceNumber };
        int eoff = off;
        for (int k = 0; k < i->bNumEndpoints; k++) {
            const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(i, k, cfg->wTotalLength, &eoff);
            eoff = off;
            if (!ep || (ep->bmAttributes & 0x03) != USB_BM_ATTRIBUTES_XFER_BULK) continue;
            if (ep->bEndpointAddress & 0x80) {
                m.ep_in = ep->bEndpointAddress;
                m.mps_in = ep->wMaxPacketSize;
            } else {
                m.ep_out = ep->bEndpointAddress;
                m.mps_out = ep->wMaxPacketSize;
            }
        }
        if (!m.ep_in) continue;
        if (usb_host_interface_claim(usb_client(), h, m.iface, i->bAlternateSetting) != ESP_OK) return;
        if (usb_host_transfer_alloc(m.mps_in, 0, &m.in) != ESP_OK) {
            usb_host_interface_release(usb_client(), h, m.iface);
            return;
        }
        m.in->device_handle = h;
        m.in->bEndpointAddress = m.ep_in;
        m.in->callback = in_done;
        m.in->num_bytes = m.mps_in;
        if (m.ep_out && usb_host_transfer_alloc(64, 0, &m.out) == ESP_OK) {
            m.out->device_handle = h;
            m.out->bEndpointAddress = m.ep_out;
            m.out->callback = out_done;
        }
        m.claimed = true;
        s_m = m;
        usb_host_transfer_submit(s_m.in);
        usb_dev_set_class(addr, T5_USB_MIDI, true);
        T5_LOGI(TAG, "MIDI device on interface %u (in 0x%02x, out 0x%02x)", m.iface, m.ep_in, m.ep_out);
        return;
    }
}

void usb_midi_release_if_unused(usb_device_handle_t h, uint8_t addr)
{
    (void)addr;
    if (!(s_m.claimed && s_m.dev == h)) usb_host_device_close(usb_client(), h);
}

void usb_midi_gone(usb_device_handle_t h)
{
    if (!s_m.claimed || s_m.dev != h) return;
    s_m.claimed = false;
    usb_host_endpoint_halt(h, s_m.ep_in);
    usb_host_endpoint_flush(h, s_m.ep_in);
    usb_host_endpoint_clear(h, s_m.ep_in);
    usb_host_transfer_free(s_m.in);
    if (s_m.out) usb_host_transfer_free(s_m.out);
    usb_host_interface_release(usb_client(), h, s_m.iface);
    usb_host_device_close(usb_client(), h);
    memset(&s_m, 0, sizeof s_m);
    T5_LOGI(TAG, "MIDI device gone");
}

bool usb_midi_owns(uint8_t addr, uint8_t iface)
{
    return s_m.claimed && s_m.addr == addr && s_m.iface == iface;
}

/* Like usb_midi_gone, with the device still there: an app takes it. */
void usb_midi_detach(void)
{
    if (!s_m.claimed) return;
    usb_device_handle_t h = s_m.dev;
    s_m.claimed = false;                        /* in_done stops resubmitting */
    usb_host_endpoint_halt(h, s_m.ep_in);
    usb_host_endpoint_flush(h, s_m.ep_in);
    usb_host_endpoint_clear(h, s_m.ep_in);
    t5_delay_ms(20);                            /* the cancelled transfer comes back */
    usb_host_transfer_free(s_m.in);
    if (s_m.out) usb_host_transfer_free(s_m.out);
    usb_host_interface_release(usb_client(), h, s_m.iface);
    usb_host_device_close(usb_client(), h);
    memset(&s_m, 0, sizeof s_m);
    T5_LOGI(TAG, "MIDI device handed to an app");
}

void usb_midi_init(void)
{
}

bool t5_usb_midi_present(void)
{
    return s_m.claimed;
}

t5_err_t t5_usb_midi_listen(t5_usb_midi_cb_t cb, void *arg)
{
    s_cb_arg = arg;
    s_cb = cb;
    return T5_OK;
}

t5_err_t t5_usb_midi_send(const uint8_t *msg, size_t len)
{
    if (!s_m.claimed || !s_m.out) return T5_ERR_STATE;
    uint8_t pkt[4];
    if (!usb_midi_pack(msg, len, pkt)) return T5_ERR_INVALID_ARG;
    memcpy(s_m.out->data_buffer, pkt, 4);
    s_m.out->num_bytes = 4;
    s_out_msgs++;
    return usb_host_transfer_submit(s_m.out) == ESP_OK ? T5_OK : T5_ERR_BUSY;
}
