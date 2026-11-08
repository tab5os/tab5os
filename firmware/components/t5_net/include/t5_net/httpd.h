/* SPDX-License-Identifier: Apache-2.0
 * HTTP/1.1 server (http:// or https://) for apps with the net permission:
 * a local web UI, a REST endpoint, webhooks, WebSockets.
 *
 * Requests are taken one at a time on the app's own task, complete with
 * their body; nothing runs behind the app's back. Several clients can be
 * connecting at once (max_clients); each request gets one response and its
 * connection is closed afterwards.
 *
 *   t5_httpd_t *srv;
 *   t5_httpd_start(8080, NULL, &srv);
 *   for (;;) {
 *       t5_httpd_req_t *req;
 *       if (t5_httpd_next(srv, 1000, &req) != T5_OK) continue;
 *       if (!strcmp(t5_httpd_path(req), "/"))
 *           t5_httpd_respond(req, 200, "text/html", page, strlen(page), NULL, 0);
 *       else
 *           t5_httpd_respond(req, 404, "text/plain", "no", 2, NULL, 0);
 *   }
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"
#include "t5_net/http.h"
#include "t5_net/sock.h"
#include "t5_net/ws.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct t5_httpd t5_httpd_t;
typedef struct t5_httpd_req t5_httpd_req_t;

typedef struct {
    int max_clients;                /* connections being read at once; 0 = 4 */
    size_t max_body;                /* largest request body; 0 = 64 KB, at most 4 MB */
    uint32_t timeout_ms;            /* a client must send its request within this; 0 = 10 s */
    const char *cert_pem;           /* HTTPS: certificate chain and key; NULL for plain HTTP */
    const char *key_pem;
} t5_httpd_cfg_t;

/* Listen on port (0 = any free port: see t5_httpd_port) on every interface. */
T5_MUST_CHECK t5_err_t t5_httpd_start(uint16_t port, const t5_httpd_cfg_t *cfg, t5_httpd_t **out);
uint16_t t5_httpd_port(t5_httpd_t *srv);
/* The next complete request. T5_ERR_TIMEOUT if none came in time. Every
 * request must be answered (respond, end or upgrade), which frees it. */
T5_MUST_CHECK t5_err_t t5_httpd_next(t5_httpd_t *srv, uint32_t timeout_ms, t5_httpd_req_t **out);
T5_MUST_CHECK t5_err_t t5_httpd_stop(t5_httpd_t *srv);

const char *t5_httpd_method(const t5_httpd_req_t *req);       /* "GET" */
const char *t5_httpd_path(const t5_httpd_req_t *req);         /* "/api/x" (decoded: no) */
const char *t5_httpd_query(const t5_httpd_req_t *req);        /* "a=1&b=2", or "" */
const char *t5_httpd_header(const t5_httpd_req_t *req, const char *name);
T5_MUST_CHECK t5_err_t t5_httpd_header_at(const t5_httpd_req_t *req, int i, const char **name, const char **value);
const void *t5_httpd_body(const t5_httpd_req_t *req, size_t *len);
T5_MUST_CHECK t5_err_t t5_httpd_peer(const t5_httpd_req_t *req, t5_net_addr_t *out);

/* The whole response at once (Content-Length), then the connection closes
 * and req is freed. content_type may be NULL. */
T5_MUST_CHECK t5_err_t t5_httpd_respond(t5_httpd_req_t *req, int status, const char *content_type, const void *body,
                                        size_t len, const t5_http_header_t *headers, size_t n_headers);
/* Or streamed: begin (chunked), write as often as needed, end (frees req). */
T5_MUST_CHECK t5_err_t t5_httpd_begin(t5_httpd_req_t *req, int status, const char *content_type,
                                      const t5_http_header_t *headers, size_t n_headers);
int t5_httpd_write(t5_httpd_req_t *req, const void *data, size_t len);
T5_MUST_CHECK t5_err_t t5_httpd_end(t5_httpd_req_t *req);
/* Accept a WebSocket upgrade request (frees req): the result is a t5_ws_t
 * like a client's (t5_ws_send, t5_ws_recv, t5_ws_close). subprotocol may be
 * NULL. T5_ERR_INVALID_ARG if req isn't an upgrade. */
T5_MUST_CHECK t5_err_t t5_httpd_websocket(t5_httpd_req_t *req, const char *subprotocol, size_t max_msg, t5_ws_t **out);

#ifdef __cplusplus
}
#endif
