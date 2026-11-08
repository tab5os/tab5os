/* SPDX-License-Identifier: Apache-2.0
 * Protocol helpers, see netproto.h. Everything here works on caller
 * buffers and stops at the input's length.
 */
#include "netproto.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* ---- URLs ---------------------------------------------------------------------------- */
static const struct {
    const char *name;
    t5np_scheme_t scheme;
    uint16_t port;
} SCHEMES[] = {
    { "http", T5NP_HTTP, 80 }, { "https", T5NP_HTTPS, 443 }, { "ws", T5NP_WS, 80 }, { "wss", T5NP_WSS, 443 },
};

static int parse_port(const char *s, size_t n, uint16_t *out)
{
    if (n == 0 || n > 5) return -1;
    uint32_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (uint32_t)(s[i] - '0');
    }
    if (v == 0 || v > 65535) return -1;
    *out = (uint16_t)v;
    return 0;
}

static int parse_host(const char *s, size_t n, t5np_url_t *u)
{
    const char *colon = NULL;
    size_t hn;
    const char *h = s;
    if (n && s[0] == '[') {
        const char *close = memchr(s, ']', n);
        if (!close) return -1;
        h = s + 1;
        hn = (size_t)(close - h);
        size_t rest = n - (size_t)(close + 1 - s);
        if (rest) {
            if (close[1] != ':') return -1;
            colon = close + 1;
        }
    } else {
        colon = memchr(s, ':', n);
        hn = colon ? (size_t)(colon - s) : n;
    }
    if (hn == 0 || hn > T5NP_HOST_MAX) return -1;
    for (size_t i = 0; i < hn; i++) {
        unsigned char c = (unsigned char)h[i];
        if (c <= ' ' || c == 0x7f || c == '/' || c == '\\' || c == '@') return -1;
    }
    memcpy(u->host, h, hn);
    u->host[hn] = 0;
    if (colon) {
        if (parse_port(colon + 1, (size_t)(s + n - colon - 1), &u->port)) return -1;
        u->port_given = true;
    }
    return 0;
}

int t5np_url_parse(const char *url, t5np_url_t *u)
{
    if (!url || !u) return -1;
    memset(u, 0, sizeof *u);
    const char *sep = strstr(url, "://");
    if (!sep) return -1;
    size_t sl = (size_t)(sep - url);
    bool found = false;
    for (size_t i = 0; i < sizeof SCHEMES / sizeof SCHEMES[0]; i++) {
        if (strlen(SCHEMES[i].name) == sl && !strncasecmp(url, SCHEMES[i].name, sl)) {
            u->scheme = SCHEMES[i].scheme;
            u->port = SCHEMES[i].port;
            found = true;
        }
    }
    if (!found) return -1;
    u->tls = u->scheme == T5NP_HTTPS || u->scheme == T5NP_WSS;
    const char *a = sep + 3;
    size_t an = strcspn(a, "/?#");
    const char *at = NULL;
    for (size_t i = 0; i < an; i++) {
        if (a[i] == '@') at = a + i;
    }
    if (at) {
        u->userinfo = a;
        u->userinfo_len = (size_t)(at - a);
        an -= (size_t)(at + 1 - a);
        a = at + 1;
    }
    if (parse_host(a, an, u)) return -1;
    u->path = a + an;
    for (const char *p = u->path; *p; p++) {
        if ((unsigned char)*p <= ' ' || *p == 0x7f) return -1;   /* would split the request line */
    }
    return 0;
}

static int put(char *out, size_t n, const char *a, size_t an, const char *b, size_t bn)
{
    if (an + bn + 1 > n) return -1;
    memcpy(out, a, an);
    memcpy(out + an, b, bn);
    out[an + bn] = 0;
    return 0;
}

