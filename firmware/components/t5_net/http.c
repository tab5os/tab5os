/* SPDX-License-Identifier: Apache-2.0
 * HTTP/1.1 client (t5_net/http.h) on the socket layer.
 *
 * One connection per request ("Connection: close"), so a body without a
 * length simply ends when the server closes. The response head is read
 * into one buffer (at most NET_HEAD_MAX) and parsed in place; whatever arrived
 * after it waits in rx and is decoded on the first reads.
 *
 * The handle is registered before connecting, so an app that exits during
 * a slow connect has it closed (and the connect cut short) like any other.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "netio_priv.h"
#include "netproto.h"
#include "t5_core/log.h"

static const char *TAG = "net.http";

#define RX_CAP 2048
#define MAX_REDIRECTS 5

struct t5_http {
    t5_nethdr_t hdr;
    t5_sock_t *s;
    char *url;                  /* the URL being fetched (after redirects) */
    char *head;                 /* response head, parsed in place */
    t5np_head_t h;
    int64_t clen;               /* Content-Length, -1 if none */
    int64_t left;               /* body bytes still to come (Content-Length) */
    bool chunked, done;
    t5np_chunk_t ch;
    uint8_t *rx;                /* raw bytes received past the head */
    size_t rx_off, rx_len, rx_cap;
    uint32_t timeout;
};

/* ---- request text --------------------------------------------------------------------- */
typedef struct {
    char *p;
    size_t len, cap;
    bool oom;
} sb_t;

