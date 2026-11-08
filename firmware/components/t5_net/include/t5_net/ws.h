/* SPDX-License-Identifier: Apache-2.0
 * WebSocket client (ws:// and wss://, RFC 6455) for apps with the net
 * permission.
 *
 * Messages arrive whole (fragments are joined, up to max_msg bytes). Pings
 * are answered by the library; a close from the server is answered and then
 * reported once as a T5_WS_CLOSE message whose payload is the status code
 * (2 bytes, big-endian) and reason. Sending and receiving may happen on
 * different tasks at the same time.
 *
 *   t5_ws_t *ws;
 *   if (t5_ws_connect("wss://echo.example.com/ws", NULL, &ws) == T5_OK) {
 *       t5_ws_send(ws, T5_WS_TEXT, "hi", 2, 5000);
 *       char buf[256];
 *       t5_ws_op_t op;
 *       int n = t5_ws_recv(ws, buf, sizeof buf, &op, 5000);
 *       t5_ws_close(ws, 1000);
 *   }
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"
#include "t5_net/http.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct t5_ws t5_ws_t;

typedef enum {
    T5_WS_TEXT = 1,
    T5_WS_BINARY = 2,
    T5_WS_CLOSE = 8,
    T5_WS_PING = 9,
    T5_WS_PONG = 10,
} t5_ws_op_t;

typedef struct {
    const t5_http_header_t *headers;  /* extra handshake headers (Authorization, Origin...) */
    size_t n_headers;
    const char *subprotocols;       /* Sec-WebSocket-Protocol, "chat, superchat"; NULL for none */
    uint32_t timeout_ms;            /* connect + handshake; 0 = 10 s */
    uint32_t flags;                 /* T5_HTTP_TLS_NO_VERIFY */
    const char *ca_pem;
    size_t max_msg;                 /* largest message accepted; 0 = 64 KB, at most 4 MB */
    const char *cert_pem;           /* client certificate for mutual TLS, or NULL */
    const char *key_pem;            /* its private key */
} t5_ws_cfg_t;

T5_MUST_CHECK t5_err_t t5_ws_connect(const char *url, const t5_ws_cfg_t *cfg, t5_ws_t **out);
/* The subprotocol the server picked, or "". */
const char *t5_ws_subprotocol(const t5_ws_t *ws);
/* Send one message (TEXT or BINARY), or a PING/PONG (at most 125 bytes).
 * Returns n. */
int t5_ws_send(t5_ws_t *ws, t5_ws_op_t op, const void *data, size_t n, uint32_t timeout_ms);
/* Receive one message into buf. Returns its length and sets *op; a message
 * longer than cap gives T5_ERR_NO_MEM and stays queued (t5_ws_next_len
 * says how big it is). T5_ERR_STATE once the connection is closed. */
int t5_ws_recv(t5_ws_t *ws, void *buf, size_t cap, t5_ws_op_t *op, uint32_t timeout_ms);
/* Length of the message waiting (complete), or -1 if none. */
int t5_ws_next_len(t5_ws_t *ws);
/* Send a close (code 1000 = normal; 0 = no code), wait up to a second for
 * the server's, close the connection and free. */
T5_MUST_CHECK t5_err_t t5_ws_close(t5_ws_t *ws, uint16_t code);

#ifdef __cplusplus
}
#endif
