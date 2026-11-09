/* SPDX-License-Identifier: Apache-2.0
 * HTTP/1.1 server (t5_net/httpd.h) on the socket layer.
 *
 * The server owns its listening socket and up to max_clients connections
 * that are still sending their request (none of them count against the
 * app's handle limit). t5_httpd_next accepts, reads whatever has arrived
 * on each, and hands out the first complete request; the request (a handle
 * of its own) then owns the connection until it is answered. Requests are
 * answered with "Connection: close".
 *
 * A request's buffer is its head (parsed in place once complete, so the
 * header pointers stay valid) followed by its body, decoded if it was
 * chunked.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "lwip/sockets.h"
#include "netio_priv.h"
#include "t5_net/httpd.h"
#include "netproto.h"
#include "t5_core/log.h"
#include "t5_core/os.h"

static const char *TAG = "net.httpd";

#define MAX_CLIENTS 8
#define DEFAULT_BODY (64 * 1024)
#define LIMIT_BODY (4 * 1024 * 1024)

typedef struct {
    t5_sock_t *s;
    uint32_t since;
    uint8_t *buf;
    size_t len, cap;
    size_t head_end;            /* 0 until the head is complete */
    bool chunked;
    int64_t clen;
    t5np_chunk_t ch;
    size_t raw_off;             /* chunked: next undecoded byte */
    size_t body_len;            /* bytes of (decoded) body after head_end */
    bool done;
} conn_t;

struct t5_httpd {
    t5_nethdr_t hdr;
    t5_sock_t *ls;
    uint16_t port;
    int max_clients;
    size_t max_body;
    uint32_t timeout;
    char *cert, *key;
    conn_t c[MAX_CLIENTS];
    int n;
};

struct t5_httpd_req {
    t5_nethdr_t hdr;
    t5_sock_t *s;
    uint8_t *buf;
    t5np_head_t h;
    char *path, *query;
    uint8_t *body;
    size_t body_len;
    const uint8_t *extra;       /* bytes past a bodiless request (early WebSocket frames) */
    size_t extra_len;
    bool streaming, head_only;
};

/* ---- connections being read ----------------------------------------------------------------- */
static void conn_drop(t5_httpd_t *srv, int i)
{
    ns_destroy(srv->c[i].s);
    free(srv->c[i].buf);
    srv->c[i] = srv->c[--srv->n];
    memset(&srv->c[srv->n], 0, sizeof srv->c[srv->n]);
}

