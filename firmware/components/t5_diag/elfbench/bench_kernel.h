/* SPDX-License-Identifier: Apache-2.0
 * Benchmark kernels for bench check 11 (ELF code in PSRAM). Compiled twice:
 * into the firmware (runs from flash via cache) and into elfbench.elf
 * (loaded into PSRAM by elf_loader). Same source, same flags.
 */
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t mode;     /* 1 small, 2 big */
    uint32_t iters;
    uint32_t result;
} elfbench_arg_t;

/* Small: fits in cache. 16x16 integer matrix multiply plus a PRNG. */
static inline __attribute__((always_inline)) uint32_t bench_small(uint32_t iters)
{
    int32_t a[16][16], b[16][16], c[16][16];
    uint32_t s = 0x12345678;
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 16; j++) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            a[i][j] = (int32_t)(s & 0xFF);
            b[i][j] = (int32_t)((s >> 8) & 0xFF);
        }
    }
    uint32_t sum = 0;
    for (uint32_t n = 0; n < iters; n++) {
        for (int i = 0; i < 16; i++) {
            for (int j = 0; j < 16; j++) {
                int32_t acc = 0;
                for (int k = 0; k < 16; k++) acc += a[i][k] * b[k][j];
                c[i][j] = acc;
            }
        }
        a[n & 15][(n >> 4) & 15] ^= c[(n * 7) & 15][(n * 3) & 15];
        sum += (uint32_t)c[n & 15][(n >> 4) & 15];
    }
    return sum;
}

/* Big: ~64 KB of straight-line code, so every pass misses the I-cache. */
#define BK_OP(x, k) x = (x ^ (x << 7) ^ (uint32_t)(k)) * 2654435761u;
#define BK_R4(x, k) BK_OP(x, k) BK_OP(x, (k) + 1) BK_OP(x, (k) + 2) BK_OP(x, (k) + 3)
#define BK_R16(x, k) BK_R4(x, k) BK_R4(x, (k) + 4) BK_R4(x, (k) + 8) BK_R4(x, (k) + 12)
#define BK_R64(x, k) BK_R16(x, k) BK_R16(x, (k) + 16) BK_R16(x, (k) + 32) BK_R16(x, (k) + 48)
#define BK_R256(x, k) BK_R64(x, k) BK_R64(x, (k) + 64) BK_R64(x, (k) + 128) BK_R64(x, (k) + 192)
#define BK_R1K(x, k) BK_R256(x, k) BK_R256(x, (k) + 256) BK_R256(x, (k) + 512) BK_R256(x, (k) + 768)

static __attribute__((noinline)) uint32_t bench_big_body(uint32_t x)
{
    BK_R1K(x, 0x1000) BK_R1K(x, 0x2000) BK_R1K(x, 0x3000)
    return x;
}

static inline __attribute__((always_inline)) uint32_t bench_big(uint32_t iters)
{
    uint32_t x = 1;
    for (uint32_t n = 0; n < iters; n++) x = bench_big_body(x + n);
    return x;
}
