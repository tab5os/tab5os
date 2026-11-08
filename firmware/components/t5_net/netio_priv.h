/* SPDX-License-Identifier: Apache-2.0
 * Shared by sock.c, http.c and ws.c: the handle registry and the socket
 * layer underneath the HTTP and WebSocket clients.
 *
 * Every public handle starts with a t5_nethdr_t and lives in one registry
 * list. A call first acquires the handle (it must be in the list and not
 * closing), which takes a reference; close takes it out of the list, marks
 * it closing so waits end, and frees once the last reference is gone. A
 * stale or foreign pointer is refused rather than dereferenced beyond the
 * list walk.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "t5_core/err.h"
#include "t5_net/http.h"
#include "t5_net/sock.h"
#include "t5_net/ws.h"

#define NH_SOCK 0x534f434bu   /* "SOCK" */
#define NH_HTTP 0x48545450u   /* "HTTP" */
#define NH_WS 0x57534f43u     /* "WSOC" */
#define NH_MQTT 0x4d515454u   /* "MQTT" */
#define NH_HTTPD 0x48545044u  /* "HTPD" */

#define NET_SLICE_MS 50
#define NET_DEFAULT_TIMEOUT_MS 10000

typedef struct t5_nethdr {
    uint32_t magic;
    struct t5_nethdr *next;
    void *app;                  /* owner (t5_app_ctx_get() at open), NULL = system */
    volatile bool closing;
    int refs;
    uint32_t id;                /* never reused soon: see t5_net_handle_id */
    char what[40];              /* for t5_net_handles_dump */
} t5_nethdr_t;

/* Add to the registry and the app's resource list. T5_ERR_BUSY past the
 * app's quota. release is what the app runtime calls on exit. */
t5_err_t nh_register(t5_nethdr_t *h, uint32_t magic, void (*release)(void *));
/* Take a reference, or false if h isn't a live handle of this kind. */
bool nh_acquire(void *h, uint32_t magic);
void nh_release(void *h);
/* Unlink and wait for other users to let go. Then the caller frees. */
t5_err_t nh_unregister(void *h, uint32_t magic);
/* Is h (any kind) live? Used by poll. Fills *magic. */
bool nh_acquire_any(void *h, uint32_t *magic);

/* ---- time and cancellation ------------------------------------------------------------ */
uint32_t net_now_ms(void);
/* Deadline for timeout_ms from now (T5_NET_FOREVER stays forever). */
uint32_t net_deadline(uint32_t timeout_ms);
/* ms left until the deadline (0 = passed). */
uint32_t net_left(uint32_t deadline, uint32_t timeout_ms);
/* The calling task's cancel flag is set. */
bool net_cancelled(void);

/* ---- sockets without registration (owned by an HTTP or WebSocket handle) -------------- */
/* stop: extra flag that ends waits (the owner's closing), may be NULL. */
t5_err_t ns_connect(const char *host, uint16_t port, const t5_tcp_cfg_t *cfg, volatile bool *stop, t5_sock_t **out);
int ns_send(t5_sock_t *s, const void *data, size_t n, uint32_t timeout_ms);
int ns_recv(t5_sock_t *s, void *buf, size_t n, uint32_t timeout_ms);
int ns_pending(t5_sock_t *s);
int ns_fd(t5_sock_t *s);
void ns_destroy(t5_sock_t *s);
t5_err_t ns_listen(uint16_t port, int backlog, t5_sock_t **out);
t5_err_t ns_accept(t5_sock_t *srv, uint32_t timeout_ms, t5_sock_t **out);
t5_err_t ns_start_tls(t5_sock_t *s, const t5_tls_cfg_t *cfg);
t5_err_t ns_addr(t5_sock_t *s, t5_net_addr_t *out, bool peer);
void ns_set_stop(t5_sock_t *s, volatile bool *stop);
/* The accepted side of a WebSocket upgrade (httpd.c). */
t5_err_t ws_adopt_server(t5_sock_t *s, const uint8_t *extra, size_t n, const char *proto, size_t max_msg,
                         t5_ws_t **out);

/* esp-tls crt_bundle_attach stand-in that turns verification off. */
#include "esp_err.h"
esp_err_t net_attach_no_verify(void *conf);

/* For poll: is there buffered data in an HTTP response / WebSocket? */
bool http_poll_buffered(t5_http_t *h, int *fd);
bool ws_poll_buffered(t5_ws_t *ws, int *fd);
bool mqtt_poll_buffered(void *m);
bool httpd_poll_buffered(void *srv, int *fd);
