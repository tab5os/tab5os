/* SPDX-License-Identifier: Apache-2.0
 * Sockets for apps and the OS: DNS, TCP (plain or TLS) clients, TCP
 * servers, UDP (broadcast, multicast) and a poll over all of them. HTTP and
 * WebSocket clients are built on these (t5_net/http.h, t5_net/ws.h).
 *
 * Native apps need the net permission (the ABI stubs refuse without it);
 * Python reaches the same calls through t5.net. Every handle an app opens is
 * tracked and closed when the app exits or crashes, and an app may hold at
 * most T5_NET_APP_MAX handles at once.
 *
 * Timeouts are in milliseconds: 0 doesn't wait, T5_NET_FOREVER waits until
 * something happens. Waits run in short slices, so closing a handle from
 * another task, or the cancel flag (t5_net_cancel_flag), ends them within
 * about 50 ms.
 *
 * Calls that move bytes return a count (>= 0) or a negative t5_err_t:
 * T5_ERR_TIMEOUT if nothing moved in time, T5_ERR_IO if the connection
 * failed, T5_ERR_STATE if it was closed. A receive returning 0 means the peer
 * closed its side.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_NET_FOREVER 0xffffffffu
#define T5_NET_APP_MAX 12           /* open sockets, HTTP responses and WebSockets per app */

typedef struct t5_sock t5_sock_t;

typedef struct {
    uint8_t family;                 /* 4 or 6 */
    uint8_t ip[16];                 /* IPv4 in the first 4 bytes */
    uint16_t port;
} t5_net_addr_t;

/* "1.2.3.4", "::1" or "fe80::1" into an address (port 0). Doesn't resolve names. */
T5_MUST_CHECK t5_err_t t5_net_addr_parse(const char *s, t5_net_addr_t *out);
/* The address as text, without the port. Returns the length, or a negative t5_err_t. */
int t5_net_addr_str(const t5_net_addr_t *a, char *buf, size_t n);
/* Look up a name (IPv4 first, then IPv6), or parse a literal address. */
T5_MUST_CHECK t5_err_t t5_net_resolve(const char *host, t5_net_addr_t *out, uint32_t timeout_ms);

/* ---- TCP clients -------------------------------------------------------------------- */
#define T5_TCP_TLS (1u << 0)            /* TLS, checked against the built-in CA bundle */
#define T5_TCP_TLS_NO_VERIFY (1u << 1)  /* TLS without checking the certificate (testing only) */
#define T5_TCP_NODELAY (1u << 2)        /* no Nagle delay */

typedef struct {
    uint32_t flags;                 /* T5_TCP_* */
    uint32_t timeout_ms;            /* DNS + connect + TLS handshake; 0 = 10 s */
    const char *ca_pem;             /* NUL-terminated PEM CA to trust instead of the bundle */
    const char *sni;                /* TLS server name if not host */
    const char *const *alpn;        /* NULL-terminated protocol list, or NULL */
    const char *cert_pem;           /* client certificate for mutual TLS, or NULL */
    const char *key_pem;            /* its private key */
} t5_tcp_cfg_t;

/* Connect to host:port. cfg may be NULL (plain TCP, 10 s). */
T5_MUST_CHECK t5_err_t t5_tcp_connect(const char *host, uint16_t port, const t5_tcp_cfg_t *cfg, t5_sock_t **out);
/* Send all n bytes (TCP) or one datagram to the connected peer (UDP).
 * Returns n, or fewer if the timeout ended it part way. */
int t5_sock_send(t5_sock_t *s, const void *data, size_t n, uint32_t timeout_ms);
/* Whatever has arrived, up to n bytes; waits for at least one. */
int t5_sock_recv(t5_sock_t *s, void *buf, size_t n, uint32_t timeout_ms);
/* Stop sending (TCP half-close); receiving still works. */
T5_MUST_CHECK t5_err_t t5_sock_shutdown(t5_sock_t *s);
/* Bytes already received and buffered (TLS records decrypted), without waiting. */
int t5_sock_pending(t5_sock_t *s);
T5_MUST_CHECK t5_err_t t5_sock_local_addr(t5_sock_t *s, t5_net_addr_t *out);
T5_MUST_CHECK t5_err_t t5_sock_peer_addr(t5_sock_t *s, t5_net_addr_t *out);
/* TLS on a connected TCP socket: STARTTLS (SMTP, IMAP, XMPP...), or the
 * server side of an accepted connection (T5_TLS_SERVER, with cert_pem and
 * key_pem). Blocks for the handshake. */
#define T5_TLS_NO_VERIFY (1u << 0)      /* client: don't check the certificate (testing only) */
#define T5_TLS_SERVER (1u << 1)         /* be the server */
#define T5_TLS_NO_NAME_CHECK (1u << 2)  /* client: check the chain but not the name (no hostname known) */

typedef struct {
    uint32_t flags;                 /* T5_TLS_* */
    uint32_t timeout_ms;            /* handshake; 0 = 10 s */
    const char *sni;                /* client: the server's name, checked against its certificate */
    const char *ca_pem;             /* client: trust this CA instead of the bundle; server: require client certs from it */
    const char *cert_pem;           /* server: certificate chain (required); client: mutual-TLS certificate */
    const char *key_pem;            /* the matching private key */
    const char *const *alpn;        /* NULL-terminated protocol list, or NULL */
} t5_tls_cfg_t;

