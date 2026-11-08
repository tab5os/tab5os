/* SPDX-License-Identifier: Apache-2.0
 * WebSocket client (t5_net/ws.h) on the socket layer.
 *
 * Receiving streams frames through a small raw buffer into one message
 * buffer (grown up to max_msg), so a message never needs more than its own
 * size plus RAW_CAP. Control frames are handled as they arrive: pings get a
 * pong, a close gets its echo and is then reported to the app once.
 *
 * Locks: tx is held for a whole frame (frames must not interleave), rx for
 * a whole receive (one receiver at a time). Waits inside either still end
 * within a slice when the handle is closed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "netio_priv.h"
#include "netproto.h"
#include "t5_core/log.h"

static const char *TAG = "net.ws";

#define RAW_CAP 4096
#define HS_MAX 8192
#define DEFAULT_MAX_MSG (64 * 1024)
#define LIMIT_MAX_MSG (4 * 1024 * 1024)
#define TX_CHUNK 2048

struct t5_ws {
    t5_nethdr_t hdr;
    t5_sock_t *s;
    SemaphoreHandle_t tx, rx;
    size_t max_msg;
    uint8_t *raw;               /* received, not yet parsed */
    size_t raw_len;
    bool have_frame;            /* f is the frame whose payload is arriving */
    t5np_ws_frame_t f;
    uint64_t got;               /* payload bytes of f so far */
    uint8_t ctl[125];           /* control frame payload */
    uint8_t *msg;               /* the data message being joined */
    size_t msg_len, msg_cap;
    uint8_t msg_op;
    bool in_msg, ready;         /* ready: msg is complete and not yet taken */
    bool close_ready, close_reported, close_sent, failed;
    bool server;                /* the accepted side: send unmasked, require masked */
    uint8_t close_payload[125];
    size_t close_len;
    char proto[64];
};

static TickType_t lock_ticks(uint32_t timeout_ms)
{
    return pdMS_TO_TICKS(timeout_ms > 60000 ? 60000 : timeout_ms + 1);
}

/* ---- sending ---------------------------------------------------------------------------- */
static int send_frame(t5_ws_t *ws, uint8_t op, const void *data, size_t n, uint32_t timeout_ms)
{
    uint8_t mask[4];
    uint32_t r = esp_random();
    memcpy(mask, &r, 4);
    uint8_t *buf = malloc(14 + (n < TX_CHUNK ? n : TX_CHUNK));
    if (!buf) return T5_ERR_NO_MEM;
    if (xSemaphoreTake(ws->tx, lock_ticks(timeout_ms)) != pdTRUE) {
        free(buf);
        return T5_ERR_TIMEOUT;
    }
    size_t hl = t5np_ws_header(buf, op, true, n, ws->server ? NULL : mask);
    size_t off = 0;
    int res = (int)n;
    uint32_t dl = net_deadline(timeout_ms);
    /* One header, then the payload masked a chunk at a time. */
    do {
        size_t take = n - off < TX_CHUNK ? n - off : TX_CHUNK;
        if (take) memcpy(buf + hl, (const uint8_t *)data + off, take);
        if (!ws->server) t5np_ws_mask(buf + hl, take, mask, off);
        size_t len = hl + take;
        int w = ns_send(ws->s, buf, len, net_left(dl, timeout_ms));
        if (w < 0 || (size_t)w != len) {
            /* Part of a frame went out: the stream can't be resumed. */
            if (w > 0 || off) ws->failed = true;
            res = w < 0 ? w : T5_ERR_TIMEOUT;
            T5_LOGW(TAG, "%s: send stopped at %u of %u bytes (%d)%s", ws->hdr.what, (unsigned)(off + (w > 0 ? w : 0)),
                    (unsigned)(hl + n), w, ws->failed ? ", connection failed" : "");
            break;
        }
        off += take;
        hl = 0;
    } while (off < n);
    xSemaphoreGive(ws->tx);
    free(buf);
    return res;
}

