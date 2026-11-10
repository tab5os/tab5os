/* SPDX-License-Identifier: Apache-2.0
 * PNG decoding for app icons (t5_pkg/png.h). The IDAT chunks are fed
 * straight to the package inflater, so there is no second copy of the
 * compressed data and no zlib in the image.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "t5_pkg/inflate.h"
#include "t5_pkg/png.h"

static const uint8_t SIG[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };

enum { CT_GREY = 0, CT_RGB = 2, CT_PALETTE = 3, CT_GREY_ALPHA = 4, CT_RGBA = 6 };

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

/* Walks the chunks after the signature. False at the end, at IEND or on a
 * chunk that runs past the data (then *bad). */
typedef struct {
    const uint8_t *data;
    size_t len, pos;
    bool bad;
} chunks_t;

static bool next_chunk(chunks_t *c, const uint8_t **type, const uint8_t **body, uint32_t *n)
{
    if (c->pos >= c->len) return false;
    if (c->len - c->pos < 12) {
        c->bad = true;
        return false;
    }
    *n = be32(c->data + c->pos);
    if (*n > c->len - c->pos - 12) {
        c->bad = true;
        return false;
    }
    *type = c->data + c->pos + 4;
    *body = c->data + c->pos + 8;
    c->pos += 12 + (size_t)*n;
    return memcmp(*type, "IEND", 4) != 0;
}

/* The inflater's input: the IDAT chunks' bodies, one after the other, less
 * the two-byte zlib header. The Adler-32 trailer is never asked for: the
 * inflater stops at the final block. */
typedef struct {
    chunks_t c;
    const uint8_t *cur;
    size_t left;
} idat_t;

static int idat_read(void *ctx, uint8_t *buf, size_t cap)
{
    idat_t *in = ctx;
    size_t got = 0;
    while (got < cap) {
        if (!in->left) {
            const uint8_t *type, *body;
            uint32_t n;
            bool found = false;
            while (next_chunk(&in->c, &type, &body, &n)) {
                if (!memcmp(type, "IDAT", 4)) {
                    found = true;
                    break;
                }
            }
            if (in->c.bad) return -1;
            if (!found) break;
            in->cur = body;
            in->left = n;
            continue;
        }
        size_t k = in->left < cap - got ? in->left : cap - got;
        memcpy(buf + got, in->cur, k);
        in->cur += k;
        in->left -= k;
        got += k;
    }
    return (int)got;
}

typedef struct {
    uint8_t *p;
    size_t cap, len;
} raw_t;

static t5_err_t raw_write(void *ctx, const uint8_t *data, size_t len)
{
    raw_t *r = ctx;
    if (len > r->cap - r->len) return T5_ERR_INVALID_ARG;   /* more scanlines than the header says */
    memcpy(r->p + r->len, data, len);
    r->len += len;
    return T5_OK;
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

/* Undo each scanline's filter in place. Rows are stride bytes after their
 * filter-type byte; bpp is the byte distance to the pixel on the left. */
static bool unfilter(uint8_t *raw, uint32_t h, size_t stride, size_t bpp)
{
    const uint8_t *prev = NULL;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * (stride + 1);
        uint8_t *c = row + 1;
        switch (row[0]) {
        case 0:
            break;
        case 1:
            for (size_t i = bpp; i < stride; i++) c[i] = (uint8_t)(c[i] + c[i - bpp]);
            break;
        case 2:
            if (prev) {
                for (size_t i = 0; i < stride; i++) c[i] = (uint8_t)(c[i] + prev[i]);
            }
            break;
        case 3:
            for (size_t i = 0; i < stride; i++) {
                int a = i >= bpp ? c[i - bpp] : 0, b = prev ? prev[i] : 0;
                c[i] = (uint8_t)(c[i] + ((a + b) >> 1));
            }
            break;
        case 4:
            for (size_t i = 0; i < stride; i++) {
                int a = i >= bpp ? c[i - bpp] : 0, b = prev ? prev[i] : 0;
                int d = prev && i >= bpp ? prev[i - bpp] : 0;
                c[i] = (uint8_t)(c[i] + paeth(a, b, d));
            }
            break;
        default:
            return false;
        }
        prev = c;
    }
    return true;
}

/* The i-th sample of a row, at its own bit depth. */
static uint32_t sample(const uint8_t *row, size_t i, int depth)
{
    if (depth == 16) return be16(row + 2 * i);
    if (depth == 8) return row[i];
    size_t bit = i * (size_t)depth;
    int shift = 8 - depth - (int)(bit & 7);
    return (uint32_t)(row[bit >> 3] >> shift) & ((1u << depth) - 1);
}

/* A sample scaled to 8 bits. */
static uint8_t to8(uint32_t v, int depth)
{
    if (depth == 16) return (uint8_t)(v >> 8);
    if (depth == 8) return (uint8_t)v;
    return (uint8_t)(v * 255u / ((1u << depth) - 1));
}

static bool depth_ok(int ct, int depth)
{
    switch (ct) {
    case CT_GREY: return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case CT_PALETTE: return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    case CT_RGB:
    case CT_GREY_ALPHA:
    case CT_RGBA: return depth == 8 || depth == 16;
    default: return false;
    }
}

static int channels(int ct)
{
    switch (ct) {
    case CT_RGB: return 3;
    case CT_GREY_ALPHA: return 2;
    case CT_RGBA: return 4;
    default: return 1;
    }
}