static void sb_add(sb_t *b, const char *s, size_t n)
{
    if (b->oom) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 512;
        while (cap < b->len + n + 1) cap *= 2;
        char *p = realloc(b->p, cap);
        if (!p) {
            b->oom = true;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void sb_str(sb_t *b, const char *s)
{
    sb_add(b, s, strlen(s));
}

static bool clean(const char *s)
{
    for (; s && *s; s++) {
        if (*s == '\r' || *s == '\n') return false;
    }
    return s != NULL;
}

static bool user_has(const t5_http_req_t *r, const char *name)
{
    for (size_t i = 0; i < r->n_headers; i++) {
        if (!strcasecmp(r->headers[i].name, name)) return true;
    }
    return false;
}

/* The caller-supplied headers we leave out: framing is ours. */
static bool reserved(const char *name)
{
    return !strcasecmp(name, "Host") || !strcasecmp(name, "Content-Length") ||
           !strcasecmp(name, "Transfer-Encoding") || !strcasecmp(name, "Connection");
}

static void add_host(sb_t *b, const t5np_url_t *u)
{
    sb_str(b, "Host: ");
    bool v6 = strchr(u->host, ':') != NULL;
    if (v6) sb_str(b, "[");
    sb_str(b, u->host);
    if (v6) sb_str(b, "]");
    if (u->port_given) {
        char p[8];
        snprintf(p, sizeof p, ":%u", u->port);
        sb_str(b, p);
    }
    sb_str(b, "\r\n");
}

static void add_basic_auth(sb_t *b, const t5np_url_t *u)
{
    if (!u->userinfo || !u->userinfo_len || u->userinfo_len > 256) return;
    char enc[4 * ((256 + 2) / 3) + 1];
    t5np_base64((const uint8_t *)u->userinfo, u->userinfo_len, enc);
    sb_str(b, "Authorization: Basic ");
    sb_str(b, enc);
    sb_str(b, "\r\n");
}

static char *build_request(const t5_http_req_t *r, const char *method, const t5np_url_t *u, size_t body_len,
                           bool with_body, size_t *len)
{
    sb_t b = { 0 };
    size_t pl = strcspn(u->path, "#");
    sb_str(&b, method);
    sb_str(&b, " ");
    if (pl == 0 || u->path[0] == '?') sb_str(&b, "/");
    sb_add(&b, u->path, pl);
    sb_str(&b, " HTTP/1.1\r\n");
    add_host(&b, u);
    if (!user_has(r, "User-Agent")) sb_str(&b, "User-Agent: Tab5OS\r\n");
    if (!user_has(r, "Accept")) sb_str(&b, "Accept: */*\r\n");
    if (!user_has(r, "Authorization")) add_basic_auth(&b, u);
    sb_str(&b, "Connection: close\r\n");
    if (with_body) {
        char cl[40];
        snprintf(cl, sizeof cl, "Content-Length: %u\r\n", (unsigned)body_len);
        sb_str(&b, cl);
    }
    for (size_t i = 0; i < r->n_headers; i++) {
        if (reserved(r->headers[i].name)) continue;
        sb_str(&b, r->headers[i].name);
        sb_str(&b, ": ");
        sb_str(&b, r->headers[i].value);
        sb_str(&b, "\r\n");
    }
    sb_str(&b, "\r\n");
    if (b.oom) {
        free(b.p);
        return NULL;
    }
    *len = b.len;
    return b.p;
}

static t5_err_t check_req(const t5_http_req_t *r)
{
    if (!r || !r->url || (r->body_len && !r->body)) return T5_ERR_INVALID_ARG;
    if (r->method) {
        if (!*r->method || strlen(r->method) > 16) return T5_ERR_INVALID_ARG;
        for (const char *p = r->method; *p; p++) {
            if (*p < 'A' || *p > 'Z') return T5_ERR_INVALID_ARG;
        }
    }
    if (r->n_headers && !r->headers) return T5_ERR_INVALID_ARG;
    for (size_t i = 0; i < r->n_headers; i++) {
        const char *n = r->headers[i].name;
        if (!clean(n) || !*n || strchr(n, ':') || strchr(n, ' ') || !clean(r->headers[i].value)) {
            return T5_ERR_INVALID_ARG;
        }
    }
    return T5_OK;
}

/* ---- response head --------------------------------------------------------------------- */
/* Read until the end of a head; it goes into h->head (parsed), the rest into rx. */
static t5_err_t read_head(t5_http_t *h)
{
    size_t cap = 2048, len = 0;
    char *buf = malloc(cap);
    if (!buf) return T5_ERR_NO_MEM;
    /* Bytes left over from an earlier (1xx) head come first. */
    if (h->rx && h->rx_off < h->rx_len) {
        size_t n = h->rx_len - h->rx_off;
        if (n > cap - 1) n = cap - 1;
        memcpy(buf, h->rx + h->rx_off, n);
        len = n;
        h->rx_off += n;
    }
    size_t end;
    for (;;) {
        end = t5np_head_end(buf, len);
        if (end) break;
        if (len + 1 >= cap) {
            if (cap >= NET_HEAD_MAX) {
                free(buf);
                return T5_ERR_IO;
            }
            char *nb = realloc(buf, cap * 2);
            if (!nb) {
                free(buf);
                return T5_ERR_NO_MEM;
            }
            buf = nb;
            cap *= 2;
        }
        int r = ns_recv(h->s, buf + len, cap - 1 - len, h->timeout);
        if (r <= 0) {
            free(buf);
            return r == 0 ? T5_ERR_IO : (t5_err_t)r;
        }
        len += (size_t)r;
    }
    size_t extra = len - end;
    if (!h->rx) {
        h->rx_cap = extra > RX_CAP ? extra : RX_CAP;
        h->rx = malloc(h->rx_cap);
        if (!h->rx) {
            free(buf);
            return T5_ERR_NO_MEM;
        }
        h->rx_off = h->rx_len = 0;
    }
    /* Whatever is left in rx follows the bytes we took from it. */
    size_t keep = h->rx_len - h->rx_off;
    if (extra + keep > h->rx_cap) {
        uint8_t *nr = realloc(h->rx, extra + keep);
        if (!nr) {
            free(buf);
            return T5_ERR_NO_MEM;
        }
        h->rx = nr;
        h->rx_cap = extra + keep;
    }
    memmove(h->rx + extra, h->rx + h->rx_off, keep);
    memcpy(h->rx, buf + end, extra);
    h->rx_off = 0;
    h->rx_len = extra + keep;
    free(h->head);
    h->head = buf;
    buf[end] = 0;
    return t5np_head_parse(buf, end, &h->h) ? T5_ERR_IO : T5_OK;
}

static t5_err_t body_mode(t5_http_t *h, const char *method)
{
    int st = h->h.status;
    h->clen = -1;
    h->chunked = false;
    h->done = false;
    if (!strcmp(method, "HEAD") || st < 200 || st == 204 || st == 304) {
        h->done = true;
        return T5_OK;
    }
    if (t5np_has_token(t5np_head_get(&h->h, "Transfer-Encoding"), "chunked")) {
        h->chunked = true;
        t5np_chunk_init(&h->ch);
        return T5_OK;
    }
    const char *cl = t5np_head_get(&h->h, "Content-Length");
    if (cl) {
        char *e;
        long long v = strtoll(cl, &e, 10);
        if (e == cl || *e || v < 0) return T5_ERR_IO;
        h->clen = h->left = v;
        h->done = v == 0;
    }
    return T5_OK;
}

/* ---- one exchange --------------------------------------------------------------------- */
static t5_err_t send_request(t5_http_t *h, const t5_http_req_t *r, const char *method, bool with_body)
{
    t5np_url_t u;
    if (t5np_url_parse(h->url, &u) || (u.scheme != T5NP_HTTP && u.scheme != T5NP_HTTPS)) return T5_ERR_INVALID_ARG;
    t5_tcp_cfg_t tc = { .timeout_ms = h->timeout, .ca_pem = r->ca_pem, .flags = T5_TCP_NODELAY,
                        .cert_pem = r->cert_pem, .key_pem = r->key_pem };
    if (u.tls) tc.flags |= (r->flags & T5_HTTP_TLS_NO_VERIFY) ? T5_TCP_TLS_NO_VERIFY : T5_TCP_TLS;
    T5_RETURN_ON_ERR(ns_connect(u.host, u.port, &tc, &h->hdr.closing, &h->s));
    size_t len;
    char *req = build_request(r, method, &u, with_body ? r->body_len : 0, with_body, &len);
    if (!req) return T5_ERR_NO_MEM;
    int w = ns_send(h->s, req, len, h->timeout);
    free(req);
    if (w >= 0 && (size_t)w != len) return T5_ERR_TIMEOUT;
    if (w < 0) return (t5_err_t)w;
    if (with_body && r->body_len) {
        w = ns_send(h->s, r->body, r->body_len, h->timeout);
        if (w >= 0 && (size_t)w != r->body_len) return T5_ERR_TIMEOUT;
        if (w < 0) return (t5_err_t)w;
    }
    /* Skip interim 1xx answers (at most a few). */
    for (int i = 0; i < 4; i++) {
        T5_RETURN_ON_ERR(read_head(h));
        if (h->h.status >= 200 || h->h.status == 101) return T5_OK;
    }
    return T5_ERR_IO;
}

static bool is_redirect(int st)
{
    return st == 301 || st == 302 || st == 303 || st == 307 || st == 308;
}

static void drop_conn(t5_http_t *h)
{
    ns_destroy(h->s);
    h->s = NULL;
    free(h->rx);
    h->rx = NULL;
    h->rx_off = h->rx_len = h->rx_cap = 0;
}

static t5_err_t follow(t5_http_t *h, const char *loc)
{
    size_t n = strlen(h->url) + strlen(loc) + 2;
    char *next = malloc(n);
    if (!next) return T5_ERR_NO_MEM;
    if (t5np_url_join(h->url, loc, next, n)) {
        free(next);
        return T5_ERR_IO;
    }
    free(h->url);
    h->url = next;
    return T5_OK;
}

static t5_err_t run(t5_http_t *h, const t5_http_req_t *r)
{
    char method[17];
    snprintf(method, sizeof method, "%s", r->method ? r->method : "GET");
    bool with_body = r->body != NULL || !strcmp(method, "POST") || !strcmp(method, "PUT") || !strcmp(method, "PATCH");
    for (int hop = 0;; hop++) {
        T5_RETURN_ON_ERR(send_request(h, r, method, with_body));
        int st = h->h.status;
        const char *loc = t5np_head_get(&h->h, "Location");
        if ((r->flags & T5_HTTP_NO_REDIRECT) || !is_redirect(st) || !loc || !*loc) break;
        if (hop >= MAX_REDIRECTS) return T5_ERR_IO;
        T5_RETURN_ON_ERR(follow(h, loc));
        if (st == 303 || ((st == 301 || st == 302) && strcmp(method, "GET") && strcmp(method, "HEAD"))) {
            snprintf(method, sizeof method, "GET");
            with_body = false;
        }
        drop_conn(h);
    }
    return body_mode(h, r->method ? r->method : "GET");
}

static void http_free(t5_http_t *h)
{
    ns_destroy(h->s);
    free(h->rx);
    free(h->head);
    free(h->url);
    free(h);
}

static void destroy_http(void *h)
{
    http_free(h);
}

static void release_http(void *h)
{
    T5_IGNORE(t5_http_close(h));        /* app exit: nothing to report to */
}

t5_err_t t5_http_open(const t5_http_req_t *req, t5_http_t **out)
{
    if (!out) return T5_ERR_INVALID_ARG;
    *out = NULL;
    T5_RETURN_ON_ERR(check_req(req));
    t5np_url_t u;
    if (t5np_url_parse(req->url, &u) || (u.scheme != T5NP_HTTP && u.scheme != T5NP_HTTPS)) return T5_ERR_INVALID_ARG;
    t5_http_t *h = calloc(1, sizeof *h);
    if (!h) return T5_ERR_NO_MEM;
    h->url = strdup(req->url);
    h->timeout = req->timeout_ms ? req->timeout_ms : NET_DEFAULT_TIMEOUT_MS;
    snprintf(h->hdr.what, sizeof h->hdr.what, "http %.32s", u.host);
    if (!h->url) {
        free(h);
        return T5_ERR_NO_MEM;
    }
    t5_err_t e = nh_register(&h->hdr, NH_HTTP, release_http, destroy_http);
    if (e != T5_OK) {
        http_free(h);
        return e;
    }
    if (!nh_acquire(h, NH_HTTP)) return T5_ERR_STATE;   /* closed already (app exit) */
    e = run(h, req);
    nh_release(h);
    if (e != T5_OK) {
        if (e != T5_ERR_STATE) T5_LOGD(TAG, "%s: %s", h->url, t5_err_str(e));
        T5_IGNORE(nh_unregister(h, NH_HTTP));     /* or someone else closed it already (app exit) */
        return e;
    }
    *out = h;
    return T5_OK;
}

/* ---- the response ----------------------------------------------------------------------- */
int t5_http_status(const t5_http_t *h)
{
    if (!nh_acquire((void *)h, NH_HTTP)) return T5_ERR_INVALID_ARG;
    int st = h->h.status;
    nh_release((void *)h);
    return st;
}

const char *t5_http_header(const t5_http_t *h, const char *name)
{
    if (!name || !nh_acquire((void *)h, NH_HTTP)) return NULL;
    const char *v = t5np_head_get(&h->h, name);
    nh_release((void *)h);
    return v;
}

t5_err_t t5_http_header_at(const t5_http_t *h, int i, const char **name, const char **value)
{
    if (!name || !value || !nh_acquire((void *)h, NH_HTTP)) return T5_ERR_INVALID_ARG;
    t5_err_t e = T5_ERR_NOT_FOUND;
    if (i >= 0 && i < h->h.n) {
        *name = h->h.name[i];
        *value = h->h.value[i];
        e = T5_OK;
    }
    nh_release((void *)h);
    return e;
}

int64_t t5_http_content_length(const t5_http_t *h)
{
    if (!nh_acquire((void *)h, NH_HTTP)) return -1;
    int64_t v = h->clen;
    nh_release((void *)h);
    return v;
}

const char *t5_http_url(const t5_http_t *h)
{
    if (!nh_acquire((void *)h, NH_HTTP)) return NULL;
    const char *u = h->url;
    nh_release((void *)h);
    return u;
}

/* Decode from rx into buf. Returns bytes produced (may be 0 with rx used up). */
static int from_rx(t5_http_t *h, uint8_t *buf, size_t n)
{
    size_t avail = h->rx_len - h->rx_off;
    if (!h->chunked) {
        size_t take = avail < n ? avail : n;
        if (h->clen >= 0 && (int64_t)take > h->left) take = (size_t)h->left;
        memcpy(buf, h->rx + h->rx_off, take);
        h->rx_off += take;
        if (h->clen >= 0) {
            h->left -= (int64_t)take;
            if (h->left == 0) h->done = true;
        }
        return (int)take;
    }
    size_t take = avail < n ? avail : n;
    memcpy(buf, h->rx + h->rx_off, take);
    size_t used;
    size_t out = t5np_chunk_decode(&h->ch, buf, take, &used);
    h->rx_off += used;
    if (h->ch.bad) return T5_ERR_IO;
    if (h->ch.done) h->done = true;
    return (int)out;
}

static int read_body(t5_http_t *h, uint8_t *buf, size_t n, uint32_t timeout_ms)
{
    uint32_t dl = net_deadline(timeout_ms);
    /* Bounded: every pass either returns or consumes received bytes. */
    for (;;) {
        if (h->done) return 0;
        if (h->rx_off < h->rx_len) {
            int r = from_rx(h, buf, n);
            if (r != 0) return r;
            continue;
        }
        uint32_t left = net_left(dl, timeout_ms);
        if (!h->chunked) {
            size_t want = n;
            if (h->clen >= 0 && (int64_t)want > h->left) want = (size_t)h->left;
            int r = ns_recv(h->s, buf, want, left);
            if (r == 0) {
                if (h->clen >= 0) return T5_ERR_IO;   /* cut short */
                h->done = true;
                return 0;
            }
            if (r > 0 && h->clen >= 0) {
                h->left -= r;
                if (h->left == 0) h->done = true;
            }
            return r;
        }
        int r = ns_recv(h->s, h->rx, h->rx_cap, left);
        if (r == 0) return T5_ERR_IO;                 /* the last chunk never came */
        if (r < 0) return r;
        h->rx_off = 0;
        h->rx_len = (size_t)r;
    }
}

int t5_http_read(t5_http_t *h, void *buf, size_t n, uint32_t timeout_ms)
{
    if (!buf || !n) return T5_ERR_INVALID_ARG;
    if (!nh_acquire(h, NH_HTTP)) return T5_ERR_INVALID_ARG;
    int r = read_body(h, buf, n, timeout_ms);
    nh_release(h);
    return r;
}

bool http_poll_buffered(t5_http_t *h, int *fd)
{
    *fd = -1;
    if (h->done || !h->s) return true;
    if (h->rx_off < h->rx_len) return true;
    *fd = ns_fd(h->s);
    return ns_pending(h->s) > 0;
}

t5_err_t t5_http_close(t5_http_t *h)
{
    return nh_unregister(h, NH_HTTP);
}

int t5_http_fetch(const t5_http_req_t *req, void *buf, size_t cap, int *status)
{
    if (!buf && cap) return T5_ERR_INVALID_ARG;
    t5_http_t *h;
    T5_RETURN_ON_ERR(t5_http_open(req, &h));
    if (status) *status = t5_http_status(h);
    uint32_t to = req->timeout_ms ? req->timeout_ms : NET_DEFAULT_TIMEOUT_MS;
    size_t len = 0;
    int r = 0;
    for (;;) {
        uint8_t probe;
        uint8_t *dst = len < cap ? (uint8_t *)buf + len : &probe;
        r = t5_http_read(h, dst, len < cap ? cap - len : 1, to);
        if (r <= 0) break;
        if (dst == &probe) {
            r = T5_ERR_NO_MEM;
            break;
        }
        len += (size_t)r;
    }
    T5_IGNORE(t5_http_close(h));      /* the result is already decided */
    return r < 0 ? r : (int)len;
}
