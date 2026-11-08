/* SPDX-License-Identifier: Apache-2.0
 * MQTT client (t5_net/mqtt.h) on esp-mqtt.
 *
 * esp-mqtt runs its own task (stack and buffers in PSRAM, see
 * sdkconfig.defaults) and calls on_event there. on_event never runs app
 * code: it copies each event into a queue (joining a long message's
 * fragments first) that t5_mqtt_next drains on the app's own task. The
 * queue is bounded (QUEUE_MAX events, QUEUE_BYTES of payload); when an app
 * falls behind, the oldest messages are dropped and counted.
 */
#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"
#include "netio_priv.h"
#include "t5_net/mqtt.h"
#include "t5_core/log.h"
#include "t5_core/os.h"

static const char *TAG = "net.mqtt";

#define QUEUE_MAX 64
#define QUEUE_BYTES (512 * 1024)
#define DEFAULT_MAX_MSG (64 * 1024)
#define LIMIT_MAX_MSG (1024 * 1024)

typedef struct ev_item {
    struct ev_item *next;
    t5_mqtt_ev_type_t type;
    int msg_id;
    uint8_t qos;
    bool retain, dup;
    size_t topic_len, data_len, filled;
    uint8_t buf[];              /* topic, NUL, data */
} ev_item_t;

struct t5_mqtt {
    t5_nethdr_t hdr;
    esp_mqtt_client_handle_t c;
    SemaphoreHandle_t mu;       /* the queue */
    ev_item_t *head, *tail;
    int count;
    size_t bytes, max_msg;
    ev_item_t *partial;         /* a message whose fragments are still arriving */
    uint32_t dropped;
    volatile bool connected;
    volatile int refused;       /* connect return code from a refusal, 0 = none */
    volatile bool failed_once;
};

/* ---- the queue (esp-mqtt's task puts, the app's task takes) ------------------------- */
static void push(t5_mqtt_t *m, ev_item_t *it)
{
    xSemaphoreTake(m->mu, portMAX_DELAY);
    /* Make room: drop the oldest messages (keep state changes). */
    for (ev_item_t **pp = &m->head; *pp && (m->count >= QUEUE_MAX || m->bytes + it->data_len > QUEUE_BYTES);) {
        ev_item_t *o = *pp;
        if (o->type != T5_MQTT_EV_MESSAGE) {
            pp = &o->next;
            continue;
        }
        *pp = o->next;
        if (m->tail == o) {
            m->tail = NULL;
            for (ev_item_t *t = m->head; t; t = t->next) m->tail = t;
        }
        m->count--;
        m->bytes -= o->data_len;
        m->dropped++;
        free(o);
    }
    if (m->count >= QUEUE_MAX) {
        m->dropped++;
        free(it);
    } else {
        it->next = NULL;
        if (m->tail) m->tail->next = it;
        else m->head = it;
        m->tail = it;
        m->count++;
        m->bytes += it->data_len;
    }
    xSemaphoreGive(m->mu);
}

static ev_item_t *item_new(t5_mqtt_ev_type_t type, int msg_id, size_t topic_len, size_t data_len)
{
    ev_item_t *it = heap_caps_malloc(sizeof *it + topic_len + 1 + data_len, MALLOC_CAP_SPIRAM);
    if (!it) return NULL;
    memset(it, 0, sizeof *it);
    it->type = type;
    it->msg_id = msg_id;
    it->topic_len = topic_len;
    it->data_len = data_len;
    it->buf[topic_len] = 0;
    return it;
}

static void push_simple(t5_mqtt_t *m, t5_mqtt_ev_type_t type, int msg_id, int qos)
{
    ev_item_t *it = item_new(type, msg_id, 0, 0);
    if (!it) return;
    it->qos = (uint8_t)qos;
    push(m, it);
}

