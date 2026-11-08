/* SPDX-License-Identifier: Apache-2.0
 * MQTT client (3.1.1 and 5) for apps with the net permission, on Espressif's
 * esp-mqtt: mqtt://, mqtts:// (TLS), ws:// and wss:// brokers, QoS 0-2,
 * retained messages, a last will, username/password or client
 * certificates, and automatic reconnects.
 *
 * Everything the broker sends arrives as events, taken one at a time with
 * t5_mqtt_next (no callbacks run in your app from the client's task):
 *
 *   t5_mqtt_cfg_t cfg = { .uri = "mqtts://broker.example.com", .username = "me", .password = "pw" };
 *   t5_mqtt_t *m;
 *   if (t5_mqtt_connect(&cfg, &m) == T5_OK) {
 *       t5_mqtt_subscribe(m, "home/+/temp", 1);
 *       t5_mqtt_publish(m, "home/tab5/online", "1", 1, 1, true);
 *       char buf[1024];
 *       t5_mqtt_event_t ev;
 *       while (t5_mqtt_next(m, &ev, buf, sizeof buf, 5000) == T5_OK) {
 *           if (ev.type == T5_MQTT_EV_MESSAGE) handle(ev.topic, ev.data, ev.len);
 *       }
 *   }
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct t5_mqtt t5_mqtt_t;

#define T5_MQTT_TLS_NO_VERIFY (1u << 0)   /* mqtts/wss without checking the certificate (testing only) */
#define T5_MQTT_V5 (1u << 1)              /* speak MQTT 5 (else 3.1.1) */
#define T5_MQTT_PERSISTENT (1u << 2)      /* keep the session (clean session off) */

typedef struct {
    const char *uri;                /* mqtt://host[:1883], mqtts://host[:8883], ws://host/mqtt, wss://... */
    const char *client_id;          /* NULL: one is made up */
    const char *username;
    const char *password;
    uint16_t keepalive_s;           /* 0 = 60 */
    const char *will_topic;         /* last will, or NULL */
    const void *will_msg;
    size_t will_len;
    uint8_t will_qos;
    bool will_retain;
    uint32_t flags;                 /* T5_MQTT_* */
    const char *ca_pem;             /* trust this CA instead of the bundle */
    const char *cert_pem;           /* client certificate (mutual TLS) */
    const char *key_pem;
    size_t max_msg;                 /* largest incoming message; 0 = 64 KB, at most 1 MB */
    uint32_t timeout_ms;            /* wait for the first connection; 0 = 10 s */
} t5_mqtt_cfg_t;

typedef enum {
    T5_MQTT_EV_MESSAGE = 1,         /* topic, data, len, qos, retain, dup */
    T5_MQTT_EV_CONNECTED,
    T5_MQTT_EV_DISCONNECTED,        /* the client reconnects by itself */
    T5_MQTT_EV_SUBSCRIBED,          /* msg_id; qos is the granted QoS (0x80 = refused) */
    T5_MQTT_EV_UNSUBSCRIBED,        /* msg_id */
    T5_MQTT_EV_PUBLISHED,           /* msg_id: a QoS 1/2 publish was acknowledged */
    T5_MQTT_EV_ERROR,
} t5_mqtt_ev_type_t;

typedef struct {
    t5_mqtt_ev_type_t type;
    int msg_id;
    const char *topic;              /* NUL-terminated, in the caller's buffer */
    const uint8_t *data;            /* in the caller's buffer */
    size_t len;
    uint8_t qos;
    bool retain;
    bool dup;
} t5_mqtt_event_t;

/* Start the client and wait (timeout_ms) for the broker to accept it.
 * T5_ERR_DENIED if it refused (bad credentials), T5_ERR_TIMEOUT if it
 * never answered. */
T5_MUST_CHECK t5_err_t t5_mqtt_connect(const t5_mqtt_cfg_t *cfg, t5_mqtt_t **out);
bool t5_mqtt_connected(t5_mqtt_t *m);
/* Returns the message id (0 for QoS 0), or a negative t5_err_t. */
int t5_mqtt_publish(t5_mqtt_t *m, const char *topic, const void *data, size_t len, int qos, bool retain);
int t5_mqtt_subscribe(t5_mqtt_t *m, const char *topic, int qos);
int t5_mqtt_unsubscribe(t5_mqtt_t *m, const char *topic);
/* The next event, with its topic and data copied into buf. T5_ERR_NO_MEM
 * if buf is too small (the event stays queued; t5_mqtt_next_size says how
 * much it needs), T5_ERR_TIMEOUT if nothing came. */
T5_MUST_CHECK t5_err_t t5_mqtt_next(t5_mqtt_t *m, t5_mqtt_event_t *ev, void *buf, size_t cap, uint32_t timeout_ms);
/* Buffer size the next event needs, or -1 if none is queued. */
int t5_mqtt_next_size(t5_mqtt_t *m);
/* Messages dropped because the app didn't take them fast enough. */
uint32_t t5_mqtt_dropped(t5_mqtt_t *m);
/* Disconnect cleanly and free. */
T5_MUST_CHECK t5_err_t t5_mqtt_close(t5_mqtt_t *m);

#ifdef __cplusplus
}
#endif
