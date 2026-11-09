/* SPDX-License-Identifier: Apache-2.0
 * Raw DEFLATE decoder (RFC 1951). Structure follows Mark Adler's puff.c
 * (canonical Huffman decode with count/symbol tables), with a 9-bit lookup
 * table in front for speed, a pull input buffer and a 32 KB ring window that
 * is flushed to a sink, so memory use does not depend on the output size.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "t5_pkg/inflate.h"
#include "pkg_test.h"

#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288
#define MAXCODES (MAXLCODES + MAXDCODES)
#define WSIZE 32768u
#define WMASK (WSIZE - 1)
#define INBUF 1024
#define FAST_BITS 9

typedef struct {
    uint16_t count[MAXBITS + 1];       /* number of codes of each length */
    uint16_t symbol[FIXLCODES];        /* symbols ordered by code */
    uint16_t fast[1u << FAST_BITS];    /* (len << 9) | sym for short codes, 0 = use slow path */
} huff_t;

typedef struct {
    t5_inflate_read_fn rd;
    void *rctx;
    t5_inflate_write_fn wr;
    void *wctx;
    uint8_t in[INBUF];
    size_t in_pos, in_len;
    bool in_eof;
    uint32_t bitbuf;
    int bitcnt;
    t5_err_t err;                      /* sticky: first error wins */
    uint8_t win[WSIZE];
    size_t wpos;
    size_t total;
    huff_t lencode, distcode;
} st_t;

static void set_err(st_t *s, t5_err_t e)
{
    if (s->err == T5_OK) {
        s->err = e;
    }
}

static int next_byte(st_t *s)
{
    if (s->in_pos == s->in_len) {
        if (s->in_eof) {
            return -1;
        }
        int n = s->rd(s->rctx, s->in, INBUF);
        if (n <= 0) {
            s->in_eof = true;
            if (n < 0) {
                set_err(s, T5_ERR_IO);
            }
            return -1;
        }
        s->in_pos = 0;
        s->in_len = (size_t)n > INBUF ? INBUF : (size_t)n;
    }
    return s->in[s->in_pos++];
}

/* Take need (0..16) bits. Running out of input is a malformed stream. */
static uint32_t bits(st_t *s, int need)
{
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        int b = next_byte(s);
        if (b < 0) {
            set_err(s, T5_ERR_INVALID_ARG);
            return 0;
        }
        val |= (uint32_t)b << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return val & ((1u << need) - 1);
}

/* Best-effort look-ahead for the fast table; never an error by itself. */
static void fill(st_t *s, int need)
{
    while (s->bitcnt < need) {
        int b = next_byte(s);
        if (b < 0) {
            return;
        }
        s->bitbuf |= (uint32_t)b << s->bitcnt;
        s->bitcnt += 8;
    }
}

static int decode(st_t *s, const huff_t *h)
{
    if (s->bitcnt < FAST_BITS) {
        fill(s, FAST_BITS);
    }
    if (s->bitcnt >= FAST_BITS) {
        uint16_t e = h->fast[s->bitbuf & ((1u << FAST_BITS) - 1)];
        if (e) {
            int len = e >> 9;
            s->bitbuf >>= len;
            s->bitcnt -= len;
            return e & 0x1ff;
        }
    }
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        code |= (int)bits(s, 1);
        if (s->err) {
            return -1;
        }
        int count = h->count[len];
        if (code - count < first) {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    set_err(s, T5_ERR_INVALID_ARG);   /* ran out of codes */
    return -1;
}

static uint32_t reverse(uint32_t code, int len)
{
    uint32_t r = 0;
    for (int i = 0; i < len; i++) {
        r = (r << 1) | (code & 1);
        code >>= 1;
    }
    return r;
}

/* Returns 0 for a complete code, > 0 incomplete, < 0 over-subscribed. */
static int construct(huff_t *h, const uint8_t *length, int n)
{
    memset(h->count, 0, sizeof h->count);
    memset(h->fast, 0, sizeof h->fast);
    for (int sym = 0; sym < n; sym++) {
        h->count[length[sym]]++;
    }
    if (h->count[0] == n) {
        return 0;
    }
    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) {
            return left;
        }
    }
    uint16_t offs[MAXBITS + 1];
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++) {
        offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    }
    for (int sym = 0; sym < n; sym++) {
        if (length[sym]) {
            h->symbol[offs[length[sym]]++] = (uint16_t)sym;
        }
    }
    /* Fast table: canonical codes of length <= FAST_BITS, bit-reversed since
     * the stream is read LSB first, replicated over the unused high bits. */
    uint32_t code = 0;
    int idx = 0;
    for (int len = 1; len <= FAST_BITS; len++) {
        for (int k = 0; k < h->count[len]; k++) {
            uint16_t sym = h->symbol[idx++];
            uint32_t rev = reverse(code++, len);
            for (uint32_t f = rev; f < (1u << FAST_BITS); f += 1u << len) {
                h->fast[f] = (uint16_t)((len << 9) | sym);
            }
        }
        code <<= 1;
    }
    return left;
}

static void flush(st_t *s)
{
    if (s->wpos && s->err == T5_OK) {
        t5_err_t e = s->wr(s->wctx, s->win, s->wpos);
        if (e != T5_OK) {
            set_err(s, e);
        }
    }
    s->wpos = 0;
}

static inline void put(st_t *s, uint8_t b)
{
    s->win[s->wpos++] = b;
    s->total++;
    if (s->wpos == WSIZE) {
        flush(s);
    }
}

static void stored(st_t *s)
{
    int drop = s->bitcnt & 7;      /* rest of the current byte */
    s->bitbuf >>= drop;
    s->bitcnt -= drop;
    uint32_t len = bits(s, 16);
    uint32_t nlen = bits(s, 16);
    if (s->err) {
        return;
    }
    if (len != (~nlen & 0xffff)) {
        set_err(s, T5_ERR_INVALID_ARG);
        return;
    }
    while (len && s->bitcnt >= 8) {    /* whole bytes already in the bit buffer */
        put(s, (uint8_t)bits(s, 8));
        len--;
    }
    while (len && s->err == T5_OK) {
        int b = next_byte(s);
        if (b < 0) {
            set_err(s, T5_ERR_INVALID_ARG);
            return;
        }
        put(s, (uint8_t)b);
        len--;
    }
}

static void codes(st_t *s)
{
    static const uint16_t lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                       35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                     3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                       6145, 8193, 12289, 16385, 24577};
    static const uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                     6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
        int sym = decode(s, &s->lencode);
        if (sym < 0 || s->err) {
            return;
        }
        if (sym < 256) {
            put(s, (uint8_t)sym);
            continue;
        }
        if (sym == 256) {
            return;
        }
        sym -= 257;
        if (sym >= 29) {
            set_err(s, T5_ERR_INVALID_ARG);
            return;
        }
        uint32_t len = lbase[sym] + bits(s, lext[sym]);
        int dsym = decode(s, &s->distcode);
        if (dsym < 0 || s->err) {
            return;
        }
        if (dsym >= 30) {
            set_err(s, T5_ERR_INVALID_ARG);
            return;
        }
        uint32_t dist = dbase[dsym] + bits(s, dext[dsym]);
        if (s->err) {
            return;
        }
        if (dist > s->total) {
            set_err(s, T5_ERR_INVALID_ARG);   /* reaches before the start of the output */
            return;
        }
        size_t from = (s->wpos + WSIZE - dist) & WMASK;
        while (len--) {
            uint8_t b = s->win[from];
            from = (from + 1) & WMASK;
            put(s, b);
        }
        if (s->err) {
            return;
        }
    }
}

static void fixed(st_t *s)
{
    uint8_t lengths[FIXLCODES];
    int sym = 0;
    for (; sym < 144; sym++) lengths[sym] = 8;
    for (; sym < 256; sym++) lengths[sym] = 9;
    for (; sym < 280; sym++) lengths[sym] = 7;
    for (; sym < FIXLCODES; sym++) lengths[sym] = 8;
    construct(&s->lencode, lengths, FIXLCODES);
    for (sym = 0; sym < MAXDCODES; sym++) lengths[sym] = 5;
    construct(&s->distcode, lengths, MAXDCODES);
    codes(s);
}

