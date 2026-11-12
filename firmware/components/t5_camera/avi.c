/* SPDX-License-Identifier: Apache-2.0
 * AVI writing for Motion JPEG recordings (avi.h).
 */
#include <string.h>
#include "avi.h"

#define AVIF_HASINDEX 0x10u
#define AVIIF_KEYFRAME 0x10u

typedef struct {
    uint8_t *p;
} wr_t;

static void put4cc(wr_t *w, const char *cc)
{
    memcpy(w->p, cc, 4);
    w->p += 4;
}

static void put32(wr_t *w, uint32_t v)
{
    w->p[0] = (uint8_t)v;
    w->p[1] = (uint8_t)(v >> 8);
    w->p[2] = (uint8_t)(v >> 16);
    w->p[3] = (uint8_t)(v >> 24);
    w->p += 4;
}

static void put16(wr_t *w, uint16_t v)
{
    w->p[0] = (uint8_t)v;
    w->p[1] = (uint8_t)(v >> 8);
    w->p += 2;
}

void avi_header(uint8_t out[AVI_HDR_LEN], const avi_info_t *in)
{
    memset(out, 0, AVI_HDR_LEN);
    wr_t w = { out };
    uint32_t idx1 = in->idx_entries ? 8 + in->idx_entries * 16 : 0;
    uint32_t us = in->us_per_frame ? in->us_per_frame : 33333;
    put4cc(&w, "RIFF");
    put32(&w, AVI_HDR_LEN - 8 + in->movi_bytes + idx1);
    put4cc(&w, "AVI ");
    put4cc(&w, "LIST");
    put32(&w, 4 + 64 + 124);
    put4cc(&w, "hdrl");
    /* avih */
    put4cc(&w, "avih");
    put32(&w, 56);
    put32(&w, us);
    put32(&w, (uint32_t)((uint64_t)in->max_chunk * 1000000u / us));
    put32(&w, 0);
    put32(&w, in->idx_entries ? AVIF_HASINDEX : 0);
    put32(&w, in->frames);
    put32(&w, 0);
    put32(&w, 1);
    put32(&w, in->max_chunk);
    put32(&w, in->w);
    put32(&w, in->h);
    for (int i = 0; i < 4; i++) put32(&w, 0);
    /* strl: strh + strf */
    put4cc(&w, "LIST");
    put32(&w, 4 + 64 + 48);
    put4cc(&w, "strl");
    put4cc(&w, "strh");
    put32(&w, 56);
    put4cc(&w, "vids");
    put4cc(&w, "MJPG");
    put32(&w, 0);                 /* flags */
    put16(&w, 0);                 /* priority */
    put16(&w, 0);                 /* language */
    put32(&w, 0);                 /* initial frames */
    put32(&w, us);                /* scale / rate = seconds per frame */
    put32(&w, 1000000);
    put32(&w, 0);                 /* start */
    put32(&w, in->frames);
    put32(&w, in->max_chunk);
    put32(&w, 0xFFFFFFFFu);       /* quality: default */
    put32(&w, 0);                 /* sample size: varies */
    put16(&w, 0);
    put16(&w, 0);
    put16(&w, in->w);
    put16(&w, in->h);
    put4cc(&w, "strf");
    put32(&w, 40);
    put32(&w, 40);
    put32(&w, in->w);
    put32(&w, in->h);
    put16(&w, 1);
    put16(&w, 24);
    put4cc(&w, "MJPG");
    put32(&w, (uint32_t)in->w * in->h * 3);
    for (int i = 0; i < 4; i++) put32(&w, 0);
    /* movi */
    put4cc(&w, "LIST");
    put32(&w, 4 + in->movi_bytes);
    put4cc(&w, "movi");
}

void avi_chunk_header(uint8_t out[AVI_CHUNK_HDR], uint32_t len)
{
    wr_t w = { out };
    put4cc(&w, "00dc");
    put32(&w, len);
}

void avi_idx1_header(uint8_t out[8], uint32_t entries)
{
    wr_t w = { out };
    put4cc(&w, "idx1");
    put32(&w, entries * 16);
}

void avi_idx1_entry(uint8_t out[16], const avi_idx_t *e)
{
    wr_t w = { out };
    put4cc(&w, "00dc");
    put32(&w, e->len ? AVIIF_KEYFRAME : 0);
    put32(&w, e->off);
    put32(&w, e->len);
}
