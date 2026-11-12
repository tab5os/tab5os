/* SPDX-License-Identifier: Apache-2.0
 * Wi-Fi monitor mode and raw frames (t5_net/wifimon.h), run on the C6 by
 * Tab5 OS's radio firmware (c6/main/monitor.c) and spoken to over
 * esp_hosted peer data (t5_wifimon_proto.h).
 *
 * Commands are sent and their replies waited for one at a time. Frame
 * batches arrive on esp_hosted's RPC event task; each record goes into a
 * PSRAM ring the app's t5_wifimon_next() drains.
 */
#include <stdlib.h>
#include <string.h>
#include "eh_host_feat_peer_data.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "t5_core/excl_sys.h"
#include "t5_core/io_hold.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_core/res.h"
#include "t5_net/net.h"
#include "t5_net/wifimon.h"
#include "t5_power/power.h"
#include "t5_wifimon_proto.h"
#include "net_priv.h"

static const char *TAG = "t5.wifimon";

#define MAGIC 0x57464d4eu      /* "WFMN" */
#define RING_BYTES (256 * 1024)
#define REPLY_MS 1000
#define TX_MAX 1500

struct t5_wifimon {
    uint32_t magic;
    bool exclusive, held;
    volatile bool stopped;
    RingbufHandle_t ring;
    t5_wakelock_t *lock;
    uint32_t last_seq;
    bool have_seq;
    t5_wifimon_stats_t st;
};

static t5_mutex_t s_mx, s_cmd_mx;
static t5_wifimon_t *s_mon;
static SemaphoreHandle_t s_reply_sem;
static t5wm_reply_t s_reply;
static uint16_t s_seq;
static bool s_registered;

static t5_mutex_t mx(void)
{
    return t5_mutex_once(&s_mx);
}

/* ---- from the C6 (esp_hosted's event task) ---------------------------------- */
static volatile uint32_t s_replies, s_batches;

static char s_build[T5WM_BUILD_LEN];     /* from the last HELLO reply that had it */

static void on_reply(uint32_t id, const uint8_t *data, size_t len, void *ctx)
{
    (void)id; (void)ctx;
    s_replies++;
    if (len < sizeof s_reply) return;
    if (len >= sizeof s_reply + sizeof s_build) {
        memcpy(s_build, data + sizeof s_reply, sizeof s_build);
        s_build[sizeof s_build - 1] = 0;
    }
    memcpy(&s_reply, data, sizeof s_reply);
    xSemaphoreGive(s_reply_sem);
}

static void on_frames(uint32_t id, const uint8_t *data, size_t len, void *ctx)
{
    (void)id; (void)ctx;
    s_batches++;
    t5wm_batch_t b;
    if (len < sizeof b) return;
    memcpy(&b, data, sizeof b);
    t5_mutex_lock(mx());
    t5_wifimon_t *m = s_mon;
    if (!m || m->stopped) {
        t5_mutex_unlock(mx());
        return;
    }
    if (m->have_seq && b.seq > m->last_seq + 1) m->st.dropped_link += b.seq - m->last_seq - 1;
    m->last_seq = b.seq;
    m->have_seq = true;
    m->st.dropped_radio = b.dropped;
    m->st.refused = b.refused;
    size_t off = sizeof b;
    for (uint16_t i = 0; i < b.count && off + sizeof(t5wm_rec_t) <= len; i++) {
        t5wm_rec_t r;
        memcpy(&r, data + off, sizeof r);
        size_t n = sizeof r + r.len;
        if (off + n > len) break;
        if (xRingbufferSend(m->ring, data + off, n, 0) != pdTRUE) m->st.dropped_queue++;
        off += n;
    }
    t5_mutex_unlock(mx());
}

/* ---- commands --------------------------------------------------------------- */
static t5_err_t esp_to_t5(int32_t e)
{
    switch (e) {
    case 0: return T5_OK;                         /* ESP_OK */
    case 0x102: return T5_ERR_DENIED;             /* ESP_ERR_INVALID_ARG: a frame ESP-IDF refuses */
    case 0x103: return T5_ERR_BUSY;               /* ESP_ERR_INVALID_STATE: the limit, or not now */
    case 0x106: return T5_ERR_NOT_SUPPORTED;      /* ESP_ERR_NOT_SUPPORTED */
    default: return T5_ERR_IO;
    }
}

