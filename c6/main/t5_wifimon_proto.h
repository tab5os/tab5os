/* SPDX-License-Identifier: Apache-2.0
 * Wi-Fi monitor mode and raw frames between the P4 (t5_net/wifimon.c) and
 * this radio firmware (monitor.c), over esp_hosted's peer data messages.
 * esp_hosted forwards neither promiscuous mode nor raw transmit, so these
 * run here and the frames travel to the P4 in batches.
 *
 * Both sides include this file; everything is little-endian and packed.
 */
#pragma once
#include <stdint.h>

#define T5WM_PROTO_VERSION 1

/* Message ids. */
#define T5WM_MSG_CMD 0x54350001u      /* P4 -> C6: a command (t5wm_cmd_t, then a frame for TX) */
#define T5WM_MSG_REPLY 0x54350002u    /* C6 -> P4: the result of the last command (t5wm_reply_t) */
#define T5WM_MSG_FRAMES 0x54350003u   /* C6 -> P4: a batch (t5wm_batch_t, then records) */

enum {
    T5WM_OP_HELLO,            /* reply: version and limits */
    T5WM_OP_START,            /* promiscuous on with channel/types/snaplen */
    T5WM_OP_STOP,
    T5WM_OP_CHANNEL,          /* channel (with second: HT40 above/below, 0 none) */
    T5WM_OP_TX,               /* the frame follows the command */
    T5WM_OP_TX_LIMIT,         /* frames per second (tx_limit) */
};

/* Frame types for the filter (bits). */
#define T5WM_TYPE_MGMT 0x01
#define T5WM_TYPE_CTRL 0x02
#define T5WM_TYPE_DATA 0x04
#define T5WM_TYPE_MISC 0x08

typedef struct __attribute__((packed)) {
    uint8_t op;
    uint8_t channel;          /* 0: leave it */
    uint8_t second;
    uint8_t types;            /* T5WM_TYPE_* */
    uint16_t snaplen;
    uint16_t tx_limit;        /* frames per second */
    uint8_t tx_ifx;           /* 0 station, 1 AP */
    uint8_t tx_sys_seq;       /* let the radio set the sequence number */
    uint16_t seq;             /* echoed in the reply */
} t5wm_cmd_t;

typedef struct __attribute__((packed)) {
    uint16_t seq;
    int32_t err;              /* esp_err_t; ESP_ERR_INVALID_STATE etc. */
    uint8_t version;          /* HELLO */
    uint8_t channel;          /* now */
    uint16_t max_batch;       /* bytes */
} t5wm_reply_t;

typedef struct __attribute__((packed)) {
    uint32_t seq;             /* batches since start: a gap is a lost batch */
    uint32_t dropped;         /* frames dropped here since start (ring full) */
    uint32_t refused;         /* raw frames refused since start (rate limit, or the radio said no) */
    uint16_t count;           /* records that follow */
} t5wm_batch_t;

typedef struct __attribute__((packed)) {
    uint16_t len;             /* bytes of frame data that follow this header */
    uint16_t orig_len;        /* the frame's length on the air */
    uint32_t time_us;         /* the radio's receive timestamp */
    int8_t rssi;
    int8_t noise;
    uint8_t channel;
    uint8_t second;
    uint8_t rate;             /* legacy rate code or MCS */
    uint8_t sig_mode;         /* 0 legacy, 1 HT, 3 VHT, 4 HE */
    uint8_t type;             /* T5WM_TYPE_* */
    uint8_t flags;            /* bit 0: FCS failed */
} t5wm_rec_t;

#define T5WM_BATCH_MAX 1400   /* bytes of one batch message */