int t5np_url_join(const char *base, const char *ref, char *out, size_t n)
{
    t5np_url_t u;
    if (!base || !ref || !out || t5np_url_parse(base, &u)) return -1;
    size_t rn = strlen(ref);
    int r;
    if (strstr(ref, "://") && strcspn(ref, ":") < strcspn(ref, "/?#")) {
        r = put(out, n, ref, rn, "", 0);
    } else if (ref[0] == '/' && ref[1] == '/') {
        const char *colon = strchr(base, ':');
        r = put(out, n, base, (size_t)(colon + 1 - base), ref, rn);
    } else if (ref[0] == '/') {
        r = put(out, n, base, (size_t)(u.path - base), ref, rn);
    } else if (ref[0] == '?' || ref[0] == 0) {
        size_t keep = (size_t)(u.path - base) + strcspn(u.path, rn ? "?#" : "#");
        r = put(out, n, base, keep, ref, rn);
    } else {
        /* Relative path: replace everything after the last '/' of the path. */
        size_t pl = strcspn(u.path, "?#");
        size_t keep = (size_t)(u.path - base);
        for (size_t i = 0; i < pl; i++) {
            if (u.path[i] == '/') keep = (size_t)(u.path - base) + i + 1;
        }
        if (keep == (size_t)(u.path - base)) {
            /* No path at all: "http://h" + "x" -> "http://h/x". */
            if (keep + 1 + rn + 1 > n) return -1;
            memcpy(out, base, keep);
            out[keep] = '/';
            memcpy(out + keep + 1, ref, rn + 1);
            r = 0;
        } else {
            r = put(out, n, base, keep, ref, rn);
        }
    }
    if (r) return -1;
    t5np_url_t check;
    return t5np_url_parse(out, &check);
}

/* ---- HTTP response head ---------------------------------------------------------------- */
size_t t5np_head_end(const char *buf, size_t n)
{
    for (size_t i = 0; i + 1 < n; i++) {
        if (buf[i] != '\n') continue;
        if (buf[i + 1] == '\n') return i + 2;
        if (i + 2 < n && buf[i + 1] == '\r' && buf[i + 2] == '\n') return i + 3;
    }
    return 0;
}

static char *trim(char *s, char *end)
{
    while (s < end && (*s == ' ' || *s == '\t')) s++;
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
    *end = 0;
    return s;
}

/* Header lines from line to end, into h. 0, or -1 if malformed. */
static int parse_fields(char *line, char *end, t5np_head_t *h)
{
    while (line < end) {
        char *nl = memchr(line, '\n', (size_t)(end - line));
        if (!nl) break;
        if (line == nl || (line + 1 == nl && line[0] == '\r')) break;   /* the empty line */
        if (line[0] == ' ' || line[0] == '\t') {                         /* obsolete folding: drop */
            line = nl + 1;
            continue;
        }
        char *colon = memchr(line, ':', (size_t)(nl - line));
        if (!colon || colon == line) return -1;
        *colon = 0;
        for (char *p = line; p < colon; p++) {
            if (*p <= ' ' || *p == 0x7f) return -1;
        }
        if (h->n < T5NP_MAX_HEADERS) {
            h->name[h->n] = line;
            h->value[h->n] = trim(colon + 1, nl);
            h->n++;
        }
        line = nl + 1;
    }
    return 0;
}

int t5np_head_parse(char *buf, size_t len, t5np_head_t *h)
{
    memset(h, 0, sizeof *h);
    char *end = buf + len;
    char *nl = memchr(buf, '\n', len);
    if (!nl) return -1;
    char *sl = trim(buf, nl);
    if (strncmp(sl, "HTTP/1.", 7) || !isdigit((unsigned char)sl[7]) || sl[8] != ' ') return -1;
    h->minor = sl[7] - '0';
    if (!isdigit((unsigned char)sl[9]) || !isdigit((unsigned char)sl[10]) || !isdigit((unsigned char)sl[11]) ||
        (sl[12] != 0 && sl[12] != ' ')) {
        return -1;
    }
    h->status = (sl[9] - '0') * 100 + (sl[10] - '0') * 10 + (sl[11] - '0');
    if (h->status < 100) return -1;
    return parse_fields(nl + 1, end, h);
}