/* A DATA event: a whole message, or one fragment of a long one. */
static void on_data(t5_mqtt_t *m, esp_mqtt_event_handle_t e)
{
    size_t total = e->total_data_len > 0 ? (size_t)e->total_data_len : (size_t)(e->data_len > 0 ? e->data_len : 0);
    if (e->current_data_offset == 0) {
        free(m->partial);
        m->partial = NULL;
        if (total > m->max_msg) {
            T5_LOGW(TAG, "%u-byte message dropped (max_msg %u)", (unsigned)total, (unsigned)m->max_msg);
            m->dropped++;
            return;
        }
        size_t tl = e->topic_len > 0 ? (size_t)e->topic_len : 0;
        m->partial = item_new(T5_MQTT_EV_MESSAGE, e->msg_id, tl, total);
        if (!m->partial) {
            m->dropped++;
            return;
        }
        if (tl) memcpy(m->partial->buf, e->topic, tl);
        m->partial->qos = (uint8_t)e->qos;
        m->partial->retain = e->retain;
        m->partial->dup = e->dup;
    }
    ev_item_t *it = m->partial;
    if (!it) return;                                   /* a fragment of a dropped message */
    size_t off = (size_t)e->current_data_offset, n = e->data_len > 0 ? (size_t)e->data_len : 0;
    if (off + n > it->data_len) n = off < it->data_len ? it->data_len - off : 0;
    memcpy(it->buf + it->topic_len + 1 + off, e->data, n);
    it->filled = off + n;
    if (it->filled >= it->data_len) {
        m->partial = NULL;
        push(m, it);
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)base;
    t5_mqtt_t *m = arg;
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        m->connected = true;
        m->refused = 0;
        push_simple(m, T5_MQTT_EV_CONNECTED, 0, 0);
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (m->connected) push_simple(m, T5_MQTT_EV_DISCONNECTED, 0, 0);
        m->connected = false;
        break;
    case MQTT_EVENT_SUBSCRIBED: {
        int granted = e->data_len > 0 ? (uint8_t)e->data[0] : e->qos;
        push_simple(m, T5_MQTT_EV_SUBSCRIBED, e->msg_id, granted);
        break;
    }
    case MQTT_EVENT_UNSUBSCRIBED: push_simple(m, T5_MQTT_EV_UNSUBSCRIBED, e->msg_id, 0); break;
    case MQTT_EVENT_PUBLISHED: push_simple(m, T5_MQTT_EV_PUBLISHED, e->msg_id, 0); break;
    case MQTT_EVENT_DATA: on_data(m, e); break;
    case MQTT_EVENT_ERROR:
        m->failed_once = true;
        if (e->error_handle && e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            m->refused = e->error_handle->connect_return_code ? (int)e->error_handle->connect_return_code : 1;
        }
        push_simple(m, T5_MQTT_EV_ERROR, 0, 0);
        break;
    default:
        break;
    }
}

/* ---- open / close --------------------------------------------------------------------- */
static void mqtt_free(t5_mqtt_t *m)
{
    if (m->c) {
        esp_mqtt_client_stop(m->c);
        esp_mqtt_client_destroy(m->c);         /* its task is gone after this: no more pushes */
    }
    for (ev_item_t *it = m->head; it;) {
        ev_item_t *n = it->next;
        free(it);
        it = n;
    }
    free(m->partial);
    if (m->mu) vSemaphoreDelete(m->mu);
    free(m);
}

static void release_mqtt(void *h)
{
    T5_IGNORE(t5_mqtt_close(h));       /* app exit: nothing to report to */
}

static void fill_config(esp_mqtt_client_config_t *c, const t5_mqtt_cfg_t *cfg)
{
    c->broker.address.uri = cfg->uri;
    if (cfg->flags & T5_MQTT_TLS_NO_VERIFY) {
        c->broker.verification.crt_bundle_attach = net_attach_no_verify;
        c->broker.verification.skip_cert_common_name_check = true;
    } else if (cfg->ca_pem) {
        c->broker.verification.certificate = cfg->ca_pem;
    } else {
        c->broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }
    c->credentials.client_id = cfg->client_id;
    c->credentials.username = cfg->username;
    c->credentials.authentication.password = cfg->password;
    c->credentials.authentication.certificate = cfg->cert_pem;
    c->credentials.authentication.key = cfg->key_pem;
    c->session.keepalive = cfg->keepalive_s ? cfg->keepalive_s : 60;
    c->session.disable_clean_session = cfg->flags & T5_MQTT_PERSISTENT;
    c->session.protocol_ver = (cfg->flags & T5_MQTT_V5) ? MQTT_PROTOCOL_V_5 : MQTT_PROTOCOL_V_3_1_1;
    if (cfg->will_topic) {
        c->session.last_will.topic = cfg->will_topic;
        c->session.last_will.msg = cfg->will_msg;
        c->session.last_will.msg_len = (int)cfg->will_len;
        c->session.last_will.qos = cfg->will_qos;
        c->session.last_will.retain = cfg->will_retain;
    }
    c->network.timeout_ms = 10000;
    c->network.reconnect_timeout_ms = 5000;
    c->buffer.size = 4096;                 /* bigger messages arrive in fragments */
}

