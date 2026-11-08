/* SPDX-License-Identifier: Apache-2.0
 * Audio signal processing for t5_audio (plan 7.1, 7.2): resampling, the
 * output mixer, the system sounds and an NLMS echo canceller. Pure C with
 * no OS calls, so the host tests run it against synthetic signals.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The codecs always run at this format; streams are converted. */
#define T5_AUDIO_RATE 48000
#define T5_AUDIO_CH 2

/* ---- resampler: linear interpolation, any rate and 1 or 2 channels -------- */
typedef struct {
    uint32_t step;            /* in_rate / out_rate, 16.16 */
    uint32_t pos;             /* 16.16, 0 = the previous frame (last) */
    uint8_t in_ch;
    int16_t last[2];
} t5_resampler_t;

void t5_resampler_init(t5_resampler_t *r, uint32_t in_rate, uint8_t in_ch, uint32_t out_rate);
/* Read up to n_in frames of in, write up to max_out frames of out_ch
 * channels (mono in is copied to both, stereo to mono is averaged).
 * Returns frames written; *consumed is how many input frames were used. */
size_t t5_resample(t5_resampler_t *r, const int16_t *in, size_t n_in, size_t *consumed, int16_t *out,
                   size_t max_out, uint8_t out_ch);

/* ---- system sounds ---------------------------------------------------------- */
typedef enum {
    T5_SOUND_CLICK,           /* keys and toggles */
    T5_SOUND_NOTIFY,          /* snackbar-worthy events */
    T5_SOUND_ERROR,
    T5_SOUND_SHUTTER,         /* camera */
    T5_SOUND_START,           /* recording started */
    T5_SOUND_COUNT,
} t5_sound_t;

typedef struct {
    t5_sound_t sound;
    uint32_t t;               /* frames rendered */
    uint32_t phase;
    uint32_t seed;
    bool active;
} t5_voice_t;

void t5_voice_start(t5_voice_t *v, t5_sound_t s);
/* Add the next n stereo frames at T5_AUDIO_RATE into acc (interleaved).
 * Returns false once the sound has ended. */
bool t5_voice_render(t5_voice_t *v, int32_t *acc, size_t n);
uint32_t t5_sound_ms(t5_sound_t s);

/* ---- mixing ------------------------------------------------------------------ */
/* acc += pcm * gain_q15 / 32768 (samples, not frames). */
void t5_mix_add(int32_t *acc, const int16_t *pcm, size_t samples, int32_t gain_q15);
/* Saturate to 16 bits. */
void t5_mix_to_s16(const int32_t *acc, int16_t *out, size_t samples);
/* Peak level in dBFS (0 = full scale, -96 for silence). */
int t5_pcm_peak_dbfs(const int16_t *pcm, size_t samples);
/* Stereo interleaved to mono (average). out may equal in. */
void t5_pcm_downmix(const int16_t *in, int16_t *out, size_t frames);

/* ---- echo cancellation (NLMS with a Geigel double-talk detector) ------------- */
typedef struct {
    int taps;
    float mu;
    float *w;                 /* taps */
    float *x;                 /* 2 * taps: history, mirrored so it is contiguous */
    int xi;
    float power;              /* sum of x^2 over the window */
    int16_t *dl;              /* bulk delay line for the reference */
    int dl_len, dl_i;
    float in_energy, out_energy;   /* for ERLE, decaying sums */
    uint32_t frozen;          /* blocks where double talk froze adaptation */
    float dtd_ratio;          /* Geigel: |mic| above this times max |ref| = near-end talk */
} t5_aec_t;

/* taps: filter length in samples (512 at 16 kHz = 32 ms); delay: bulk
 * delay of the reference in samples (the playback path's latency).
 * Returns false if out of memory. */
bool t5_aec_init(t5_aec_t *a, int taps, int delay);
void t5_aec_free(t5_aec_t *a);
/* mic and ref are mono at the same rate; out may equal mic. */
void t5_aec_process(t5_aec_t *a, const int16_t *mic, const int16_t *ref, int16_t *out, size_t n);
/* Echo return loss enhancement so far, in dB (0 when unknown). */
float t5_aec_erle_db(const t5_aec_t *a);

#ifdef __cplusplus
}
#endif