static void send_simple(t5_sock_t *s, int status, const char *msg)
{
    char b[160];
    int l = snprintf(b, sizeof b, "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status, msg);
    if (l > 0) (void)ns_send(s, b, (size_t)l, 1000);   /* best effort: the connection closes next */
}

/* The head just completed: learn how the body is framed (from a copy, so
 * the real parse happens once the buffer has stopped moving). */
static int head_done(t5_httpd_t *srv, conn_t *c)
{
    char *copy = malloc(c->head_end);
    if (!copy) return -1;
    memcpy(copy, c->buf, c->head_end);
    t5np_head_t h;
    int r = t5np_req_parse(copy, c->head_end, &h);
    if (r == 0) {
        const char *te = t5np_head_get(&h, "Transfer-Encoding");
        const char *cl = t5np_head_get(&h, "Content-Length");
        c->chunked = t5np_has_token(te, "chunked");
        c->clen = 0;
        if (c->chunked) {
            t5np_chunk_init(&c->ch);
            c->raw_off = c->head_end;
        } else if (cl) {
            char *e;
            long long v = strtoll(cl, &e, 10);
            if (e == cl || *e || v < 0) r = 400;
            else if ((size_t)v > srv->max_body) r = 413;
            else c->clen = v;
        }
        if (!r && t5np_has_token(t5np_head_get(&h, "Expect"), "100-continue")) {
            static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
            if (ns_send(c->s, cont, sizeof cont - 1, 1000) < 0) r = -1;
        }
    } else {
        r = 400;
    }
    free(copy);
    if (r > 0) send_simple(c->s, r, r == 413 ? "Payload Too Large" : "Bad Request");
    return r;
}

/* New bytes arrived: advance the parse. <0 or an HTTP error: drop. */
static int conn_progress(t5_httpd_t *srv, conn_t *c)
{
    if (!c->head_end) {
        size_t end = t5np_head_end((const char *)c->buf, c->len);
        if (!end) {
            if (c->len >= NET_HEAD_MAX) {
                send_simple(c->s, 431, "Request Header Fields Too Large");
                return 431;
            }
            return 0;
        }
        c->head_end = end;
        int r = head_done(srv, c);
        if (r) return r;
        if (!c->chunked) c->body_len = 0;
    }
    if (c->chunked) {
        size_t used;
        size_t got = t5np_chunk_decode(&c->ch, c->buf + c->raw_off, c->len - c->raw_off, &used);
        memmove(c->buf + c->head_end + c->body_len, c->buf + c->raw_off, got);
        c->body_len += got;
        c->raw_off += used;
        if (c->ch.bad || c->body_len > srv->max_body) {
            send_simple(c->s, 400, "Bad Request");
            return 400;
        }
        c->done = c->ch.done;
    } else {
        size_t have = c->len - c->head_end;
        c->body_len = have < (size_t)c->clen ? have : (size_t)c->clen;
        c->done = have >= (size_t)c->clen;
    }
    return 0;
}

/* Read what's there without waiting. 0 ok, <0 gone. */
static int conn_read(t5_httpd_t *srv, conn_t *c)
{
    for (int pass = 0; pass < 8 && !c->done; pass++) {
        size_t limit = NET_HEAD_MAX + 2 * srv->max_body + 64;
        if (c->len + 1024 > c->cap) {
            size_t cap = c->cap ? c->cap * 2 : 2048;
            if (cap > limit) cap = limit;
            if (cap <= c->len) return -1;
            uint8_t *nb = realloc(c->buf, cap);
            if (!nb) return -1;
            c->buf = nb;
            c->cap = cap;
        }
        int r = ns_recv(c->s, c->buf + c->len, c->cap - c->len - 1, 0);
        if (r == T5_ERR_TIMEOUT) return 0;
        if (r <= 0) return -1;
        c->len += (size_t)r;
        if (conn_progress(srv, c)) return -1;
    }
    return 0;
}

static void accept_new(t5_httpd_t *srv)
{
    while (srv->n < srv->max_clients) {
        t5_sock_t *s;
        if (ns_accept(srv->ls, 0, &s) != T5_OK) return;
        ns_set_stop(s, &srv->hdr.closing);
        if (srv->cert) {
            t5_tls_cfg_t t = { .flags = T5_TLS_SERVER, .cert_pem = srv->cert, .key_pem = srv->key, .timeout_ms = 5000 };
            if (ns_start_tls(s, &t) != T5_OK) {
                ns_destroy(s);
                continue;
            }
        }
        srv->c[srv->n++] = (conn_t){ .s = s, .since = net_now_ms() };
    }
}

/* ---- requests ----------------------------------------------------------------------------- */
static void req_free(t5_httpd_req_t *r)
{
    ns_destroy(r->s);
    free(r->buf);
    free(r);
}

static void destroy_req(void *h)
{
    req_free(h);
}

static void release_req(void *h)
{
    T5_IGNORE(nh_unregister(h, NH_REQ));
}

static t5_err_t make_req(t5_httpd_t *srv, int i, t5_httpd_req_t **out)
{
    conn_t *c = &srv->c[i];
    t5_httpd_req_t *r = calloc(1, sizeof *r);
    if (!r) return T5_ERR_NO_MEM;
    if (t5np_req_parse((char *)c->buf, c->head_end, &r->h)) {
        free(r);
        return T5_ERR_IO;
    }
    r->s = c->s;
    r->buf = c->buf;
    r->body = c->buf + c->head_end;
    r->body_len = c->body_len;
    r->body[r->body_len] = 0;                      /* room was left for it */
    if (!c->chunked && c->len > c->head_end + c->body_len) {
        r->extra = c->buf + c->head_end + c->body_len;
        r->extra_len = c->len - c->head_end - c->body_len;
    }
    r->head_only = !strcmp(r->h.method, "HEAD");
    /* Split target into path and query, in place. */
    r->path = (char *)r->h.target;
    char *q = strchr(r->path, '?');
    r->query = q ? q + 1 : (char *)"";
    if (q) *q = 0;
    char *frag = strchr(r->query[0] ? r->query : r->path, '#');
    if (frag) *frag = 0;
    snprintf(r->hdr.what, sizeof r->hdr.what, "http req %.24s", r->path);
    /* The connection is the request's now. */
    srv->c[i] = srv->c[--srv->n];
    memset(&srv->c[srv->n], 0, sizeof srv->c[srv->n]);
    t5_err_t e = nh_register(&r->hdr, NH_REQ, release_req, destroy_req);
    if (e != T5_OK) {
        send_simple(r->s, 503, "Service Unavailable");
        req_free(r);
        return e;
    }
    *out = r;
    return T5_OK;
}

/* One wait over the listening socket and every connection, a slice at most. */
static void wait_any(t5_httpd_t *srv, uint32_t left)
{
    fd_set rd;
    FD_ZERO(&rd);
    int maxfd = ns_fd(srv->ls);
    FD_SET(maxfd, &rd);
    for (int i = 0; i < srv->n; i++) {
        if (ns_pending(srv->c[i].s) > 0) return;    /* TLS has data already */
        int fd = ns_fd(srv->c[i].s);
        FD_SET(fd, &rd);
        if (fd > maxfd) maxfd = fd;
    }
    uint32_t ms = left < NET_SLICE_MS ? left : NET_SLICE_MS;
    struct timeval tv = { .tv_sec = 0, .tv_usec = (suseconds_t)ms * 1000 };
    select(maxfd + 1, &rd, NULL, NULL, &tv);
}

/* Accept, read and expire; the index of a complete request, or -1. */
static int service(t5_httpd_t *srv)
{
    accept_new(srv);
    for (int i = srv->n - 1; i >= 0; i--) {
        conn_t *c = &srv->c[i];
        if (conn_read(srv, c) < 0) {
            conn_drop(srv, i);
        } else if (!c->done && net_now_ms() - c->since > srv->timeout) {
            send_simple(c->s, 408, "Request Timeout");
            conn_drop(srv, i);
        }
    }
    for (int i = 0; i < srv->n; i++) {
        if (srv->c[i].done) return i;
    }
    return -1;
}

t5_err_t t5_httpd_next(t5_httpd_t *srv, uint32_t timeout_ms, t5_httpd_req_t **out)
{
    if (!out) return T5_ERR_INVALID_ARG;
    *out = NULL;
    if (!nh_acquire(srv, NH_HTTPD)) return T5_ERR_INVALID_ARG;
    uint32_t dl = net_deadline(timeout_ms);
    t5_err_t e;
    for (;;) {
        int i = service(srv);
        if (i >= 0) {
            e = make_req(srv, i, out);
            if (e == T5_OK) break;
            continue;
        }
        if (srv->hdr.closing || net_cancelled()) {
            e = T5_ERR_STATE;
            break;
        }
        uint32_t left = net_left(dl, timeout_ms);
        if (!left) {
            e = T5_ERR_TIMEOUT;
            break;
        }
        wait_any(srv, left);
    }
    nh_release(srv);
    return e;
}

bool httpd_poll_buffered(void *h, int *fd)
{
    t5_httpd_t *srv = h;
    *fd = -2;                                      /* several fds: checked here each slice */
    fd_set rd;
    FD_ZERO(&rd);
    int maxfd = ns_fd(srv->ls);
    FD_SET(maxfd, &rd);
    for (int i = 0; i < srv->n; i++) {
        if (srv->c[i].done || ns_pending(srv->c[i].s) > 0) return true;
        int f = ns_fd(srv->c[i].s);
        FD_SET(f, &rd);
        if (f > maxfd) maxfd = f;
    }
    struct timeval tv = { 0, 0 };
    return select(maxfd + 1, &rd, NULL, NULL, &tv) > 0;
}

/* ---- start / stop ------------------------------------------------------------------------- */
static void httpd_free(t5_httpd_t *srv)
{
    while (srv->n) conn_drop(srv, srv->n - 1);
    ns_destroy(srv->ls);
    free(srv->cert);
    free(srv->key);
    free(srv);
}

static void destroy_httpd(void *h)
{
    httpd_free(h);
}

static void release_httpd(void *h)
{
    T5_IGNORE(t5_httpd_stop(h));       /* app exit: nothing to report to */
}

t5_err_t t5_httpd_start(uint16_t port, const t5_httpd_cfg_t *cfg, t5_httpd_t **out)
{
    static const t5_httpd_cfg_t defaults = { 0 };
    if (!out) return T5_ERR_INVALID_ARG;
    *out = NULL;
    if (!cfg) cfg = &defaults;
    if (!cfg->cert_pem != !cfg->key_pem) return T5_ERR_INVALID_ARG;
    t5_httpd_t *srv = calloc(1, sizeof *srv);
    if (!srv) return T5_ERR_NO_MEM;
    srv->max_clients = cfg->max_clients > 0 && cfg->max_clients <= MAX_CLIENTS ? cfg->max_clients : 4;
    srv->max_body = cfg->max_body ? (cfg->max_body < LIMIT_BODY ? cfg->max_body : LIMIT_BODY) : DEFAULT_BODY;
    srv->timeout = cfg->timeout_ms ? cfg->timeout_ms : NET_DEFAULT_TIMEOUT_MS;
    if (cfg->cert_pem) {
        srv->cert = strdup(cfg->cert_pem);
        srv->key = strdup(cfg->key_pem);
        if (!srv->cert || !srv->key) {
            httpd_free(srv);
            return T5_ERR_NO_MEM;
        }
    }
    t5_err_t e = ns_listen(port, srv->max_clients, &srv->ls);
    if (e != T5_OK) {
        httpd_free(srv);
        return e;
    }
    ns_set_stop(srv->ls, &srv->hdr.closing);
    t5_net_addr_t a;
    srv->port = ns_addr(srv->ls, &a, false) == T5_OK ? a.port : port;
    snprintf(srv->hdr.what, sizeof srv->hdr.what, "%s server :%u", srv->cert ? "https" : "http", srv->port);
    e = nh_register(&srv->hdr, NH_HTTPD, release_httpd, destroy_httpd);
    if (e != T5_OK) {
        httpd_free(srv);
        return e;
    }
    T5_LOGI(TAG, "%s", srv->hdr.what);
    *out = srv;
    return T5_OK;
}

uint16_t t5_httpd_port(t5_httpd_t *srv)
{
    if (!nh_acquire(srv, NH_HTTPD)) return 0;
    uint16_t p = srv->port;
    nh_release(srv);
    return p;
}

t5_err_t t5_httpd_stop(t5_httpd_t *srv)
{
    return nh_unregister(srv, NH_HTTPD);
}

/* ---- reading a request ---------------------------------------------------------------------- */
#define REQ_GET(r, field, fallback)                                                              \
    do {                                                                                         \
        if (!nh_acquire((void *)(r), NH_REQ)) return fallback;                                   \
        __typeof__((r)->field) v_ = (r)->field;                                                  \
        nh_release((void *)(r));                                                                 \
        return v_;                                                                               \
    } while (0)

const char *t5_httpd_method(const t5_httpd_req_t *req)
{
    REQ_GET(req, h.method, NULL);
}

const char *t5_httpd_path(const t5_httpd_req_t *req)
{
    REQ_GET(req, path, NULL);
}

const char *t5_httpd_query(const t5_httpd_req_t *req)
{
    REQ_GET(req, query, NULL);
}

const char *t5_httpd_header(const t5_httpd_req_t *req, const char *name)
{
    if (!name || !nh_acquire((void *)req, NH_REQ)) return NULL;
    const char *v = t5np_head_get(&req->h, name);
    nh_release((void *)req);
    return v;
}

t5_err_t t5_httpd_header_at(const t5_httpd_req_t *req, int i, const char **name, const char **value)
{
    if (!name || !value || !nh_acquire((void *)req, NH_REQ)) return T5_ERR_INVALID_ARG;
    t5_err_t e = T5_ERR_NOT_FOUND;
    if (i >= 0 && i < req->h.n) {
        *name = req->h.name[i];
        *value = req->h.value[i];
        e = T5_OK;
    }
    nh_release((void *)req);
    return e;
}

const void *t5_httpd_body(const t5_httpd_req_t *req, size_t *len)
{
    if (!nh_acquire((void *)req, NH_REQ)) return NULL;
    if (len) *len = req->body_len;
    const void *b = req->body;
    nh_release((void *)req);
    return b;
}

t5_err_t t5_httpd_peer(const t5_httpd_req_t *req, t5_net_addr_t *out)
{
    if (!out || !nh_acquire((void *)req, NH_REQ)) return T5_ERR_INVALID_ARG;
    t5_err_t e = ns_addr(req->s, out, true);
    nh_release((void *)req);
    return e;
}

/* ---- answering -------------------------------------------------------------------------------- */
static const char *reason(int st)
{
    static const struct {
        int code;
        const char *text;
    } T[] = {
        { 100, "Continue" }, { 101, "Switching Protocols" }, { 200, "OK" }, { 201, "Created" }, { 202, "Accepted" },
        { 204, "No Content" }, { 206, "Partial Content" }, { 301, "Moved Permanently" }, { 302, "Found" },
        { 303, "See Other" }, { 304, "Not Modified" }, { 307, "Temporary Redirect" }, { 308, "Permanent Redirect" },
        { 400, "Bad Request" }, { 401, "Unauthorized" }, { 403, "Forbidden" }, { 404, "Not Found" },
        { 405, "Method Not Allowed" }, { 409, "Conflict" }, { 413, "Payload Too Large" }, { 415, "Unsupported Media Type" },
        { 429, "Too Many Requests" }, { 500, "Internal Server Error" }, { 501, "Not Implemented" },
        { 503, "Service Unavailable" },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        if (T[i].code == st) return T[i].text;
    }
    return "Status";
}

static bool hdr_clean(const char *s)
{
    return s && !strpbrk(s, "\r\n");
}

/* Status line and headers. len < 0: chunked. */
static t5_err_t send_head(t5_httpd_req_t *r, int status, const char *ctype, long long len, const t5_http_header_t *h,
                          size_t nh)
{
    if (status < 100 || status > 999 || (ctype && !hdr_clean(ctype)) || (nh && !h)) return T5_ERR_INVALID_ARG;
    for (size_t i = 0; i < nh; i++) {
        if (!hdr_clean(h[i].name) || !*h[i].name || strchr(h[i].name, ':') || !hdr_clean(h[i].value)) {
            return T5_ERR_INVALID_ARG;
        }
    }
    size_t cap = 256 + (ctype ? strlen(ctype) : 0);
    for (size_t i = 0; i < nh; i++) cap += strlen(h[i].name) + strlen(h[i].value) + 4;
    char *b = malloc(cap);
    if (!b) return T5_ERR_NO_MEM;
    int off = snprintf(b, cap, "HTTP/1.1 %d %s\r\nServer: Tab5OS\r\nConnection: close\r\n", status, reason(status));
    if (ctype) off += snprintf(b + off, cap - (size_t)off, "Content-Type: %s\r\n", ctype);
    if (len >= 0) off += snprintf(b + off, cap - (size_t)off, "Content-Length: %lld\r\n", len);
    else off += snprintf(b + off, cap - (size_t)off, "Transfer-Encoding: chunked\r\n");
    for (size_t i = 0; i < nh; i++) off += snprintf(b + off, cap - (size_t)off, "%s: %s\r\n", h[i].name, h[i].value);
    off += snprintf(b + off, cap - (size_t)off, "\r\n");
    int w = ns_send(r->s, b, (size_t)off, NET_DEFAULT_TIMEOUT_MS);
    free(b);
    return w == off ? T5_OK : w < 0 ? (t5_err_t)w : T5_ERR_TIMEOUT;
}

static t5_err_t finish(t5_httpd_req_t *r)
{
    nh_release(r);
    return nh_unregister(r, NH_REQ);
}

t5_err_t t5_httpd_respond(t5_httpd_req_t *req, int status, const char *content_type, const void *body, size_t len,
                          const t5_http_header_t *headers, size_t n_headers)
{
    if (len && !body) return T5_ERR_INVALID_ARG;
    if (!nh_acquire(req, NH_REQ)) return T5_ERR_INVALID_ARG;
    if (req->streaming) {
        nh_release(req);
        return T5_ERR_STATE;
    }
    t5_err_t e = send_head(req, status, content_type, (long long)len, headers, n_headers);
    if (e == T5_OK && len && !req->head_only) {
        int w = ns_send(req->s, body, len, NET_DEFAULT_TIMEOUT_MS);
        if (w < 0 || (size_t)w != len) e = w < 0 ? (t5_err_t)w : T5_ERR_TIMEOUT;
    }
    if (e == T5_ERR_INVALID_ARG) {
        nh_release(req);
        return e;                          /* the app can try again */
    }
    t5_err_t f = finish(req);
    return e != T5_OK ? e : f;
}

t5_err_t t5_httpd_begin(t5_httpd_req_t *req, int status, const char *content_type, const t5_http_header_t *headers,
                        size_t n_headers)
{
    if (!nh_acquire(req, NH_REQ)) return T5_ERR_INVALID_ARG;
    t5_err_t e = req->streaming ? T5_ERR_STATE : send_head(req, status, content_type, -1, headers, n_headers);
    if (e == T5_OK) req->streaming = true;
    nh_release(req);
    return e;
}

int t5_httpd_write(t5_httpd_req_t *req, const void *data, size_t len)
{
    if (!data && len) return T5_ERR_INVALID_ARG;
    if (!nh_acquire(req, NH_REQ)) return T5_ERR_INVALID_ARG;
    int r = (int)len;
    if (!req->streaming) {
        r = T5_ERR_STATE;
    } else if (len && !req->head_only) {
        char sz[16];
        int l = snprintf(sz, sizeof sz, "%x\r\n", (unsigned)len);
        if (ns_send(req->s, sz, (size_t)l, NET_DEFAULT_TIMEOUT_MS) != l ||
            ns_send(req->s, data, len, NET_DEFAULT_TIMEOUT_MS) != (int)len ||
            ns_send(req->s, "\r\n", 2, NET_DEFAULT_TIMEOUT_MS) != 2) {
            r = T5_ERR_IO;
        }
    }
    nh_release(req);
    return r;
}

t5_err_t t5_httpd_end(t5_httpd_req_t *req)
{
    if (!nh_acquire(req, NH_REQ)) return T5_ERR_INVALID_ARG;
    t5_err_t e = T5_OK;
    if (!req->streaming) e = send_head(req, 204, NULL, 0, NULL, 0);
    else if (!req->head_only && ns_send(req->s, "0\r\n\r\n", 5, NET_DEFAULT_TIMEOUT_MS) != 5) e = T5_ERR_IO;
    t5_err_t f = finish(req);
    return e != T5_OK ? e : f;
}

t5_err_t t5_httpd_websocket(t5_httpd_req_t *req, const char *subprotocol, size_t max_msg, t5_ws_t **out)
{
    if (!out || (subprotocol && !hdr_clean(subprotocol))) return T5_ERR_INVALID_ARG;
    *out = NULL;
    if (!nh_acquire(req, NH_REQ)) return T5_ERR_INVALID_ARG;
    const char *key = t5np_head_get(&req->h, "Sec-WebSocket-Key");
    if (req->streaming || strcmp(req->h.method, "GET") || !key || strlen(key) > 64 ||
        !t5np_has_token(t5np_head_get(&req->h, "Upgrade"), "websocket") ||
        !t5np_has_token(t5np_head_get(&req->h, "Connection"), "upgrade")) {
        nh_release(req);
        return T5_ERR_INVALID_ARG;
    }
    char acc[29];
    t5np_ws_accept(key, acc);
    char b[256];
    int l = snprintf(b, sizeof b,
                     "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n%s%s%s\r\n",
                     acc, subprotocol ? "Sec-WebSocket-Protocol: " : "", subprotocol ? subprotocol : "",
                     subprotocol ? "\r\n" : "");
    t5_err_t e = (l > 0 && (size_t)l < sizeof b && ns_send(req->s, b, (size_t)l, 5000) == l) ? T5_OK : T5_ERR_IO;
    t5_ws_t *ws = NULL;
    if (e == T5_OK) e = ws_adopt_server(req->s, req->extra, req->extra_len, subprotocol, max_msg, &ws);
    if (e == T5_OK) req->s = NULL;         /* the WebSocket owns the connection now */
    nh_release(req);
    T5_IGNORE(nh_unregister(req, NH_REQ));
    if (e != T5_OK) return e;
    *out = ws;
    return T5_OK;
}