static void send_close(t5_ws_t *ws, uint16_t code, uint32_t timeout_ms)
{
    if (ws->close_sent || ws->failed) return;
    ws->close_sent = true;
    uint8_t p[2] = { (uint8_t)(code >> 8), (uint8_t)code };
    int r = send_frame(ws, T5NP_WS_CLOSE, p, code ? 2 : 0, timeout_ms);
    if (r < 0) ws->failed = true;
}

/* A protocol violation by the server: tell it and stop. */
static int protocol_error(t5_ws_t *ws, uint16_t code)
{
    T5_LOGW(TAG, "%s: protocol error, closing (%u)", ws->hdr.what, code);
    send_close(ws, code, 500);
    ws->failed = true;
    return T5_ERR_IO;
}

/* ---- receiving -------------------------------------------------------------------------- */
static void raw_consume(t5_ws_t *ws, size_t n)
{
    memmove(ws->raw, ws->raw + n, ws->raw_len - n);
    ws->raw_len -= n;
}

static int start_frame(t5_ws_t *ws)
{
    int r = t5np_ws_parse(ws->raw, ws->raw_len, &ws->f);
    if (r <= 0) return r < 0 ? protocol_error(ws, 1002) : 0;
    t5np_ws_frame_t *f = &ws->f;
    if (f->masked != ws->server || f->rsv) return protocol_error(ws, 1002);
    if (f->opcode < 8) {
        if ((f->opcode == T5NP_WS_CONT) != ws->in_msg) return protocol_error(ws, 1002);
        if (f->len > ws->max_msg - ws->msg_len) return protocol_error(ws, 1009);
        if (!ws->in_msg) {
            ws->in_msg = true;
            ws->msg_op = f->opcode;
            ws->msg_len = 0;
        }
        size_t need = ws->msg_len + (size_t)f->len;
        if (need > ws->msg_cap) {
            size_t cap = ws->msg_cap ? ws->msg_cap : 1024;
            while (cap < need) cap *= 2;
            if (cap > ws->max_msg) cap = ws->max_msg;
            uint8_t *m = realloc(ws->msg, cap);
            if (!m) return protocol_error(ws, 1009);
            ws->msg = m;
            ws->msg_cap = cap;
        }
    }
    raw_consume(ws, f->hdr_len);
    ws->have_frame = true;
    ws->got = 0;
    return 1;
}

static void end_frame(t5_ws_t *ws)
{
    t5np_ws_frame_t *f = &ws->f;
    ws->have_frame = false;
    if (f->opcode < 8) {
        if (f->fin) {
            ws->in_msg = false;
            ws->ready = true;
        }
        return;
    }
    size_t n = (size_t)f->len;
    if (f->opcode == T5NP_WS_PING) {
        if (!ws->close_sent && send_frame(ws, T5NP_WS_PONG, ws->ctl, n, 1000) < 0) ws->failed = true;
    } else if (f->opcode == T5NP_WS_CLOSE) {
        memcpy(ws->close_payload, ws->ctl, n);
        ws->close_len = n;
        uint16_t code = n >= 2 ? (uint16_t)((ws->ctl[0] << 8) | ws->ctl[1]) : 0;
        send_close(ws, code ? code : 1000, 1000);
        ws->close_ready = true;
    }
}

/* Move payload bytes from raw into the message (or control) buffer. */
static void take_payload(t5_ws_t *ws)
{
    uint64_t want = ws->f.len - ws->got;
    size_t take = (uint64_t)ws->raw_len < want ? ws->raw_len : (size_t)want;
    uint8_t *dst = ws->f.opcode < 8 ? ws->msg + ws->msg_len : ws->ctl + ws->got;
    if (take) memcpy(dst, ws->raw, take);
    if (take && ws->f.masked) t5np_ws_mask(dst, take, ws->f.mask, ws->got);
    if (ws->f.opcode < 8) ws->msg_len += take;
    ws->got += take;
    raw_consume(ws, take);
    if (ws->got == ws->f.len) end_frame(ws);
}

