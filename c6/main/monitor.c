/* SPDX-License-Identifier: Apache-2.0
 * Wi-Fi monitor mode and raw frames for the P4 (t5_wifimon_proto.h).
 *
 * The promiscuous callback runs on the Wi-Fi task and must be quick: it
 * copies each frame (cut to the snap length) with its metadata into a ring
 * buffer and returns. A task drains the ring into batches of up to
 * T5WM_BATCH_MAX bytes and sends each to the P4 as one peer data message.
 * When the ring is full, frames are dropped and counted; the count rides in
 * every batch, so the P4 knows exactly what it missed.
 *
 * Raw transmit goes through esp_wifi_80211_tx as ESP-IDF provides it,
 * including the frame types it refuses, with a per-second limit here.
 */
#include <string.h>
#include "eh_cp_feat_peer_data.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "t5_wifimon_proto.h"

static const char *TAG = "t5_mon";

#define RING_BYTES (24 * 1024)
#define SNAP_MAX 2500

static RingbufHandle_t s_ring;
static TaskHandle_t s_task;
static volatile bool s_on;
static uint16_t s_snap = 256;
static uint8_t s_types = T5WM_TYPE_MGMT;
static uint32_t s_seq, s_dropped, s_refused;
static uint16_t s_tx_limit = 50;
static int64_t s_tx_window;
static uint32_t s_tx_count;
static uint8_t s_batch[T5WM_BATCH_MAX];

static uint8_t type_bit(wifi_promiscuous_pkt_type_t t)
{
    switch (t) {
    case WIFI_PKT_MGMT: return T5WM_TYPE_MGMT;
    case WIFI_PKT_CTRL: return T5WM_TYPE_CTRL;
    case WIFI_PKT_DATA: return T5WM_TYPE_DATA;
    default: return T5WM_TYPE_MISC;
    }
}

/* The Wi-Fi task: copy and go. */
static void rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!s_on) return;
    const wifi_promiscuous_pkt_t *p = buf;
    uint16_t orig = (uint16_t)p->rx_ctrl.sig_len;
    uint16_t len = orig < s_snap ? orig : s_snap;
    if (len > T5WM_BATCH_MAX - sizeof(t5wm_batch_t) - sizeof(t5wm_rec_t)) {
        len = T5WM_BATCH_MAX - sizeof(t5wm_batch_t) - sizeof(t5wm_rec_t);
    }
    void *slot = NULL;
    if (xRingbufferSendAcquire(s_ring, &slot, sizeof(t5wm_rec_t) + len, 0) != pdTRUE) {
        s_dropped++;
        return;
    }
    t5wm_rec_t r = {
        .len = len,
        .orig_len = orig,
        .time_us = p->rx_ctrl.timestamp,
        .rssi = (int8_t)p->rx_ctrl.rssi,
        .noise = (int8_t)p->rx_ctrl.noise_floor,
        .channel = (uint8_t)p->rx_ctrl.channel,
        .second = (uint8_t)p->rx_ctrl.second,
        .rate = (uint8_t)p->rx_ctrl.rate,
        .sig_mode = (uint8_t)p->rx_ctrl.cur_bb_format,
        .type = type_bit(type),
        .flags = p->rx_ctrl.rx_state ? 1 : 0,
    };
    memcpy(slot, &r, sizeof r);
    memcpy((uint8_t *)slot + sizeof r, p->payload, len);
    xRingbufferSendComplete(s_ring, slot);
}

static void on_cmd(uint32_t id, const uint8_t *data, size_t len, void *ctx);

/* esp_hosted's peer data init clears its callback table, and it may run
 * again when the P4 connects, after ours went in: register again on every
 * connection (main.c) and once a second, which is cheap. */
void t5_monitor_register(void)
{
    eh_cp_feat_peer_data_init();
    eh_cp_feat_peer_data_register_callback(T5WM_MSG_CMD, on_cmd, NULL);
}

/* Drain the ring into batches. A batch goes out when the next record
 * wouldn't fit, or 20 ms after its first record. */
static void batch_task(void *arg)
{
    (void)arg;
    size_t used = sizeof(t5wm_batch_t);
    uint16_t count = 0;
    int64_t next_reg = 0;
    for (;;) {
        if (!count && esp_timer_get_time() >= next_reg) {
            t5_monitor_register();
            next_reg = esp_timer_get_time() + 1000000;
        }
        size_t n = 0;
        void *item = xRingbufferReceive(s_ring, &n, pdMS_TO_TICKS(count ? 20 : 200));
        bool flush = !item && count;
        if (item && used + n > sizeof s_batch) flush = true;
        if (flush) {
            t5wm_batch_t h = { .seq = s_seq++, .dropped = s_dropped, .refused = s_refused, .count = count };
            memcpy(s_batch, &h, sizeof h);
            if (eh_cp_feat_peer_data_send(T5WM_MSG_FRAMES, s_batch, used) != ESP_OK) s_dropped += count;
            used = sizeof(t5wm_batch_t);
            count = 0;
        }
        if (item) {
            memcpy(s_batch + used, item, n);
            used += n;
            count++;
            vRingbufferReturnItem(s_ring, item);
        }
    }
}

