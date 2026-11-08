/* SPDX-License-Identifier: Apache-2.0 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "t5_audio/dsp.h"

/* ---- resampler --------------------------------------------------------------- */
void t5_resampler_init(t5_resampler_t *r, uint32_t in_rate, uint8_t in_ch, uint32_t out_rate)
{
    memset(r, 0, sizeof *r);
    if (!in_rate) in_rate = T5_AUDIO_RATE;
    if (!out_rate) out_rate = T5_AUDIO_RATE;
    r->step = (uint32_t)(((uint64_t)in_rate << 16) / out_rate);
    r->pos = 1u << 16;            /* start on in[0], not on the silent "last" frame */
    r->in_ch = in_ch == 1 ? 1 : 2;
}

static inline int16_t frame_at(const t5_resampler_t *r, const int16_t *in, uint32_t k, int ch)
{
    /* v[0] = last, v[k] = in[k - 1] */
    if (k == 0) return r->last[ch];
    return in[(k - 1) * r->in_ch + (r->in_ch == 1 ? 0 : ch)];
}

size_t t5_resample(t5_resampler_t *r, const int16_t *in, size_t n_in, size_t *consumed, int16_t *out,
                   size_t max_out, uint8_t out_ch)
{
    size_t n = 0;
    while (n < max_out) {
        uint32_t i = r->pos >> 16;
        int32_t frac = (int32_t)(r->pos & 0xFFFF);
        if (i > n_in || (frac && i + 1 > n_in)) break;    /* need v[i + 1] unless exactly on v[i] */
        int32_t s[2];
        for (int ch = 0; ch < 2; ch++) {
            int32_t a = frame_at(r, in, i, ch);
            s[ch] = frac ? a + (((frame_at(r, in, i + 1, ch) - a) * frac) >> 16) : a;
        }
        if (out_ch == 1) {
            out[n] = (int16_t)((s[0] + s[1]) / 2);
        } else {
            out[2 * n] = (int16_t)s[0];
            out[2 * n + 1] = (int16_t)s[1];
        }
        n++;
        r->pos += r->step;
    }
    uint32_t k = r->pos >> 16;
    if (k > n_in) k = (uint32_t)n_in;
    if (k > 0) {
        r->last[0] = frame_at(r, in, k, 0);
        r->last[1] = frame_at(r, in, k, 1);
    }
    r->pos -= k << 16;
    if (consumed) *consumed = k;
    return n;
}

/* ---- system sounds ------------------------------------------------------------ */
static int16_t s_sine[256];
static bool s_sine_ready;

static void sine_init(void)
{
    if (s_sine_ready) return;
    for (int i = 0; i < 256; i++) s_sine[i] = (int16_t)lrintf(32767.0f * sinf((float)i * 6.2831853f / 256.0f));
    s_sine_ready = true;
}

static const uint16_t s_len_ms[T5_SOUND_COUNT] = {
    [T5_SOUND_CLICK] = 12, [T5_SOUND_NOTIFY] = 260, [T5_SOUND_ERROR] = 220,
    [T5_SOUND_SHUTTER] = 90, [T5_SOUND_START] = 160,
};

uint32_t t5_sound_ms(t5_sound_t s)
{
    return s < T5_SOUND_COUNT ? s_len_ms[s] : 0;
}

void t5_voice_start(t5_voice_t *v, t5_sound_t s)
{
    sine_init();
    memset(v, 0, sizeof *v);
    v->sound = s;
    v->seed = 0x12345678u;
    v->active = s < T5_SOUND_COUNT;
}

static inline uint32_t phase_inc(uint32_t hz)
{
    /* 8.24 phase over a 256-entry table */
    return (uint32_t)(((uint64_t)hz << 32) / T5_AUDIO_RATE);
}

bool t5_voice_render(t5_voice_t *v, int32_t *acc, size_t n)
{
    if (!v->active) return false;
    uint32_t total = s_len_ms[v->sound] * (T5_AUDIO_RATE / 1000);
    for (size_t i = 0; i < n; i++, v->t++) {
        if (v->t >= total) {
            v->active = false;
            return false;
        }
        /* Linear decay envelope, 0..1 in Q15, with a 2 ms attack. */
        uint32_t attack = 2 * (T5_AUDIO_RATE / 1000);
        int32_t env = (int32_t)(((uint64_t)(total - v->t) << 15) / total);
        if (v->t < attack) env = env * (int32_t)v->t / (int32_t)attack;
        uint32_t hz;
        switch (v->sound) {
        case T5_SOUND_CLICK: hz = 2400; break;
        case T5_SOUND_NOTIFY: hz = v->t < total / 2 ? 880 : 1320; break;   /* two notes, a fifth up */
        case T5_SOUND_ERROR: hz = 220; break;
        case T5_SOUND_START: hz = 660 + (uint32_t)(v->t * 660 / total); break;  /* rising sweep */
        default: hz = 0; break;
        }
        int32_t s;
        if (v->sound == T5_SOUND_SHUTTER) {
            v->seed = v->seed * 1664525u + 1013904223u;            /* LCG noise burst */
            s = (int16_t)(v->seed >> 16);
        } else {
            v->phase += phase_inc(hz);
            s = s_sine[v->phase >> 24];
        }
        /* -12 dBFS: system sounds sit under app audio. */
        s = (s * env >> 15) / 4;
        acc[2 * i] += s;
        acc[2 * i + 1] += s;
    }
    return true;
}