/* Until a message (or the close) is ready. 1 ready, else a t5_err_t. */
static int pump(t5_ws_t *ws, uint32_t timeout_ms)
{
    uint32_t dl = net_deadline(timeout_ms);
    for (;;) {
        if (ws->ready || (ws->close_ready && !ws->close_reported)) return 1;
        if (ws->close_reported || ws->failed) return T5_ERR_STATE;
        int r = 1;
        if (!ws->have_frame) r = start_frame(ws);
        if (r < 0) return r;
        if (r > 0) {
            if (ws->raw_len || ws->f.len == ws->got) take_payload(ws);
            /* take_payload uses up raw unless the frame ended: then parse on. */
            if (!ws->have_frame && (ws->raw_len || ws->ready || ws->close_ready)) continue;
        }
        int got = ns_recv(ws->s, ws->raw + ws->raw_len, RAW_CAP - ws->raw_len, net_left(dl, timeout_ms));
        if (got == 0) {
            /* Gone without a close frame: report 1006 (abnormal). */
            ws->close_payload[0] = 1006 >> 8;
            ws->close_payload[1] = 1006 & 0xff;
            ws->close_len = 2;
            ws->close_ready = true;
            ws->close_sent = true;
            continue;
        }
        if (got < 0) {
            if (got != T5_ERR_TIMEOUT && got != T5_ERR_STATE) {
                ws->failed = true;
                T5_LOGW(TAG, "%s: receive failed (%d), connection failed", ws->hdr.what, got);
            }
            return got;
        }
        ws->raw_len += (size_t)got;
    }
}

int t5_ws_recv(t5_ws_t *ws, void *buf, size_t cap, t5_ws_op_t *op, uint32_t timeout_ms)
{
    if ((!buf && cap) || !op) return T5_ERR_INVALID_ARG;
    if (!nh_acquire(ws, NH_WS)) return T5_ERR_INVALID_ARG;
    int res;
    if (xSemaphoreTake(ws->rx, lock_ticks(timeout_ms)) != pdTRUE) {
        nh_release(ws);
        return T5_ERR_BUSY;
    }
    res = pump(ws, timeout_ms);
    if (res == 1 && ws->ready) {
        if (ws->msg_len > cap) {
            res = T5_ERR_NO_MEM;
        } else {
            if (ws->msg_len) memcpy(buf, ws->msg, ws->msg_len);
            *op = (t5_ws_op_t)ws->msg_op;
            res = (int)ws->msg_len;
            ws->ready = false;
            ws->msg_len = 0;
        }
    } else if (res == 1) {
        if (ws->close_len > cap) {
            res = T5_ERR_NO_MEM;
        } else {
            if (ws->close_len) memcpy(buf, ws->close_payload, ws->close_len);
            *op = T5_WS_CLOSE;
            res = (int)ws->close_len;
            ws->close_reported = true;
        }
    }
    xSemaphoreGive(ws->rx);
    nh_release(ws);
    return res;
}

int t5_ws_next_len(t5_ws_t *ws)
{
    if (!nh_acquire(ws, NH_WS)) return T5_ERR_INVALID_ARG;
    int n = -1;
    if (ws->ready) n = (int)ws->msg_len;
    else if (ws->close_ready && !ws->close_reported) n = (int)ws->close_len;
    nh_release(ws);
    return n;
}

int t5_ws_send(t5_ws_t *ws, t5_ws_op_t op, const void *data, size_t n, uint32_t timeout_ms)
{
    if (!data && n) return T5_ERR_INVALID_ARG;
    if (op != T5_WS_TEXT && op != T5_WS_BINARY && op != T5_WS_PING && op != T5_WS_PONG) return T5_ERR_INVALID_ARG;
    if (op >= T5_WS_PING && n > 125) return T5_ERR_INVALID_ARG;
    if (!nh_acquire(ws, NH_WS)) return T5_ERR_INVALID_ARG;
    int r = (ws->failed || ws->close_sent) ? T5_ERR_STATE : send_frame(ws, (uint8_t)op, data, n, timeout_ms);
    nh_release(ws);
    return r;
}

