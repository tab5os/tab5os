/* SPDX-License-Identifier: Apache-2.0
 * ESP app image header parsing for the C6 image in the c6fw partition.
 * Pure: reads go through a callback, so host tests feed it byte arrays.
 */
#include <stdbool.h>
#include <string.h>
#include "t5_bootfmt/espimg.h"
#include "c6img.h"

#define MAX_SEGMENTS 16

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

t5_err_t t5_c6img_parse(t5_c6img_read_t rd, void *ctx, uint32_t part_size, t5_c6img_info_t *out)
{
    memset(out, 0, sizeof *out);
    uint8_t h[T5_IMG_DESC_OFF + T5_IMG_DESC_VERSION_OFF + T5_IMG_DESC_VERSION_LEN];
    if (part_size < sizeof h || rd(ctx, 0, h, sizeof h) != T5_OK) return T5_ERR_IO;
    if (h[0] != T5_IMG_MAGIC) return T5_ERR_NOT_FOUND;   /* erased (0xFF) or not an image */
    uint8_t nseg = h[T5_IMG_SEGMENTS_OFF];
    if (nseg == 0 || nseg > MAX_SEGMENTS) return T5_ERR_INVALID_ARG;
    out->chip_id = (uint16_t)(h[T5_IMG_CHIP_ID_OFF] | h[T5_IMG_CHIP_ID_OFF + 1] << 8);
    bool hash_appended = h[T5_IMG_HASH_APPENDED_OFF] == 1;
    const uint8_t *d = h + T5_IMG_DESC_OFF;
    if (le32(d) != T5_IMG_DESC_MAGIC) return T5_ERR_INVALID_ARG;
    _Static_assert(sizeof out->version == T5_IMG_DESC_VERSION_LEN, "version");
    memcpy(out->version, d + T5_IMG_DESC_VERSION_OFF, sizeof out->version - 1);
    out->version[sizeof out->version - 1] = 0;

    uint32_t off = T5_IMG_HDR_LEN;
    for (int i = 0; i < nseg; i++) {
        uint8_t sh[T5_IMG_SEG_HDR_LEN];
        if (off + T5_IMG_SEG_HDR_LEN > part_size || rd(ctx, off, sh, sizeof sh) != T5_OK) return T5_ERR_IO;
        uint32_t len = le32(sh + 4);
        if (len > part_size) return T5_ERR_INVALID_ARG;
        off += T5_IMG_SEG_HDR_LEN + len;
        if (off > part_size) return T5_ERR_INVALID_ARG;
    }
    /* Checksum byte sits at the end of a 16-byte padded block. */
    off = (off + 16) & ~15u;
    if (hash_appended) off += 32;
    if (off > part_size) return T5_ERR_INVALID_ARG;
    out->size = off;
    return T5_OK;
}