/* ---- mixing ---------------------------------------------------------------------- */
void t5_mix_add(int32_t *acc, const int16_t *pcm, size_t samples, int32_t gain_q15)
{
    for (size_t i = 0; i < samples; i++) acc[i] += (pcm[i] * gain_q15 + 16384) >> 15;
}

void t5_mix_to_s16(const int32_t *acc, int16_t *out, size_t samples)
{
    for (size_t i = 0; i < samples; i++) {
        int32_t v = acc[i];
        out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

int t5_pcm_peak_dbfs(const int16_t *pcm, size_t samples)
{
    int32_t peak = 0;
    for (size_t i = 0; i < samples; i++) {
        int32_t a = pcm[i] < 0 ? -(int32_t)pcm[i] : pcm[i];
        if (a > peak) peak = a;
    }
    if (peak == 0) return -96;
    return (int)lrintf(20.0f * log10f((float)peak / 32768.0f));
}

void t5_pcm_downmix(const int16_t *in, int16_t *out, size_t frames)
{
    for (size_t i = 0; i < frames; i++) out[i] = (int16_t)(((int32_t)in[2 * i] + in[2 * i + 1]) / 2);
}

/* ---- NLMS echo canceller ------------------------------------------------------------ */
bool t5_aec_init(t5_aec_t *a, int taps, int delay)
{
    memset(a, 0, sizeof *a);
    a->taps = taps;
    a->mu = 0.3f;
    /* The Tab5's mics hear the speaker louder than the digital reference
     * (echo gain above 1 at 30 dB mic gain), so a classic 0.5 Geigel
     * threshold would call everything double talk and never adapt. */
    a->dtd_ratio = 4.0f;
    a->w = calloc((size_t)taps, sizeof *a->w);
    a->x = calloc((size_t)taps * 2, sizeof *a->x);
    a->dl_len = delay > 0 ? delay : 0;
    if (a->dl_len) a->dl = calloc((size_t)a->dl_len, sizeof *a->dl);
    if (!a->w || !a->x || (a->dl_len && !a->dl)) {
        t5_aec_free(a);
        return false;
    }
    return true;
}

void t5_aec_free(t5_aec_t *a)
{
    free(a->w);
    free(a->x);
    free(a->dl);
    a->w = a->x = NULL;
    a->dl = NULL;
}

void t5_aec_process(t5_aec_t *a, const int16_t *mic, const int16_t *ref, int16_t *out, size_t n)
{
    const int L = a->taps;
    bool froze = false;
    for (size_t k = 0; k < n; k++) {
        /* Bulk delay on the reference. */
        int16_t r = ref[k];
        if (a->dl_len) {
            int16_t d = a->dl[a->dl_i];
            a->dl[a->dl_i] = r;
            a->dl_i = (a->dl_i + 1) % a->dl_len;
            r = d;
        }
        float xr = (float)r / 32768.0f;
        /* Newest sample at x[xi] (and its mirror), history after it. */
        a->xi = (a->xi - 1 + L) % L;
        float old = a->x[a->xi];
        a->x[a->xi] = xr;
        a->x[a->xi + L] = xr;
        a->power += xr * xr - old * old;
        if (a->power < 0) a->power = 0;
        const float *xv = &a->x[a->xi];
        float y = 0, xmax = 0;
        for (int i = 0; i < L; i++) {
            y += a->w[i] * xv[i];
            float ax = fabsf(xv[i]);
            if (ax > xmax) xmax = ax;
        }
        float d = (float)mic[k] / 32768.0f;
        float e = d - y;
        /* Geigel: the near end talking over the echo freezes adaptation. */
        bool dt = fabsf(d) > a->dtd_ratio * xmax && xmax > 1e-4f;
        if (!dt) {
            /* Regularised: no huge steps while the window is still filling. */
            float g = a->mu * e / (a->power + 0.002f * (float)L);
            for (int i = 0; i < L; i++) a->w[i] += g * xv[i];
        } else {
            froze = true;
        }
        a->in_energy = a->in_energy * 0.9995f + d * d;
        a->out_energy = a->out_energy * 0.9995f + e * e;
        float o = e * 32768.0f;
        out[k] = (int16_t)(o > 32767.0f ? 32767 : o < -32768.0f ? -32768 : (int)o);
    }
    if (froze) a->frozen++;
}

float t5_aec_erle_db(const t5_aec_t *a)
{
    if (a->out_energy <= 0 || a->in_energy <= 0) return 0;
    return 10.0f * log10f(a->in_energy / a->out_energy);
}