const char *t5_ws_subprotocol(const t5_ws_t *ws)
{
    if (!nh_acquire((void *)ws, NH_WS)) return "";
    const char *p = ws->proto;
    nh_release((void *)ws);
    return p;
}

bool ws_poll_buffered(t5_ws_t *ws, int *fd)
{
    *fd = ws->failed || ws->close_reported ? -1 : ns_fd(ws->s);
    if (ws->ready || (ws->close_ready && !ws->close_reported) || *fd < 0) return true;
    if (ns_pending(ws->s) > 0) return true;
    /* A whole frame already sitting in raw. */
    if (ws->have_frame) return ws->raw_len >= ws->f.len - ws->got;
    t5np_ws_frame_t f;
    return t5np_ws_parse(ws->raw, ws->raw_len, &f) != 0 && (f.hdr_len + f.len <= ws->raw_len || !f.hdr_len);
}

/* ---- handshake -------------------------------------------------------------------------- */
static bool hdr_ok(const char *s)
{
    for (; s && *s; s++) {
        if (*s == '\r' || *s == '\n') return false;
    }
    return s != NULL;
}

static int put_request(char *b, size_t n, const t5np_url_t *u, const char *key, const t5_ws_cfg_t *cfg)
{
    size_t pl = strcspn(u->path, "#");
    bool v6 = strchr(u->host, ':') != NULL;
    char port[8] = "";
    if (u->port_given) snprintf(port, sizeof port, ":%u", u->port);
    int w = snprintf(b, n,
                     "GET %s%.*s HTTP/1.1\r\nHost: %s%s%s%s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nUser-Agent: Tab5OS\r\n",
                     (pl == 0 || u->path[0] == '?') ? "/" : "", (int)pl, u->path, v6 ? "[" : "", u->host,
                     v6 ? "]" : "", port, key);
    if (w < 0 || (size_t)w >= n) return -1;
    size_t off = (size_t)w;
    if (cfg->subprotocols) {
        w = snprintf(b + off, n - off, "Sec-WebSocket-Protocol: %s\r\n", cfg->subprotocols);
        if (w < 0 || (size_t)w >= n - off) return -1;
        off += (size_t)w;
    }
    for (size_t i = 0; i < cfg->n_headers; i++) {
        w = snprintf(b + off, n - off, "%s: %s\r\n", cfg->headers[i].name, cfg->headers[i].value);
        if (w < 0 || (size_t)w >= n - off) return -1;
        off += (size_t)w;
    }
    w = snprintf(b + off, n - off, "\r\n");
    if (w < 0 || (size_t)w >= n - off) return -1;
    return (int)(off + (size_t)w);
}

static t5_err_t check_answer(t5_ws_t *ws, char *buf, size_t end, const char *key)
{
    t5np_head_t h;
    if (t5np_head_parse(buf, end, &h)) return T5_ERR_IO;
    if (h.status != 101) {
        T5_LOGW(TAG, "%s: server answered %d, not 101", ws->hdr.what, h.status);
        return h.status == 401 || h.status == 403 ? T5_ERR_DENIED : h.status == 404 ? T5_ERR_NOT_FOUND : T5_ERR_IO;
    }
    char want[29];
    t5np_ws_accept(key, want);
    const char *acc = t5np_head_get(&h, "Sec-WebSocket-Accept");
    if (!t5np_has_token(t5np_head_get(&h, "Upgrade"), "websocket") ||
        !t5np_has_token(t5np_head_get(&h, "Connection"), "upgrade") || !acc || strcmp(acc, want)) {
        T5_LOGW(TAG, "%s: bad upgrade answer", ws->hdr.what);
        return T5_ERR_IO;
    }
    const char *p = t5np_head_get(&h, "Sec-WebSocket-Protocol");
    snprintf(ws->proto, sizeof ws->proto, "%s", p ? p : "");
    return T5_OK;
}