static t5_err_t wait_connected(t5_mqtt_t *m, uint32_t timeout)
{
    uint32_t dl = net_deadline(timeout);
    for (;;) {
        if (m->connected) return T5_OK;
        if (m->refused) return T5_ERR_DENIED;
        if (m->hdr.closing || net_cancelled()) return T5_ERR_STATE;
        if (!net_left(dl, timeout)) return m->failed_once ? T5_ERR_IO : T5_ERR_TIMEOUT;
        t5_delay_ms(20);
    }
}

t5_err_t t5_mqtt_connect(const t5_mqtt_cfg_t *cfg, t5_mqtt_t **out)
{
    if (!out) return T5_ERR_INVALID_ARG;
    *out = NULL;
    if (!cfg || !cfg->uri || cfg->will_qos > 2 || (cfg->will_len && !cfg->will_msg)) return T5_ERR_INVALID_ARG;
    t5_mqtt_t *m = calloc(1, sizeof *m);
    if (!m) return T5_ERR_NO_MEM;
    m->max_msg = cfg->max_msg ? (cfg->max_msg < LIMIT_MAX_MSG ? cfg->max_msg : LIMIT_MAX_MSG) : DEFAULT_MAX_MSG;
    m->mu = xSemaphoreCreateMutex();
    snprintf(m->hdr.what, sizeof m->hdr.what, "mqtt %.34s", cfg->uri);
    esp_mqtt_client_config_t c = { 0 };
    fill_config(&c, cfg);
    /* The client's task must not carry the app context: it isn't the app's. */
    void *saved = t5_app_ctx_get();
    t5_app_ctx_set(NULL);
    m->c = m->mu ? esp_mqtt_client_init(&c) : NULL;
    if (m->c) esp_mqtt_client_register_event(m->c, MQTT_EVENT_ANY, on_event, m);
    t5_app_ctx_set(saved);
    if (!m->c) {
        mqtt_free(m);
        return T5_ERR_INVALID_ARG;         /* most likely a bad URI */
    }
    t5_err_t e = nh_register(&m->hdr, NH_MQTT, release_mqtt);
    if (e != T5_OK) {
        mqtt_free(m);
        return e;
    }
    if (!nh_acquire(m, NH_MQTT)) return T5_ERR_STATE;
    t5_app_ctx_set(NULL);
    e = esp_mqtt_client_start(m->c) == ESP_OK ? T5_OK : T5_ERR_NO_MEM;
    t5_app_ctx_set(saved);
    if (e == T5_OK) e = wait_connected(m, cfg->timeout_ms ? cfg->timeout_ms : NET_DEFAULT_TIMEOUT_MS);
    nh_release(m);
    if (e != T5_OK) {
        if (nh_unregister(m, NH_MQTT) == T5_OK) mqtt_free(m);
        return e;
    }
    *out = m;
    return T5_OK;
}

t5_err_t t5_mqtt_close(t5_mqtt_t *m)
{
    T5_RETURN_ON_ERR(nh_unregister(m, NH_MQTT));
    if (m->connected) esp_mqtt_client_disconnect(m->c);
    mqtt_free(m);
    return T5_OK;
}

/* ---- use ------------------------------------------------------------------------------ */
bool t5_mqtt_connected(t5_mqtt_t *m)
{
    if (!nh_acquire(m, NH_MQTT)) return false;
    bool c = m->connected;
    nh_release(m);
    return c;
}

