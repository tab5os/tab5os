/* SPDX-License-Identifier: Apache-2.0
 * The AVI container for Motion JPEG recordings: one video stream ('00dc'
 * chunks, each a whole JPEG; an empty one repeats the previous frame), the
 * index (idx1) at the end. The header has a fixed size, so it is written
 * with zeros at the start and again with the real numbers at the end.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define AVI_HDR_LEN 224u                 /* RIFF, hdrl, and the movi list's own header */
#define AVI_CHUNK_HDR 8u
#define AVI_MAX_MOVI (1900u * 1024 * 1024)   /* stay clear of the 2 GB RIFF limit */

typedef struct {
    uint32_t off;      /* from the 'movi' fourcc, as idx1 counts */
    uint32_t len;      /* 0: a repeated frame */
} avi_idx_t;

typedef struct {
    uint16_t w, h;
    uint32_t us_per_frame;
    uint32_t frames;
    uint32_t movi_bytes;   /* chunks, headers and padding included */
    uint32_t max_chunk;
    uint32_t idx_entries;  /* 0 while recording */
} avi_info_t;

void avi_header(uint8_t out[AVI_HDR_LEN], const avi_info_t *info);
/* The chunk header for a frame of len bytes ('00dc', len); the chunk takes
 * AVI_CHUNK_HDR + len, plus one byte of padding when len is odd. */
void avi_chunk_header(uint8_t out[AVI_CHUNK_HDR], uint32_t len);
static inline uint32_t avi_chunk_bytes(uint32_t len)
{
    return AVI_CHUNK_HDR + len + (len & 1);
}
/* The idx1 chunk's header, then one 16-byte entry per frame. */
void avi_idx1_header(uint8_t out[8], uint32_t entries);
void avi_idx1_entry(uint8_t out[16], const avi_idx_t *e);