static t5_err_t handshake(t5_ws_t *ws, const t5np_url_t *u, const t5_ws_cfg_t *cfg, uint32_t timeout)
{
    uint8_t nonce[16];
    esp_fill_random(nonce, sizeof nonce);
    char key[25];
    t5np_base64(nonce, sizeof nonce, key);
    char *buf = malloc(HS_MAX);
    if (!buf) return T5_ERR_NO_MEM;
    t5_err_t e = T5_OK;
    int len = put_request(buf, HS_MAX, u, key, cfg);
    if (len < 0) e = T5_ERR_INVALID_ARG;
    uint32_t dl = net_deadline(timeout);
    if (e == T5_OK) {
        int w = ns_send(ws->s, buf, (size_t)len, timeout);
        if (w != len) e = w < 0 ? (t5_err_t)w : T5_ERR_TIMEOUT;
    }
    size_t got = 0, end = 0;
    while (e == T5_OK && !(end = t5np_head_end(buf, got))) {
        if (got + 1 >= HS_MAX) {
            e = T5_ERR_IO;
            break;
        }
        int r = ns_recv(ws->s, buf + got, HS_MAX - 1 - got, net_left(dl, timeout));
        if (r <= 0) e = r == 0 ? T5_ERR_IO : (t5_err_t)r;
        else got += (size_t)r;
    }
    if (e == T5_OK) {
        /* Frames sent right after the answer stay for the first receive. */
        size_t extra = got - end;
        if (extra > RAW_CAP) e = T5_ERR_IO;
        else {
            memcpy(ws->raw, buf + end, extra);
            ws->raw_len = extra;
            buf[end] = 0;
            e = check_answer(ws, buf, end, key);
        }
    }
    free(buf);
    return e;
}

/* ---- open / close ------------------------------------------------------------------------- */
static void ws_free(t5_ws_t *ws)
{
    ns_destroy(ws->s);
    if (ws->tx) vSemaphoreDelete(ws->tx);
    if (ws->rx) vSemaphoreDelete(ws->rx);
    free(ws->raw);
    free(ws->msg);
    free(ws);
}

static void release_ws(void *h)
{
    /* App exit: a quick "going away", no waiting for the answer. */
    t5_ws_t *ws = h;
    if (nh_acquire(ws, NH_WS)) {
        send_close(ws, 1001, 200);
        nh_release(ws);
    }
    if (nh_unregister(ws, NH_WS) == T5_OK) ws_free(ws);
}

static t5_err_t check_cfg(const t5_ws_cfg_t *c)
{
    if (c->n_headers && !c->headers) return T5_ERR_INVALID_ARG;
    for (size_t i = 0; i < c->n_headers; i++) {
        const char *n = c->headers[i].name;
        if (!hdr_ok(n) || !*n || strchr(n, ':') || strchr(n, ' ') || !hdr_ok(c->headers[i].value)) {
            return T5_ERR_INVALID_ARG;
        }
    }
    if (c->subprotocols && !hdr_ok(c->subprotocols)) return T5_ERR_INVALID_ARG;
    return T5_OK;
}

