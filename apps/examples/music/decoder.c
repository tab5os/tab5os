/* Decoders (decoder.h). */
#include "decoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <t5/audio.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "third_party/minimp3/minimp3.h"

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_WCHAR
#include "third_party/dr_flac/dr_flac.h"

/* GCC's helper for a 64-bit signed shift right by a variable amount, which
 * dr_flac needs on RV32. The OS doesn't export it, and an app is linked
 * without libgcc, so here it is. */
long long __ashrdi3(long long a, int b);
long long __ashrdi3(long long a, int b)
{
    if (b <= 0) return a;
    if (b >= 64) return a < 0 ? -1 : 0;
    uint32_t lo = (uint32_t)a, hi = (uint32_t)((unsigned long long)a >> 32);
    int32_t shi = (int32_t)hi;
    if (b >= 32) {
        lo = (uint32_t)(shi >> (b - 32));
        hi = (uint32_t)(shi < 0 ? -1 : 0);
    } else {
        lo = (lo >> b) | (hi << (32 - b));
        hi = (uint32_t)(shi >> b);
    }
    return (long long)((unsigned long long)hi << 32 | lo);
}

#define MP3_BUF 16384
#define MAX_RATE 48000
#define SRC_FRAMES 1152            /* a native-rate block */

struct dec {
    track_fmt_t fmt;
    FILE *f;
    uint32_t rate, src_rate;       /* out, native */
    uint8_t ch, src_ch;            /* out (1, 2), native */
    int decim;                     /* src_rate / rate */
    uint64_t pos;                  /* frames given out, at the out rate */
    uint32_t duration_ms;
    /* native PCM waiting to be given out */
    int16_t *pend;
    size_t npend, pend_at;
    /* MP3 */
    mp3dec_t *mp3;
    uint8_t *buf;
    size_t blen, bat;
    uint32_t audio_off, audio_end;
    uint8_t toc[100];
    bool has_toc;
    /* FLAC */
    drflac *flac;
    int16_t *fbuf;                 /* the file's channels, before taking two */
    /* WAV */
    t5_audio_dec_t *wav;
};

/* ---- MP3 ------------------------------------------------------------------------- */
static void mp3_fill(dec_t *d)
{
    if (d->bat) {
        memmove(d->buf, d->buf + d->bat, d->blen - d->bat);
        d->blen -= d->bat;
        d->bat = 0;
    }
    long here = ftell(d->f);
    size_t want = MP3_BUF - d->blen;
    if (here >= 0 && (uint32_t)here + want > d->audio_end) want = here < (long)d->audio_end ? d->audio_end - (uint32_t)here : 0;
    if (want) d->blen += fread(d->buf + d->blen, 1, want, d->f);
}

/* One MP3 frame into pend. False at the end. */
static bool mp3_frame(dec_t *d)
{
    static const int GIVE_UP = 16;
    for (int tries = 0; tries < GIVE_UP; tries++) {
        if (d->blen - d->bat < MP3_BUF / 2) mp3_fill(d);
        if (d->blen == d->bat) return false;
        mp3dec_frame_info_t fi;
        int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        int n = mp3dec_decode_frame(d->mp3, d->buf + d->bat, (int)(d->blen - d->bat), pcm, &fi);
        if (!fi.frame_bytes) {                         /* no frame in what we have */
            long here = ftell(d->f);
            if (feof(d->f) || here < 0 || (uint32_t)here >= d->audio_end) return false;
            size_t keep = d->blen - d->bat > 3 ? 3 : d->blen - d->bat;   /* a header may straddle */
            d->bat = d->blen - keep;
            continue;
        }
        d->bat += (size_t)fi.frame_bytes;
        if (!n) continue;                              /* a Xing frame, or the bit reservoir filling */
        for (int i = 0; i < n; i++) {
            int16_t l = pcm[i * fi.channels], r = fi.channels > 1 ? pcm[i * fi.channels + 1] : l;
            d->pend[i * d->src_ch] = l;
            if (d->src_ch == 2) d->pend[i * 2 + 1] = r;
        }
        d->npend = (size_t)n;
        d->pend_at = 0;
        return true;
    }
    return false;
}