static void dynamic(st_t *s)
{
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint8_t lengths[MAXCODES];
    int nlen = (int)bits(s, 5) + 257;
    int ndist = (int)bits(s, 5) + 1;
    int ncode = (int)bits(s, 4) + 4;
    if (s->err) {
        return;
    }
    if (nlen > MAXLCODES || ndist > MAXDCODES) {
        set_err(s, T5_ERR_INVALID_ARG);
        return;
    }
    memset(lengths, 0, sizeof lengths);
    for (int i = 0; i < ncode; i++) {
        lengths[order[i]] = (uint8_t)bits(s, 3);
    }
    if (s->err) {
        return;
    }
    if (construct(&s->lencode, lengths, 19) != 0) {   /* code length code must be complete */
        set_err(s, T5_ERR_INVALID_ARG);
        return;
    }
    int index = 0;
    while (index < nlen + ndist) {
        int sym = decode(s, &s->lencode);
        if (sym < 0 || s->err) {
            set_err(s, T5_ERR_INVALID_ARG);
            return;
        }
        if (sym < 16) {
            lengths[index++] = (uint8_t)sym;
            continue;
        }
        uint8_t len = 0;
        int rep;
        if (sym == 16) {
            if (index == 0) {
                set_err(s, T5_ERR_INVALID_ARG);
                return;
            }
            len = lengths[index - 1];
            rep = 3 + (int)bits(s, 2);
        } else if (sym == 17) {
            rep = 3 + (int)bits(s, 3);
        } else {
            rep = 11 + (int)bits(s, 7);
        }
        if (s->err || index + rep > nlen + ndist) {
            set_err(s, T5_ERR_INVALID_ARG);
            return;
        }
        while (rep--) {
            lengths[index++] = len;
        }
    }
    if (lengths[256] == 0) {   /* no end-of-block code */
        set_err(s, T5_ERR_INVALID_ARG);
        return;
    }
    /* Incomplete codes are only allowed if they have a single code. */
    int err = construct(&s->lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - s->lencode.count[0] != 1)) {
        set_err(s, T5_ERR_INVALID_ARG);
        return;
    }
    err = construct(&s->distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - s->distcode.count[0] != 1)) {
        set_err(s, T5_ERR_INVALID_ARG);
        return;
    }
    codes(s);
}

t5_err_t t5_inflate_stream(t5_inflate_read_fn rd, void *rctx, t5_inflate_write_fn wr, void *wctx,
                           size_t *out_len)
{
    if (out_len) {
        *out_len = 0;
    }
    if (!rd || !wr) {
        return T5_ERR_INVALID_ARG;
    }
    st_t *s = calloc(1, sizeof *s);
    if (!s) {
        return T5_ERR_NO_MEM;
    }
    s->rd = rd;
    s->rctx = rctx;
    s->wr = wr;
    s->wctx = wctx;
    uint32_t last;
    do {
        last = bits(s, 1);
        uint32_t type = bits(s, 2);
        if (s->err) {
            break;
        }
        switch (type) {
        case 0: stored(s); break;
        case 1: fixed(s); break;
        case 2: dynamic(s); break;
        default: set_err(s, T5_ERR_INVALID_ARG); break;
        }
    } while (!last && s->err == T5_OK);
    flush(s);
    t5_err_t err = s->err;
    if (out_len) {
        *out_len = s->total;
    }
    free(s);
    return err;
}

typedef struct {
    const uint8_t *p;
    size_t left;
} mem_in_t;

typedef struct {
    uint8_t *p;
    size_t cap, len;
} mem_out_t;

static int mem_read(void *ctx, uint8_t *buf, size_t cap)
{
    mem_in_t *m = ctx;
    size_t n = m->left < cap ? m->left : cap;
    memcpy(buf, m->p, n);
    m->p += n;
    m->left -= n;
    return (int)n;
}

static t5_err_t mem_write(void *ctx, const uint8_t *data, size_t len)
{
    mem_out_t *m = ctx;
    if (len > m->cap - m->len) {
        return T5_ERR_NO_MEM;
    }
    memcpy(m->p + m->len, data, len);
    m->len += len;
    return T5_OK;
}

t5_err_t t5_inflate_mem(const void *src, size_t src_len, void *dst, size_t dst_cap, size_t *out_len)
{
    if (out_len) {
        *out_len = 0;
    }
    if ((!src && src_len) || (!dst && dst_cap)) {
        return T5_ERR_INVALID_ARG;
    }
    mem_in_t in = {src, src_len};
    mem_out_t out = {dst, dst_cap, 0};
    t5_err_t err = t5_inflate_stream(mem_read, &in, mem_write, &out, NULL);
    if (out_len) {
        *out_len = out.len;
    }
    return err;
}