/* One command and its reply. *reply gets the reply when non-NULL. */
static t5_err_t cmd(t5wm_cmd_t *c, const void *extra, size_t extra_len, t5wm_reply_t *reply)
{
    t5_mutex_lock(t5_mutex_once(&s_cmd_mx));
    c->seq = ++s_seq;
    uint8_t *buf = malloc(sizeof *c + extra_len);
    t5_err_t e = T5_ERR_NO_MEM;
    if (buf) {
        memcpy(buf, c, sizeof *c);
        if (extra_len) memcpy(buf + sizeof *c, extra, extra_len);
        xSemaphoreTake(s_reply_sem, 0);
        esp_err_t se = eh_host_peer_data_send(T5WM_MSG_CMD, buf, sizeof *c + extra_len);
        if (se != ESP_OK) T5_LOGW(TAG, "command %u: peer data send failed: 0x%x", c->op, (unsigned)se);
        e = se == ESP_OK ? T5_OK : T5_ERR_IO;
        free(buf);
        int64_t until = t5_time_us() + REPLY_MS * 1000;
        while (e == T5_OK) {
            int64_t left = until - t5_time_us();
            if (left <= 0 || xSemaphoreTake(s_reply_sem, pdMS_TO_TICKS(left / 1000 + 1)) != pdTRUE) {
                e = T5_ERR_TIMEOUT;
                break;
            }
            if (s_reply.seq != c->seq) continue;   /* a late reply to an older command */
            if (reply) *reply = s_reply;
            e = esp_to_t5(s_reply.err);
            break;
        }
    }
    t5_mutex_unlock(t5_mutex_once(&s_cmd_mx));
    return e;
}

static bool ensure_registered(void)
{
    t5_mutex_lock(mx());
    if (!s_registered) {
        if (!s_reply_sem) s_reply_sem = xSemaphoreCreateBinary();
        s_registered = s_reply_sem && eh_host_feat_peer_data_init() == ESP_OK &&
                       eh_host_peer_data_register(T5WM_MSG_REPLY, on_reply, NULL) == ESP_OK &&
                       eh_host_peer_data_register(T5WM_MSG_FRAMES, on_frames, NULL) == ESP_OK;
    }
    bool ok = s_registered;
    t5_mutex_unlock(mx());
    return ok;
}

t5_err_t wifimon_c6_build(char *out, size_t n)
{
    if (n) out[0] = 0;
    if (!ensure_registered()) return T5_ERR_IO;
    t5_err_t e = T5_ERR_TIMEOUT;
    /* Right after the C6 starts, its handler may not be in yet: a few tries. */
    for (int i = 0; i < 3 && e != T5_OK; i++) {
        s_build[0] = 0;
        t5wm_cmd_t hello = { .op = T5WM_OP_HELLO };
        e = cmd(&hello, NULL, 0, NULL);
    }
    if (e == T5_OK) snprintf(out, n, "%s", s_build);
    return e;
}

/* ---- channels and the station ----------------------------------------------- */
static bool connected(uint8_t *channel)
{
    t5_net_status_t ns;
    t5_net_status(&ns);
    if (channel) *channel = ns.channel;
    return ns.state == T5_NET_CONNECTED || ns.state == T5_NET_CONNECTING;
}

/* Before a channel change: the station has to stand aside, if it's up. */
static t5_err_t make_room(t5_wifimon_t *m, uint8_t channel)
{
    uint8_t cur = 0;
    if (t5_excl_claimed(T5_EXCL_WIFI)) return T5_OK;   /* the app holds the radio: the station is held already */
    if (!channel || !connected(&cur) || m->held) return T5_OK;
    if (!m->exclusive) return channel == cur ? T5_OK : T5_ERR_STATE;
    T5_RETURN_ON_ERR(net_station_hold(true));
    m->held = true;
    for (int i = 0; i < 40 && connected(NULL); i++) t5_delay_ms(50);
    return T5_OK;
}

t5_err_t t5_wifimon_set_channel(t5_wifimon_t *m, uint8_t channel)
{
    if (!m || m->magic != MAGIC || channel < 1 || channel > 13) return T5_ERR_INVALID_ARG;
    if (m->stopped) return T5_ERR_STATE;
    T5_RETURN_ON_ERR(make_room(m, channel));
    t5wm_cmd_t c = { .op = T5WM_OP_CHANNEL, .channel = channel };
    t5wm_reply_t r;
    t5_err_t e = cmd(&c, NULL, 0, &r);
    if (e == T5_OK) m->st.channel = r.channel;
    return e;
}