t5_err_t t5_png_decode(const void *data, size_t len, uint32_t max_dim, t5_png_t *out)
{
    if (!out) return T5_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    if (!data || len < sizeof SIG + 25 || memcmp(data, SIG, sizeof SIG)) return T5_ERR_INVALID_ARG;
    if (!max_dim || max_dim > T5_PNG_MAX_DIM) max_dim = T5_PNG_MAX_DIM;

    chunks_t c = { .data = data, .len = len, .pos = sizeof SIG };
    const uint8_t *type, *body;
    uint32_t n;
    if (!next_chunk(&c, &type, &body, &n) || memcmp(type, "IHDR", 4) || n != 13) return T5_ERR_INVALID_ARG;
    uint32_t w = be32(body), h = be32(body + 4);
    int depth = body[8], ct = body[9];
    if (!w || !h || w > 0x7fffffffu || h > 0x7fffffffu) return T5_ERR_INVALID_ARG;
    if (!depth_ok(ct, depth) || body[10] != 0 || body[11] != 0 || body[12] > 1) return T5_ERR_INVALID_ARG;
    if (body[12] == 1 || w > max_dim || h > max_dim) return T5_ERR_NOT_SUPPORTED;

    /* The chunks that matter before the pixels: the palette and tRNS. */
    uint8_t pal[256][3], trns[256];
    int npal = 0, ntrns = 0;
    uint16_t key[3];
    bool has_key = false, has_idat = false;
    while (next_chunk(&c, &type, &body, &n)) {
        if (!memcmp(type, "PLTE", 4)) {
            if (n % 3 || n > sizeof pal || !n) return T5_ERR_INVALID_ARG;
            npal = (int)(n / 3);
            memcpy(pal, body, n);
        } else if (!memcmp(type, "tRNS", 4)) {
            if (ct == CT_PALETTE) {
                if (n > sizeof trns) return T5_ERR_INVALID_ARG;
                ntrns = (int)n;
                memcpy(trns, body, n);
            } else if ((ct == CT_GREY && n == 2) || (ct == CT_RGB && n == 6)) {
                for (uint32_t i = 0; i < n / 2; i++) key[i] = be16(body + 2 * i);
                has_key = true;
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            has_idat = true;
        }
    }
    if (c.bad || !has_idat || (ct == CT_PALETTE && !npal)) return T5_ERR_INVALID_ARG;

    int ch = channels(ct);
    size_t stride = ((size_t)w * (size_t)ch * (size_t)depth + 7) / 8;
    size_t bpp = (size_t)ch * (size_t)depth / 8;
    if (!bpp) bpp = 1;
    raw_t raw = { .cap = (size_t)h * (stride + 1) };
    raw.p = malloc(raw.cap);
    if (!raw.p) return T5_ERR_NO_MEM;

    idat_t in = { .c = { .data = data, .len = len, .pos = sizeof SIG } };
    uint8_t zh[2];
    t5_err_t e = T5_OK;
    /* zlib header: deflate, no preset dictionary, and its check bits. */
    if (idat_read(&in, zh, 2) != 2 || (zh[0] & 0x0f) != 8 || (zh[0] >> 4) > 7 || (zh[1] & 0x20) ||
        ((zh[0] << 8) | zh[1]) % 31) {
        e = T5_ERR_INVALID_ARG;
    }
    if (e == T5_OK) {
        e = t5_inflate_stream(idat_read, &in, raw_write, &raw, NULL);
        if (e == T5_ERR_IO) e = T5_ERR_INVALID_ARG;   /* a chunk ran past the end of the data */
    }
    if (e == T5_OK && (raw.len != raw.cap || !unfilter(raw.p, h, stride, bpp))) e = T5_ERR_INVALID_ARG;
    uint8_t *px = e == T5_OK ? malloc((size_t)w * h * 4) : NULL;
    if (e == T5_OK && !px) e = T5_ERR_NO_MEM;
    if (e != T5_OK) {
        free(raw.p);
        return e;
    }

    uint8_t *o = px;
    for (uint32_t y = 0; y < h && e == T5_OK; y++) {
        const uint8_t *row = raw.p + (size_t)y * (stride + 1) + 1;
        for (uint32_t x = 0; x < w; x++, o += 4) {
            uint32_t s0, s1, s2;
            switch (ct) {
            case CT_GREY:
                s0 = sample(row, x, depth);
                o[0] = o[1] = o[2] = to8(s0, depth);
                o[3] = has_key && s0 == key[0] ? 0 : 255;
                break;
            case CT_RGB:
                s0 = sample(row, 3 * (size_t)x, depth);
                s1 = sample(row, 3 * (size_t)x + 1, depth);
                s2 = sample(row, 3 * (size_t)x + 2, depth);
                o[0] = to8(s0, depth);
                o[1] = to8(s1, depth);
                o[2] = to8(s2, depth);
                o[3] = has_key && s0 == key[0] && s1 == key[1] && s2 == key[2] ? 0 : 255;
                break;
            case CT_PALETTE:
                s0 = sample(row, x, depth);
                if ((int)s0 >= npal) {
                    e = T5_ERR_INVALID_ARG;
                    break;
                }
                memcpy(o, pal[s0], 3);
                o[3] = (int)s0 < ntrns ? trns[s0] : 255;
                break;
            case CT_GREY_ALPHA:
                o[0] = o[1] = o[2] = to8(sample(row, 2 * (size_t)x, depth), depth);
                o[3] = to8(sample(row, 2 * (size_t)x + 1, depth), depth);
                break;
            default: /* CT_RGBA */
                for (int k = 0; k < 4; k++) o[k] = to8(sample(row, 4 * (size_t)x + (size_t)k, depth), depth);
                break;
            }
            if (e != T5_OK) break;
        }
    }
    free(raw.p);
    if (e != T5_OK) {
        free(px);
        return e;
    }
    out->w = w;
    out->h = h;
    out->rgba = px;
    return T5_OK;
}