int t5np_req_parse(char *buf, size_t len, t5np_head_t *h)
{
    memset(h, 0, sizeof *h);
    char *end = buf + len;
    char *nl = memchr(buf, '\n', len);
    if (!nl) return -1;
    char *sl = trim(buf, nl);
    char *sp1 = strchr(sl, ' ');
    if (!sp1 || sp1 == sl || sp1 - sl > 16) return -1;
    char *sp2 = strchr(sp1 + 1, ' ');
    if (!sp2 || sp2 == sp1 + 1) return -1;
    *sp1 = 0;
    *sp2 = 0;
    for (char *p = sl; *p; p++) {
        if (*p < 'A' || *p > 'Z') return -1;
    }
    if (strncmp(sp2 + 1, "HTTP/1.", 7) || !isdigit((unsigned char)sp2[8]) || sp2[9]) return -1;
    h->method = sl;
    h->target = sp1 + 1;
    h->minor = sp2[8] - '0';
    for (char *p = sp1 + 1; *p; p++) {
        if ((unsigned char)*p <= ' ' || *p == 0x7f) return -1;
    }
    return parse_fields(nl + 1, end, h);
}

const char *t5np_head_get(const t5np_head_t *h, const char *name)
{
    for (int i = 0; i < h->n; i++) {
        if (!strcasecmp(h->name[i], name)) return h->value[i];
    }
    return NULL;
}

bool t5np_has_token(const char *v, const char *token)
{
    if (!v) return false;
    size_t tl = strlen(token);
    while (*v) {
        while (*v == ' ' || *v == '\t' || *v == ',') v++;
        size_t l = strcspn(v, ",");
        size_t t = l;
        while (t && (v[t - 1] == ' ' || v[t - 1] == '\t')) t--;
        if (t == tl && !strncasecmp(v, token, tl)) return true;
        v += l;
    }
    return false;
}

/* ---- chunked transfer coding ----------------------------------------------------------- */
enum { CH_SIZE, CH_EXT, CH_DATA, CH_DATA_CR, CH_DATA_LF, CH_TRAILER, CH_TRAILER_LINE, CH_DONE };

void t5np_chunk_init(t5np_chunk_t *c)
{
    memset(c, 0, sizeof *c);
}

