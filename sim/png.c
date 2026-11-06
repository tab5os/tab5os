/* SPDX-License-Identifier: Apache-2.0
 * Minimal PNG writer for screenshots: RGB8, zlib stream of stored blocks. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim.h"

static uint32_t crc_table[256];

static uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crc_table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    }
    c = ~c;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t h[8];
    be32(h, len);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    uint32_t c = crc(crc(0, (const uint8_t *)type, 4), data, len);
    be32(h, c);
    fwrite(h, 1, 4, f);
}

int sim_png_write(const char *path, const uint16_t *px, int w, int h, int stride)
{
    size_t row = (size_t)w * 3 + 1, raw_len = row * (size_t)h;
    uint8_t *raw = malloc(raw_len);
    if (!raw) return -1;
    for (int y = 0; y < h; y++) {
        uint8_t *o = raw + (size_t)y * row;
        *o++ = 0;
        for (int x = 0; x < w; x++) {
            uint16_t c = px[(size_t)y * (size_t)stride + (size_t)x];
            uint8_t r = (uint8_t)((c >> 11) & 0x1F), g = (uint8_t)((c >> 5) & 0x3F), b = (uint8_t)(c & 0x1F);
            *o++ = (uint8_t)(r << 3 | r >> 2);
            *o++ = (uint8_t)(g << 2 | g >> 4);
            *o++ = (uint8_t)(b << 3 | b >> 2);
        }
    }
    size_t nblk = (raw_len + 65534) / 65535;
    size_t zlen = 2 + raw_len + nblk * 5 + 4;
    uint8_t *z = malloc(zlen), *p = z;
    *p++ = 0x78; *p++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t off = 0; off < raw_len; off += 65535) {
        size_t n = raw_len - off < 65535 ? raw_len - off : 65535;
        *p++ = off + n >= raw_len;
        *p++ = (uint8_t)n; *p++ = (uint8_t)(n >> 8); *p++ = (uint8_t)~n; *p++ = (uint8_t)(~n >> 8);
        memcpy(p, raw + off, n);
        p += n;
    }
    for (size_t i = 0; i < raw_len; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    be32(p, b << 16 | a);
    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return -1; }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w); be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)zlen);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
    return 0;
}

uint64_t sim_hash(const uint16_t *px, int w, int h, int stride)
{
    uint64_t v = 1469598103934665603ull;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            v ^= px[(size_t)y * (size_t)stride + (size_t)x];
            v *= 1099511628211ull;
        }
    }
    return v;
}