static esp_err_t set_channel(uint8_t ch, uint8_t second)
{
    if (!ch) return ESP_OK;
    wifi_second_chan_t sc = second == 1 ? WIFI_SECOND_CHAN_ABOVE : second == 2 ? WIFI_SECOND_CHAN_BELOW
                                                                               : WIFI_SECOND_CHAN_NONE;
    return esp_wifi_set_channel(ch, sc);
}

static esp_err_t start(const t5wm_cmd_t *c)
{
    s_snap = c->snaplen >= 24 && c->snaplen <= SNAP_MAX ? c->snaplen : 256;
    s_types = c->types ? c->types : T5WM_TYPE_MGMT;
    wifi_promiscuous_filter_t f = { .filter_mask = 0 };
    if (s_types & T5WM_TYPE_MGMT) f.filter_mask |= WIFI_PROMIS_FILTER_MASK_MGMT;
    if (s_types & T5WM_TYPE_CTRL) f.filter_mask |= WIFI_PROMIS_FILTER_MASK_CTRL;
    if (s_types & T5WM_TYPE_DATA) f.filter_mask |= WIFI_PROMIS_FILTER_MASK_DATA;
    if (s_types & T5WM_TYPE_MISC) f.filter_mask |= WIFI_PROMIS_FILTER_MASK_MISC;
    esp_err_t e = esp_wifi_set_promiscuous_filter(&f);
    if (e == ESP_OK && (s_types & T5WM_TYPE_CTRL)) {
        wifi_promiscuous_filter_t cf = { .filter_mask = WIFI_PROMIS_CTRL_FILTER_MASK_ALL };
        e = esp_wifi_set_promiscuous_ctrl_filter(&cf);
    }
    if (e == ESP_OK) e = esp_wifi_set_promiscuous_rx_cb(rx_cb);
    s_seq = s_dropped = s_refused = 0;
    if (e == ESP_OK) {
        s_on = true;
        e = esp_wifi_set_promiscuous(true);
        if (e != ESP_OK) s_on = false;
    }
    if (e == ESP_OK) e = set_channel(c->channel, c->second);
    ESP_LOGI(TAG, "monitor %s: types 0x%x snap %u ch %u", e == ESP_OK ? "on" : "failed", s_types, s_snap, c->channel);
    return e;
}

static esp_err_t stop(void)
{
    s_on = false;
    esp_err_t e = esp_wifi_set_promiscuous(false);
    ESP_LOGI(TAG, "monitor off (dropped %u)", (unsigned)s_dropped);
    return e;
}

static esp_err_t tx(const t5wm_cmd_t *c, const uint8_t *frame, size_t len)
{
    int64_t now = esp_timer_get_time();
    if (now - s_tx_window >= 1000000) {
        s_tx_window = now;
        s_tx_count = 0;
    }
    if (s_tx_count >= s_tx_limit) {
        s_refused++;
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t e = esp_wifi_80211_tx(c->tx_ifx ? WIFI_IF_AP : WIFI_IF_STA, frame, (int)len, c->tx_sys_seq);
    if (e == ESP_OK) s_tx_count++;
    else s_refused++;
    return e;
}

static void on_cmd(uint32_t id, const uint8_t *data, size_t len, void *ctx)
{
    (void)id; (void)ctx;
    t5wm_reply_t r = { .err = ESP_ERR_INVALID_ARG, .version = T5WM_PROTO_VERSION, .max_batch = T5WM_BATCH_MAX };
    if (len >= sizeof(t5wm_cmd_t)) {
        t5wm_cmd_t c;
        memcpy(&c, data, sizeof c);
        r.seq = c.seq;
        switch (c.op) {
        case T5WM_OP_HELLO: r.err = ESP_OK; break;
        case T5WM_OP_START: r.err = start(&c); break;
        case T5WM_OP_STOP: r.err = stop(); break;
        case T5WM_OP_CHANNEL: r.err = set_channel(c.channel, c.second); break;
        case T5WM_OP_TX: r.err = tx(&c, data + sizeof c, len - sizeof c); break;
        case T5WM_OP_TX_LIMIT:
            s_tx_limit = c.tx_limit ? c.tx_limit : 50;
            r.err = ESP_OK;
            break;
        default: r.err = ESP_ERR_NOT_SUPPORTED; break;
        }
    }
    uint8_t primary = 0;
    wifi_second_chan_t sc;
    if (esp_wifi_get_channel(&primary, &sc) == ESP_OK) r.channel = primary;
    eh_cp_feat_peer_data_send(T5WM_MSG_REPLY, (const uint8_t *)&r, sizeof r);
}

void t5_monitor_init(void)
{
    esp_log_level_set("cp_ext_peer_data", ESP_LOG_WARN);   /* "Updated callback" every second otherwise */
    s_ring = xRingbufferCreate(RING_BYTES, RINGBUF_TYPE_NOSPLIT);
    if (!s_ring || xTaskCreate(batch_task, "t5_mon", 3072, NULL, 5, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the monitor");
        return;
    }
    t5_monitor_register();
}