t5_err_t t5_ws_connect(const char *url, const t5_ws_cfg_t *cfg, t5_ws_t **out)
{
    static const t5_ws_cfg_t defaults = { 0 };
    if (!out) return T5_ERR_INVALID_ARG;
    *out = NULL;
    if (!cfg) cfg = &defaults;
    t5np_url_t u;
    if (!url || t5np_url_parse(url, &u) || (u.scheme != T5NP_WS && u.scheme != T5NP_WSS)) return T5_ERR_INVALID_ARG;
    T5_RETURN_ON_ERR(check_cfg(cfg));
    t5_ws_t *ws = calloc(1, sizeof *ws);
    if (!ws) return T5_ERR_NO_MEM;
    ws->max_msg = cfg->max_msg ? (cfg->max_msg < LIMIT_MAX_MSG ? cfg->max_msg : LIMIT_MAX_MSG) : DEFAULT_MAX_MSG;
    ws->tx = xSemaphoreCreateMutex();
    ws->rx = xSemaphoreCreateMutex();
    ws->raw = malloc(RAW_CAP);
    snprintf(ws->hdr.what, sizeof ws->hdr.what, "%s %.32s", u.tls ? "wss" : "ws", u.host);
    if (!ws->tx || !ws->rx || !ws->raw) {
        ws_free(ws);
        return T5_ERR_NO_MEM;
    }
    t5_err_t e = nh_register(&ws->hdr, NH_WS, release_ws);
    if (e != T5_OK) {
        ws_free(ws);
        return e;
    }
    if (!nh_acquire(ws, NH_WS)) return T5_ERR_STATE;      /* closed already (app exit) */
    uint32_t timeout = cfg->timeout_ms ? cfg->timeout_ms : NET_DEFAULT_TIMEOUT_MS;
    t5_tcp_cfg_t tc = { .timeout_ms = timeout, .ca_pem = cfg->ca_pem, .flags = T5_TCP_NODELAY,
                        .cert_pem = cfg->cert_pem, .key_pem = cfg->key_pem };
    if (u.tls) tc.flags |= (cfg->flags & T5_HTTP_TLS_NO_VERIFY) ? T5_TCP_TLS_NO_VERIFY : T5_TCP_TLS;
    e = ns_connect(u.host, u.port, &tc, &ws->hdr.closing, &ws->s);
    if (e == T5_OK) e = handshake(ws, &u, cfg, timeout);
    nh_release(ws);
    if (e != T5_OK) {
        if (nh_unregister(ws, NH_WS) == T5_OK) ws_free(ws);
        return e;
    }
    *out = ws;
    return T5_OK;
}

t5_err_t t5_ws_close(t5_ws_t *ws, uint16_t code)
{
    if (!nh_acquire(ws, NH_WS)) return T5_ERR_INVALID_ARG;
    send_close(ws, code, 1000);
    /* Wait (a second at most) for the server's close, dropping messages. */
    if (xSemaphoreTake(ws->rx, pdMS_TO_TICKS(200)) == pdTRUE) {
        uint32_t dl = net_deadline(1000);
        while (!ws->close_ready && !ws->failed && net_left(dl, 1000)) {
            int r = pump(ws, net_left(dl, 1000));
            if (r == 1 && ws->ready) {
                ws->ready = false;
                ws->msg_len = 0;
            } else if (r != 1) {
                break;
            }
        }
        xSemaphoreGive(ws->rx);
    }
    nh_release(ws);
    T5_RETURN_ON_ERR(nh_unregister(ws, NH_WS));
    ws_free(ws);
    return T5_OK;
}

/* ---- server side (t5_net/httpd.h) ------------------------------------------------------ */
t5_err_t ws_adopt_server(t5_sock_t *s, const uint8_t *extra, size_t n, const char *proto, size_t max_msg,
                         t5_ws_t **out)
{
    if (n > RAW_CAP) return T5_ERR_INVALID_ARG;
    t5_ws_t *ws = calloc(1, sizeof *ws);
    if (!ws) return T5_ERR_NO_MEM;
    ws->server = true;
    ws->max_msg = max_msg ? (max_msg < LIMIT_MAX_MSG ? max_msg : LIMIT_MAX_MSG) : DEFAULT_MAX_MSG;
    ws->tx = xSemaphoreCreateMutex();
    ws->rx = xSemaphoreCreateMutex();
    ws->raw = malloc(RAW_CAP);
    snprintf(ws->proto, sizeof ws->proto, "%s", proto ? proto : "");
    snprintf(ws->hdr.what, sizeof ws->hdr.what, "ws server side");
    if (!ws->tx || !ws->rx || !ws->raw) {
        ws_free(ws);                    /* ws->s is still NULL: the socket stays the caller's */
        return T5_ERR_NO_MEM;
    }
    t5_err_t e = nh_register(&ws->hdr, NH_WS, release_ws);
    if (e != T5_OK) {
        ws_free(ws);
        return e;
    }
    ns_set_stop(s, &ws->hdr.closing);                    /* ours now */
    ws->s = s;
    if (n) memcpy(ws->raw, extra, n);
    ws->raw_len = n;
    *out = ws;
    return T5_OK;
}