T5_MUST_CHECK t5_err_t t5_sock_start_tls(t5_sock_t *s, const t5_tls_cfg_t *cfg);

typedef enum {
    T5_SOCKOPT_NODELAY,             /* TCP: 1 = send small writes at once */
    T5_SOCKOPT_KEEPALIVE,           /* TCP: probe after value seconds idle (0 = off) */
    T5_SOCKOPT_BROADCAST,           /* UDP: may send to broadcast addresses */
    T5_SOCKOPT_REUSEADDR,
    T5_SOCKOPT_RCVBUF,              /* bytes */
    T5_SOCKOPT_TTL,                 /* IP TTL */
    T5_SOCKOPT_TOS,                 /* IP TOS / DSCP byte */
} t5_sockopt_t;

T5_MUST_CHECK t5_err_t t5_sock_set_option(t5_sock_t *s, t5_sockopt_t opt, int value);

/* Close and free. Safe to call from another task while one is waiting. */
T5_MUST_CHECK t5_err_t t5_sock_close(t5_sock_t *s);

/* ---- TCP servers --------------------------------------------------------------------- */
/* Listen on port (0 = any free port, see t5_sock_local_addr) on every
 * interface. Plain TCP only. */
T5_MUST_CHECK t5_err_t t5_tcp_listen(uint16_t port, int backlog, t5_sock_t **out);
/* Wait for a client; *out is a connected socket like t5_tcp_connect's. */
T5_MUST_CHECK t5_err_t t5_tcp_accept(t5_sock_t *server, uint32_t timeout_ms, t5_sock_t **out);

/* ---- UDP ------------------------------------------------------------------------------ */
#define T5_UDP_BROADCAST (1u << 0)      /* may send to 255.255.255.255 / subnet broadcast */
#define T5_UDP_REUSE (1u << 1)          /* share the port with other sockets (multicast listeners) */
#define T5_UDP_IPV6 (1u << 2)           /* an IPv6 socket (else IPv4) */

/* Bind a UDP socket to local_port (0 = any). */
T5_MUST_CHECK t5_err_t t5_udp_open(uint16_t local_port, uint32_t flags, t5_sock_t **out);
/* Fix the peer for t5_sock_send / t5_sock_recv (and filter what arrives). */
T5_MUST_CHECK t5_err_t t5_udp_connect(t5_sock_t *s, const t5_net_addr_t *to);
/* One datagram (up to 65507 bytes). Returns n. */
int t5_udp_sendto(t5_sock_t *s, const t5_net_addr_t *to, const void *data, size_t n, uint32_t timeout_ms);
/* One datagram, truncated to n; from may be NULL. */
int t5_udp_recvfrom(t5_sock_t *s, void *buf, size_t n, t5_net_addr_t *from, uint32_t timeout_ms);
/* IPv4 multicast membership on the Wi-Fi interface ("239.1.2.3"). */
T5_MUST_CHECK t5_err_t t5_udp_join(t5_sock_t *s, const char *group);
T5_MUST_CHECK t5_err_t t5_udp_leave(t5_sock_t *s, const char *group);
/* Multicast TTL (1 = this network, the default) and loopback of our own packets. */
T5_MUST_CHECK t5_err_t t5_udp_multicast_opts(t5_sock_t *s, uint8_t ttl, bool loop);

/* ---- poll ------------------------------------------------------------------------------ */
#define T5_POLL_IN (1u << 0)            /* data (or a client, or a message) is waiting; or EOF */
#define T5_POLL_OUT (1u << 1)           /* sending won't wait */
#define T5_POLL_ERR (1u << 2)           /* the connection failed or was closed (always reported) */

typedef struct {
    void *handle;                   /* t5_sock_t *, t5_http_t * or t5_ws_t * */
    uint8_t events;                 /* T5_POLL_IN | T5_POLL_OUT */
    uint8_t revents;                /* filled in */
} t5_poll_t;

/* Wait until one of the handles is ready. Returns how many are (0 on
 * timeout), or a negative t5_err_t. At most 16 handles. */
int t5_net_poll(t5_poll_t *items, int n, uint32_t timeout_ms);

/* ---- OS side --------------------------------------------------------------------------- */
/* Every blocking network call made by the calling task stops early (with
 * T5_ERR_STATE) once *flag becomes true. NULL removes it. The MicroPython VM
 * uses this so Stop gets through a connect or a DNS lookup. Not in the ABI. */
void t5_net_cancel_flag(volatile bool *flag);

/* A number for a live handle (any kind), 0 if it isn't one. Pointers can be
 * reused after a close; the number isn't (for 2^32 opens). Not in the ABI. */
uint32_t t5_net_handle_id(void *h);

/* Open handles, for diagnostics: one line each into buf. Returns the count. */
int t5_net_handles_dump(char *buf, size_t n);

#ifdef __cplusplus
}
#endif