/* ---- open and close --------------------------------------------------------- */
/* The monitor ends (close, or the person's Stop): radio, station, locks. */
static void stop_locked_out(t5_wifimon_t *m)
{
    if (m->stopped) return;
    m->stopped = true;
    t5wm_cmd_t c = { .op = T5WM_OP_STOP };
    T5_IGNORE(cmd(&c, NULL, 0, NULL));
    if (m->held) T5_IGNORE(net_station_hold(false));
    m->held = false;
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);             /* the net task's normal setting */
    if (m->lock) t5_wakelock_release(m->lock);
    m->lock = NULL;
    t5_io_hold_set(T5_IO_WIFI_MONITOR, NULL, NULL);
    T5_LOGI(TAG, "monitor stopped: %u frames, dropped %u radio / %u link / %u queue", (unsigned)m->st.frames,
            (unsigned)m->st.dropped_radio, (unsigned)m->st.dropped_link, (unsigned)m->st.dropped_queue);
}

static void stop_for_person(void)
{
    t5_mutex_lock(mx());
    t5_wifimon_t *m = s_mon;
    t5_mutex_unlock(mx());
    if (m) stop_locked_out(m);
}

void wifimon_end_for_excl(void)
{
    stop_for_person();
}

static void release_handle(void *h)
{
    T5_IGNORE(t5_wifimon_close(h));
}

t5_err_t t5_wifimon_open(const t5_wifimon_cfg_t *cfg, t5_wifimon_t **out)
{
    if (!cfg || !out || cfg->size < sizeof *cfg || cfg->channel > 13 || cfg->types > 0x0f) return T5_ERR_INVALID_ARG;
    if (cfg->snaplen && (cfg->snaplen < 24 || cfg->snaplen > 1350)) return T5_ERR_INVALID_ARG;
    if (cfg->tx_per_s > 500) return T5_ERR_INVALID_ARG;
    *out = NULL;
    int pe = t5_core_perm_check(T5_CORE_CAP_WIFI_MONITOR);
    if (pe != T5_OK) return (t5_err_t)pe;
    if (!net_wifi_up()) return T5_ERR_STATE;
    if (!ensure_registered()) return T5_ERR_NOT_SUPPORTED;

    t5_wifimon_t *m = heap_caps_calloc(1, sizeof *m, MALLOC_CAP_SPIRAM);
    if (!m) return T5_ERR_NO_MEM;
    m->magic = MAGIC;
    m->exclusive = cfg->exclusive;
    m->ring = xRingbufferCreateWithCaps(RING_BYTES, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM);
    if (!m->ring) {
        free(m);
        return T5_ERR_NO_MEM;
    }
    t5_mutex_lock(mx());
    t5_err_t e = s_mon ? T5_ERR_BUSY : T5_OK;
    if (e == T5_OK) s_mon = m;
    t5_mutex_unlock(mx());

    /* Is this radio firmware new enough? */
    t5wm_cmd_t hello = { .op = T5WM_OP_HELLO };
    t5wm_reply_t r;
    if (e == T5_OK && cmd(&hello, NULL, 0, &r) != T5_OK) {
        T5_LOGW(TAG, "the C6 firmware doesn't answer (replies %u, batches %u): update it (net update)",
                (unsigned)s_replies, (unsigned)s_batches);
        e = T5_ERR_NOT_SUPPORTED;
    }
    if (e == T5_OK) e = make_room(m, cfg->channel);
    if (e == T5_OK) {
        m->lock = t5_wakelock_acquire_system(T5_LOCK_NET, "wifimon");
        esp_err_t pe = esp_wifi_set_ps(WIFI_PS_NONE);   /* power save sleeps through most frames */
        wifi_ps_type_t ps = WIFI_PS_MAX_MODEM;
        esp_wifi_get_ps(&ps);
        T5_LOGD(TAG, "power save off: set %s, now %d", esp_err_to_name(pe), (int)ps);
        t5wm_cmd_t lim = { .op = T5WM_OP_TX_LIMIT, .tx_limit = cfg->tx_per_s ? cfg->tx_per_s : 50 };
        T5_IGNORE(cmd(&lim, NULL, 0, NULL));
        t5wm_cmd_t c = { .op = T5WM_OP_START, .channel = cfg->channel, .types = cfg->types ? cfg->types : T5WM_TYPE_MGMT,
                         .snaplen = cfg->snaplen ? cfg->snaplen : 256 };
        e = cmd(&c, NULL, 0, &r);
        if (e == T5_OK) m->st.channel = r.channel;
    }
    if (e != T5_OK) {
        t5_mutex_lock(mx());
        if (s_mon == m) {
            stop_locked_out(m);
            s_mon = NULL;
        }
        t5_mutex_unlock(mx());
        vRingbufferDeleteWithCaps(m->ring);
        free(m);
        return e;
    }
    const t5_app_ctx_hdr_t *app = t5_app_ctx_get();
    char who[32] = "";
    if (app && app->magic == T5_APP_CTX_MAGIC) snprintf(who, sizeof who, "%s", app->name && app->name[0] ? app->name : app->id);
    t5_io_hold_set(T5_IO_WIFI_MONITOR, who, stop_for_person);
    t5_res_add(T5_RES_OTHER, m, release_handle);
    T5_LOGI(TAG, "monitor open on channel %u", m->st.channel);
    *out = m;
    return T5_OK;
}