static void mp3_read_toc(dec_t *d)
{
    uint8_t b[192];
    if (fseek(d->f, (long)d->audio_off, SEEK_SET) || fread(b, 1, sizeof b, d->f) < sizeof b) return;
    for (size_t x = 4; x + 120 <= sizeof b; x++) {
        if (memcmp(b + x, "Xing", 4) && memcmp(b + x, "Info", 4)) continue;
        uint32_t flags = (uint32_t)b[x + 4] << 24 | (uint32_t)b[x + 5] << 16 | (uint32_t)b[x + 6] << 8 | b[x + 7];
        size_t at = x + 8 + ((flags & 1) ? 4 : 0) + ((flags & 2) ? 4 : 0);
        if ((flags & 4) && at + 100 <= sizeof b) {
            memcpy(d->toc, b + at, 100);
            d->has_toc = true;
        }
        return;
    }
}

static bool mp3_open(dec_t *d, const tags_t *t)
{
    d->mp3 = malloc(sizeof *d->mp3);
    d->buf = malloc(MP3_BUF);
    if (!d->mp3 || !d->buf) return false;
    mp3dec_init(d->mp3);
    d->audio_off = t->audio_off;
    d->audio_end = t->audio_end ? t->audio_end : UINT32_MAX;
    d->src_rate = t->rate;
    d->src_ch = t->channels == 1 ? 1 : 2;
    mp3_read_toc(d);
    return fseek(d->f, (long)d->audio_off, SEEK_SET) == 0;
}

static bool mp3_seek(dec_t *d, uint32_t ms)
{
    if (!d->duration_ms) return false;
    if (ms >= d->duration_ms) ms = d->duration_ms - 1;
    uint32_t span = d->audio_end - d->audio_off;
    uint32_t off;
    if (d->has_toc) {                                   /* percent -> 1/256 of the file */
        float pct = (float)ms * 100.0f / (float)d->duration_ms;
        int a = (int)pct;
        float fa = d->toc[a], fb = a < 99 ? d->toc[a + 1] : 256.0f;
        off = (uint32_t)((fa + (fb - fa) * (pct - (float)a)) / 256.0f * (float)span);
    } else {
        off = (uint32_t)((uint64_t)span * ms / d->duration_ms);
    }
    if (fseek(d->f, (long)(d->audio_off + off), SEEK_SET)) return false;
    mp3dec_init(d->mp3);
    d->blen = d->bat = 0;
    return true;
}

/* ---- FLAC ------------------------------------------------------------------------ */
static size_t fl_read(void *u, void *buf, size_t n)
{
    return fread(buf, 1, n, ((dec_t *)u)->f);
}

static drflac_bool32 fl_seek(void *u, int off, drflac_seek_origin o)
{
    int w = o == DRFLAC_SEEK_SET ? SEEK_SET : o == DRFLAC_SEEK_CUR ? SEEK_CUR : SEEK_END;
    return fseek(((dec_t *)u)->f, off, w) == 0;
}

static drflac_bool32 fl_tell(void *u, drflac_int64 *at)
{
    long p = ftell(((dec_t *)u)->f);
    if (p < 0) return DRFLAC_FALSE;
    *at = p;
    return DRFLAC_TRUE;
}

static bool flac_frames(dec_t *d)
{
    drflac_uint64 n = drflac_read_pcm_frames_s16(d->flac, SRC_FRAMES, d->fbuf);
    if (!n) return false;
    unsigned c = d->flac->channels;
    for (drflac_uint64 i = 0; i < n; i++) {
        d->pend[i * d->src_ch] = d->fbuf[i * c];
        if (d->src_ch == 2) d->pend[i * 2 + 1] = d->fbuf[i * c + (c > 1 ? 1 : 0)];
    }
    d->npend = (size_t)n;
    d->pend_at = 0;
    return true;
}

/* ---- WAV ------------------------------------------------------------------------- */
static bool wav_frames(dec_t *d)
{
    int n = t5_audio_dec_read(d->wav, d->pend, SRC_FRAMES);
    if (n <= 0) return false;
    d->npend = (size_t)n;
    d->pend_at = 0;
    return true;
}