static int hexval(uint8_t ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* One header byte (anything but chunk data). */
static void chunk_ctl(t5np_chunk_t *c, uint8_t ch)
{
    switch (c->state) {
    case CH_SIZE: {
        int v = hexval(ch);
        if (v >= 0) {
            if (++c->digits > 15) c->bad = true;
            c->left = c->left * 16 + (uint64_t)v;
        } else if (ch == ';' || ch == ' ' || ch == '\t' || ch == '\r') {
            c->state = c->digits ? CH_EXT : (c->bad = true, CH_SIZE);
        } else if (ch == '\n' && c->digits) {
            c->state = c->left ? CH_DATA : CH_TRAILER;
        } else {
            c->bad = true;
        }
        break;
    }
    case CH_EXT:
        if (ch == '\n') c->state = c->left ? CH_DATA : CH_TRAILER;
        break;
    case CH_DATA_CR:
        if (ch == '\r') c->state = CH_DATA_LF;
        else if (ch == '\n') c->state = CH_SIZE;
        else c->bad = true;
        break;
    case CH_DATA_LF:
        if (ch == '\n') c->state = CH_SIZE;
        else c->bad = true;
        break;
    case CH_TRAILER:
        if (ch == '\n') c->state = CH_DONE;
        else if (ch != '\r') c->state = CH_TRAILER_LINE;
        break;
    case CH_TRAILER_LINE:
        if (ch == '\n') c->state = CH_TRAILER;
        break;
    default:
        break;
    }
    if (c->state == CH_SIZE && ch == '\n') {
        c->digits = 0;
        c->left = 0;
    }
    if (c->state == CH_DONE) c->done = true;
}

size_t t5np_chunk_decode(t5np_chunk_t *c, uint8_t *buf, size_t n, size_t *used)
{
    size_t in = 0, out = 0;
    while (in < n && !c->done && !c->bad) {
        if (c->state == CH_DATA) {
            size_t take = n - in;
            if ((uint64_t)take > c->left) take = (size_t)c->left;
            memmove(buf + out, buf + in, take);
            out += take;
            in += take;
            c->left -= take;
            if (c->left == 0) c->state = CH_DATA_CR;
        } else {
            chunk_ctl(c, buf[in++]);
        }
    }
    if (used) *used = in;
    return out;
}

/* ---- WebSocket frames ------------------------------------------------------------------- */
size_t t5np_ws_header(uint8_t out[14], uint8_t opcode, bool fin, uint64_t len, const uint8_t *mask)
{
    size_t i = 0;
    out[i++] = (uint8_t)((fin ? 0x80 : 0) | (opcode & 0x0f));
    uint8_t m = mask ? 0x80 : 0;
    if (len < 126) {
        out[i++] = (uint8_t)(m | len);
    } else if (len <= 0xffff) {
        out[i++] = m | 126;
        out[i++] = (uint8_t)(len >> 8);
        out[i++] = (uint8_t)len;
    } else {
        out[i++] = m | 127;
        for (int b = 7; b >= 0; b--) out[i++] = (uint8_t)(len >> (8 * b));
    }
    if (mask) {
        memcpy(out + i, mask, 4);
        i += 4;
    }
    return i;
}

void t5np_ws_mask(uint8_t *data, size_t n, const uint8_t mask[4], uint64_t off)
{
    for (size_t i = 0; i < n; i++) data[i] ^= mask[(off + i) & 3];
}

int t5np_ws_parse(const uint8_t *buf, size_t n, t5np_ws_frame_t *f)
{
    if (n < 2) return 0;
    memset(f, 0, sizeof *f);
    f->fin = buf[0] & 0x80;
    f->rsv = (buf[0] >> 4) & 7;
    f->opcode = buf[0] & 0x0f;
    f->masked = buf[1] & 0x80;
    uint64_t len = buf[1] & 0x7f;
    size_t h = 2;
    if ((f->opcode > 2 && f->opcode < 8) || f->opcode > 10) return -1;
    if (len == 126) {
        if (n < 4) return 0;
        len = ((uint64_t)buf[2] << 8) | buf[3];
        h = 4;
    } else if (len == 127) {
        if (n < 10) return 0;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | buf[2 + i];
        if (len >> 63) return -1;
        h = 10;
    }
    if (f->opcode >= 8 && (len > 125 || !f->fin)) return -1;
    if (f->masked) {
        if (n < h + 4) return 0;
        memcpy(f->mask, buf + h, 4);
        h += 4;
    }
    f->len = len;
    f->hdr_len = h;
    return 1;
}

/* ---- SHA-1 ------------------------------------------------------------------------------- */
#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_block(uint32_t st[5], const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    }
    for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = ROL(b, 30);
        b = a;
        a = t;
    }
    st[0] += a;
    st[1] += b;
    st[2] += c;
    st[3] += d;
    st[4] += e;
}

void t5np_sha1(const void *data, size_t n, uint8_t out[20])
{
    uint32_t st[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    const uint8_t *p = data;
    size_t left = n;
    while (left >= 64) {
        sha1_block(st, p);
        p += 64;
        left -= 64;
    }
    uint8_t tail[128] = { 0 };
    memcpy(tail, p, left);
    tail[left] = 0x80;
    size_t tl = left + 1 + 8 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) tail[tl - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha1_block(st, tail);
    if (tl == 128) sha1_block(st, tail + 64);
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(st[i] >> 24);
        out[4 * i + 1] = (uint8_t)(st[i] >> 16);
        out[4 * i + 2] = (uint8_t)(st[i] >> 8);
        out[4 * i + 3] = (uint8_t)st[i];
    }
}

/* ---- base64 ------------------------------------------------------------------------------ */
size_t t5np_base64(const uint8_t *in, size_t n, char *out)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = i + 1 < n ? T[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? T[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

void t5np_ws_accept(const char *key, char out[29])
{
    static const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char buf[128];
    int l = snprintf(buf, sizeof buf, "%s%s", key, GUID);
    if (l < 0 || (size_t)l >= sizeof buf) l = 0;
    uint8_t d[20];
    t5np_sha1(buf, (size_t)l, d);
    t5np_base64(d, sizeof d, out);
}
