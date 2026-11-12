/* SPDX-License-Identifier: Apache-2.0
 * pcap records for Wi-Fi monitor captures (t5_net/wifimon.h): radiotap
 * headers with flags, channel, signal and noise. Pure.
 */
#include <string.h>
#include "t5_net/wifimon.h"

#define RT_LEN 16     /* radiotap: header 8, flags 1, pad 1, channel 4, signal 1, noise 1 */

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

size_t t5_wifimon_pcap_header(uint8_t out[24])
{
    put32(out, 0xa1b2c3d4u);        /* microsecond timestamps */
    put16(out + 4, 2);
    put16(out + 6, 4);
    put32(out + 8, 0);              /* GMT offset */
    put32(out + 12, 0);             /* accuracy */
    put32(out + 16, 65535);         /* snap length */
    put32(out + 20, 127);           /* LINKTYPE_IEEE802_11_RADIOTAP */
    return 24;
}

size_t t5_wifimon_pcap_record(const t5_wifimon_frame_t *meta, const void *frame, size_t n, uint8_t *out, size_t cap)
{
    if (!meta || (n && !frame) || !out) return 0;
    size_t total = 16 + RT_LEN + n;
    if (cap < total) return 0;
    put32(out, meta->time_us / 1000000u);
    put32(out + 4, meta->time_us % 1000000u);
    put32(out + 8, (uint32_t)(RT_LEN + n));
    put32(out + 12, (uint32_t)(RT_LEN + (meta->len > n ? meta->len : n)));
    uint8_t *rt = out + 16;
    memset(rt, 0, RT_LEN);
    put16(rt + 2, RT_LEN);
    put32(rt + 4, 1u << 1 | 1u << 3 | 1u << 5 | 1u << 6);   /* flags, channel, signal, noise */
    rt[8] = meta->fcs_bad ? 0x40 : 0x00;                      /* no FCS in the data */
    uint16_t mhz = meta->channel == 14 ? 2484 : (uint16_t)(2407 + 5 * meta->channel);
    put16(rt + 10, meta->channel ? mhz : 0);
    put16(rt + 12, 0x0080);                                   /* 2 GHz */
    rt[14] = (uint8_t)meta->rssi;
    rt[15] = (uint8_t)meta->noise_floor;
    if (n) memcpy(out + 16 + RT_LEN, frame, n);
    return total;
}
