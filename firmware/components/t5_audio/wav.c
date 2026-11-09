/* SPDX-License-Identifier: Apache-2.0
 * WAV parsing and conversion (t5_audio/wav.h).
 */
#include <math.h>
#include <string.h>
#include "t5_audio/wav.h"

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static t5_err_t parse_fmt(const uint8_t *c, uint32_t len, t5_wav_t *w)
{
    if (len < 16) return T5_ERR_INVALID_ARG;
    w->format = le16(c);
    w->channels = le16(c + 2);
    w->rate = le32(c + 4);
    w->block = le16(c + 12);
    w->bits = le16(c + 14);
    if (w->format == T5_WAV_EXTENSIBLE) {
        if (len < 40) return T5_ERR_INVALID_ARG;
        w->format = le16(c + 24);         /* the sub-format GUID's first two bytes */
    }
    if (w->format != T5_WAV_PCM && w->format != T5_WAV_FLOAT) return T5_ERR_NOT_SUPPORTED;
    bool pcm_ok = w->format == T5_WAV_PCM && (w->bits == 8 || w->bits == 16 || w->bits == 24 || w->bits == 32);
    bool flt_ok = w->format == T5_WAV_FLOAT && (w->bits == 32 || w->bits == 64);
    if (!pcm_ok && !flt_ok) return T5_ERR_NOT_SUPPORTED;
    if (!w->channels || w->channels > T5_WAV_MAX_CH || !w->rate) return T5_ERR_INVALID_ARG;
    if (w->block != w->channels * (w->bits / 8)) return T5_ERR_INVALID_ARG;
    return T5_OK;
}

t5_err_t t5_wav_parse(const uint8_t *buf, size_t n, t5_wav_t *out)
{
    memset(out, 0, sizeof *out);
    if (n < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) return T5_ERR_INVALID_ARG;
    bool fmt = false;
    size_t at = 12;
    /* Chunks: each id, size, body padded to even. At most n / 8 of them. */
    while (at + 8 <= n) {
        const uint8_t *c = buf + at;
        uint32_t len = le32(c + 4);
        if (!memcmp(c, "fmt ", 4)) {
            if (at + 8 + len > n) return T5_ERR_STATE;
            t5_err_t e = parse_fmt(c + 8, len, out);
            if (e != T5_OK) return e;
            fmt = true;
        } else if (!memcmp(c, "data", 4)) {
            if (!fmt) return T5_ERR_INVALID_ARG;
            out->data_off = (uint32_t)(at + 8);
            out->data_len = len - len % out->block;
            return T5_OK;
        }
        if (len >= n) return T5_ERR_STATE;       /* also keeps at from wrapping */
        at += 8 + (size_t)len + (len & 1u);
    }
    return T5_ERR_STATE;          /* "data" is further on: parse again with more of the file */
}

uint8_t t5_wav_out_channels(const t5_wav_t *w)
{
    return w->channels >= 2 ? 2 : 1;
}

static int16_t sample(const t5_wav_t *w, const uint8_t *p)
{
    if (w->format == T5_WAV_FLOAT) {
        double v;
        if (w->bits == 32) {
            float f;
            memcpy(&f, p, 4);
            v = f;
        } else {
            memcpy(&v, p, 8);
        }
        if (!(v == v)) v = 0;                    /* NaN */
        v = v * 32767.0;
        return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrint(v));
    }
    switch (w->bits) {
    case 8: return (int16_t)((p[0] - 128) << 8);
    case 16: return (int16_t)le16(p);
    case 24: return (int16_t)le16(p + 1);       /* the top 16 bits */
    default: return (int16_t)le16(p + 2);
    }
}

void t5_wav_convert(const t5_wav_t *w, const uint8_t *in, size_t frames, int16_t *out)
{
    uint8_t och = t5_wav_out_channels(w);
    size_t bps = w->bits / 8;
    for (size_t f = 0; f < frames; f++) {
        const uint8_t *fr = in + f * w->block;
        for (uint8_t c = 0; c < och; c++) out[f * och + c] = sample(w, fr + c * bps);
    }
}
