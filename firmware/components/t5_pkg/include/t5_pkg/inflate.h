/* SPDX-License-Identifier: Apache-2.0
 * Raw DEFLATE (RFC 1951) decoder. No zlib/gzip header handling: zip entries
 * carry raw streams. Memory is bounded: one ~38 KB state block (32 KB window,
 * 1 KB input buffer, decode tables), whatever the output size, so multi-MB
 * zip entries can be streamed straight to a file.
 *
 * Malformed input returns T5_ERR_INVALID_ARG; it never reads or writes out of
 * bounds and never loops forever.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Input source: copy up to cap bytes into buf. Return the number of bytes
 * (0 = end of input) or a negative value on an I/O error. */
typedef int (*t5_inflate_read_fn)(void *ctx, uint8_t *buf, size_t cap);
/* Output sink: consume len bytes. Return T5_OK to continue; anything else
 * aborts decoding and is returned from t5_inflate_stream(). */
typedef t5_err_t (*t5_inflate_write_fn)(void *ctx, const uint8_t *data, size_t len);

/* Decode one raw DEFLATE stream until its final block.
 * Errors: T5_ERR_INVALID_ARG malformed or truncated stream, T5_ERR_IO read
 * failure, T5_ERR_NO_MEM state allocation, or the sink's own error.
 * out_len (optional) receives the number of bytes produced. */
t5_err_t t5_inflate_stream(t5_inflate_read_fn rd, void *rctx, t5_inflate_write_fn wr, void *wctx,
                           size_t *out_len);

/* Decode src into dst. T5_ERR_NO_MEM if the output does not fit in dst_cap. */
t5_err_t t5_inflate_mem(const void *src, size_t src_len, void *dst, size_t dst_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif
