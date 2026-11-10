/* SPDX-License-Identifier: Apache-2.0
 * A small PNG decoder for app icons (the manifest's icon), built on the
 * package inflater so the OS carries one DEFLATE implementation.
 *
 * Every colour type and bit depth in the PNG spec is read (grey, RGB,
 * palette, grey with alpha, RGBA; 1 to 16 bits), with tRNS transparency.
 * Interlaced (Adam7) images are refused with T5_ERR_NOT_SUPPORTED: export
 * the icon without interlacing. CRCs aren't checked; malformed input
 * returns T5_ERR_INVALID_ARG and never reads or writes out of bounds.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The largest width or height decoded: bounds the memory a bad icon costs. */
#define T5_PNG_MAX_DIM 512

typedef struct {
    uint32_t w, h;
    uint8_t *rgba;   /* w * h pixels, 4 bytes each: R, G, B, A (straight alpha); free() it */
} t5_png_t;

/* Decode data into out. Errors: T5_ERR_INVALID_ARG not a PNG or malformed,
 * T5_ERR_NOT_SUPPORTED interlaced or larger than max_dim (at most
 * T5_PNG_MAX_DIM) on a side, T5_ERR_NO_MEM. */
T5_MUST_CHECK t5_err_t t5_png_decode(const void *data, size_t len, uint32_t max_dim, t5_png_t *out);

#ifdef __cplusplus
}
#endif