int t5_mqtt_publish(t5_mqtt_t *m, const char *topic, const void *data, size_t len, int qos, bool retain)
{
    if (!topic || !*topic || (len && !data) || qos < 0 || qos > 2 || len > LIMIT_MAX_MSG) return T5_ERR_INVALID_ARG;
    if (!nh_acquire(m, NH_MQTT)) return T5_ERR_INVALID_ARG;
    int r;
    if (!m->connected && qos == 0) {
        r = T5_ERR_STATE;
    } else {
        /* QoS 1/2 while disconnected: queued in the outbox, sent on reconnect. */
        r = m->connected ? esp_mqtt_client_publish(m->c, topic, data, (int)len, qos, retain)
                         : esp_mqtt_client_enqueue(m->c, topic, data, (int)len, qos, retain, true);
        if (r < 0) r = T5_ERR_IO;
    }
    nh_release(m);
    return r;
}

int t5_mqtt_subscribe(t5_mqtt_t *m, const char *topic, int qos)
{
    if (!topic || !*topic || qos < 0 || qos > 2) return T5_ERR_INVALID_ARG;
    if (!nh_acquire(m, NH_MQTT)) return T5_ERR_INVALID_ARG;
    int r = m->connected ? esp_mqtt_client_subscribe_single(m->c, topic, qos) : T5_ERR_STATE;
    if (r == -1) r = T5_ERR_IO;
    nh_release(m);
    return r;
}

int t5_mqtt_unsubscribe(t5_mqtt_t *m, const char *topic)
{
    if (!topic || !*topic) return T5_ERR_INVALID_ARG;
    if (!nh_acquire(m, NH_MQTT)) return T5_ERR_INVALID_ARG;
    int r = m->connected ? esp_mqtt_client_unsubscribe(m->c, topic) : T5_ERR_STATE;
    if (r == -1) r = T5_ERR_IO;
    nh_release(m);
    return r;
}

static size_t need(const ev_item_t *it)
{
    return it->topic_len + 1 + it->data_len;
}

t5_err_t t5_mqtt_next(t5_mqtt_t *m, t5_mqtt_event_t *ev, void *buf, size_t cap, uint32_t timeout_ms)
{
    if (!ev || (!buf && cap)) return T5_ERR_INVALID_ARG;
    if (!nh_acquire(m, NH_MQTT)) return T5_ERR_INVALID_ARG;
    uint32_t dl = net_deadline(timeout_ms);
    t5_err_t e = T5_ERR_TIMEOUT;
    for (;;) {
        xSemaphoreTake(m->mu, portMAX_DELAY);
        ev_item_t *it = m->head;
        if (it && need(it) > cap) {
            e = T5_ERR_NO_MEM;
        } else if (it) {
            m->head = it->next;
            if (!m->head) m->tail = NULL;
            m->count--;
            m->bytes -= it->data_len;
        }
        xSemaphoreGive(m->mu);
        if (e == T5_ERR_NO_MEM) break;
        if (it) {
            uint8_t *b = buf;
            memcpy(b, it->buf, need(it));
            *ev = (t5_mqtt_event_t){ .type = it->type, .msg_id = it->msg_id, .topic = (const char *)b,
                                     .data = b + it->topic_len + 1, .len = it->data_len, .qos = it->qos,
                                     .retain = it->retain, .dup = it->dup };
            free(it);
            e = T5_OK;
            break;
        }
        if (m->hdr.closing || net_cancelled()) {
            e = T5_ERR_STATE;
            break;
        }
        uint32_t left = net_left(dl, timeout_ms);
        if (!left) break;
        t5_delay_ms(left < 20 ? left : 20);
    }
    nh_release(m);
    return e;
}

int t5_mqtt_next_size(t5_mqtt_t *m)
{
    if (!nh_acquire(m, NH_MQTT)) return T5_ERR_INVALID_ARG;
    xSemaphoreTake(m->mu, portMAX_DELAY);
    int n = m->head ? (int)need(m->head) : -1;
    xSemaphoreGive(m->mu);
    nh_release(m);
    return n;
}

uint32_t t5_mqtt_dropped(t5_mqtt_t *m)
{
    if (!nh_acquire(m, NH_MQTT)) return 0;
    uint32_t d = m->dropped;
    nh_release(m);
    return d;
}

bool mqtt_poll_buffered(void *h)
{
    t5_mqtt_t *m = h;
    return m->head != NULL;
}