int t5_wifimon_next(t5_wifimon_t *m, t5_wifimon_frame_t *meta, void *buf, size_t cap, uint32_t timeout_ms)
{
    if (!m || m->magic != MAGIC || !meta || (cap && !buf)) return T5_ERR_INVALID_ARG;
    size_t n = 0;
    TickType_t t = timeout_ms == T5_WIFIMON_FOREVER ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (m->stopped) t = 0;
    uint8_t *item = xRingbufferReceive(m->ring, &n, t);
    if (!item) return m->stopped ? T5_ERR_STATE : T5_ERR_TIMEOUT;
    t5wm_rec_t r;
    memcpy(&r, item, sizeof r);
    /* The radio's copy ends with the 4-byte FCS when the whole frame fit. */
    size_t data = r.len;
    bool has_fcs = r.len == r.orig_len && r.len >= 4;
    if (has_fcs) data -= 4;
    *meta = (t5_wifimon_frame_t){
        .time_us = r.time_us, .rssi = r.rssi, .noise_floor = r.noise, .channel = r.channel, .second = r.second,
        .rate = r.rate, .format = r.sig_mode, .fcs_bad = r.flags & 1,
        .type = r.type, .len = (uint16_t)(r.orig_len >= 4 ? r.orig_len - 4 : r.orig_len),
    };
    size_t k = data < cap ? data : cap;
    if (k) memcpy(buf, item + sizeof r, k);
    vRingbufferReturnItem(m->ring, item);
    m->st.frames++;
    return (int)k;
}

void t5_wifimon_stats(t5_wifimon_t *m, t5_wifimon_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!m || m->magic != MAGIC) return;
    t5_mutex_lock(mx());
    *out = m->st;
    t5_mutex_unlock(mx());
}

t5_err_t t5_wifimon_send(t5_wifimon_t *m, const void *frame, size_t len, bool use_sys_seq)
{
    if (!m || m->magic != MAGIC || !frame || len < 24 || len > TX_MAX) return T5_ERR_INVALID_ARG;
    if (m->stopped) return T5_ERR_STATE;
    int pe = t5_core_perm_check(T5_CORE_CAP_WIFI_TX);
    if (pe != T5_OK) return (t5_err_t)pe;
    t5wm_cmd_t c = { .op = T5WM_OP_TX, .tx_ifx = 0, .tx_sys_seq = use_sys_seq };
    t5_err_t e = cmd(&c, frame, len, NULL);
    if (e == T5_OK) m->st.sent++;
    return e;
}

t5_err_t t5_wifimon_close(t5_wifimon_t *m)
{
    if (!m || m->magic != MAGIC) return T5_ERR_INVALID_ARG;
    t5_res_remove(m);
    stop_locked_out(m);
    t5_mutex_lock(mx());
    if (s_mon == m) s_mon = NULL;
    m->magic = 0;
    t5_mutex_unlock(mx());
    vRingbufferDeleteWithCaps(m->ring);
    free(m);
    return T5_OK;
}