/* ---- common ---------------------------------------------------------------------- */
dec_t *dec_open(const char *path, const tags_t *t)
{
    dec_t *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->fmt = t->fmt;
    d->duration_ms = t->duration_ms;
    bool ok = false;
    if (t->fmt == TRACK_WAV) {
        t5_audio_info_t in;
        if (t5_audio_dec_open(path, &d->wav) == T5_OK && t5_audio_dec_info(d->wav, &in) == T5_OK) {
            d->src_rate = in.rate;
            d->src_ch = in.channels;
            if (!d->duration_ms) d->duration_ms = in.duration_ms;
            ok = true;
        }
    } else if ((d->f = fopen(path, "rb"))) {
        if (t->fmt == TRACK_MP3) {
            ok = mp3_open(d, t);
        } else if (t->fmt == TRACK_FLAC) {
            d->flac = drflac_open(fl_read, fl_seek, fl_tell, d, NULL);
            if (d->flac) {
                d->src_rate = d->flac->sampleRate;
                d->src_ch = d->flac->channels > 1 ? 2 : 1;
                d->fbuf = malloc(SRC_FRAMES * d->flac->channels * sizeof(int16_t));
                if (!d->duration_ms && d->src_rate) d->duration_ms = (uint32_t)(d->flac->totalPCMFrameCount * 1000 / d->src_rate);
                ok = d->fbuf != NULL;
            }
        }
    }
    d->pend = malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t) > SRC_FRAMES * 2 * sizeof(int16_t)
                         ? MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t)
                         : SRC_FRAMES * 2 * sizeof(int16_t));
    if (!ok || !d->pend || !d->src_rate) {
        dec_close(d);
        return NULL;
    }
    d->decim = 1;
    while (d->src_rate / (uint32_t)d->decim > MAX_RATE) d->decim *= 2;
    d->rate = d->src_rate / (uint32_t)d->decim;
    d->ch = d->src_ch;
    return d;
}

uint32_t dec_rate(const dec_t *d) { return d->rate; }
uint8_t dec_channels(const dec_t *d) { return d->ch; }

static bool refill(dec_t *d)
{
    switch (d->fmt) {
    case TRACK_MP3: return mp3_frame(d);
    case TRACK_FLAC: return flac_frames(d);
    case TRACK_WAV: return wav_frames(d);
    default: return false;
    }
}

int dec_read(dec_t *d, int16_t *pcm, size_t frames)
{
    size_t done = 0;
    while (done < frames) {
        if (d->npend - d->pend_at < (size_t)d->decim) {
            if (!refill(d)) break;
        }
        /* average decim native frames into one */
        while (done < frames && d->npend - d->pend_at >= (size_t)d->decim) {
            for (int c = 0; c < d->ch; c++) {
                int32_t acc = 0;
                for (int k = 0; k < d->decim; k++) acc += d->pend[(d->pend_at + (size_t)k) * d->src_ch + (size_t)c];
                pcm[done * d->ch + (size_t)c] = (int16_t)(acc / d->decim);
            }
            d->pend_at += (size_t)d->decim;
            done++;
        }
    }
    d->pos += done;
    return (int)done;
}

bool dec_seek(dec_t *d, uint32_t ms)
{
    bool ok;
    switch (d->fmt) {
    case TRACK_MP3: ok = mp3_seek(d, ms); break;
    case TRACK_FLAC: ok = drflac_seek_to_pcm_frame(d->flac, (drflac_uint64)ms * d->src_rate / 1000); break;
    case TRACK_WAV: ok = t5_audio_dec_seek(d->wav, ms) == T5_OK; break;
    default: ok = false; break;
    }
    if (ok) {
        d->npend = d->pend_at = 0;
        d->pos = (uint64_t)ms * d->rate / 1000;
    }
    return ok;
}

uint32_t dec_pos_ms(const dec_t *d)
{
    return d->rate ? (uint32_t)(d->pos * 1000 / d->rate) : 0;
}

void dec_close(dec_t *d)
{
    if (!d) return;
    if (d->flac) drflac_close(d->flac);
    if (d->wav) T5_IGNORE(t5_audio_dec_close(d->wav));
    if (d->f) fclose(d->f);
    free(d->mp3);
    free(d->buf);
    free(d->fbuf);
    free(d->pend);
    free(d);
}
