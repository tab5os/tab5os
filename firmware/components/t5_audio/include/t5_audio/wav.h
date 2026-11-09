/* SPDX-License-Identifier: Apache-2.0
 * WAV (RIFF WAVE) parsing and sample conversion for the decoder
 * (t5_audio/decode.h). Pure C, host-tested. For the OS only: apps use
 * t5_audio_dec_* and t5_audio_play_*.
 *
 * Takes PCM (8-bit unsigned, 16, 24 and 32-bit signed), IEEE float (32 and
 * 64-bit) and WAVE_FORMAT_EXTENSIBLE wrapping either, 1 to 8 channels, any
 * rate. Converts to 16-bit signed, 1 or 2 channels (more are cut to the
 * first two).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_WAV_PCM 1
#define T5_WAV_FLOAT 3
#define T5_WAV_EXTENSIBLE 0xfffe
#define T5_WAV_MAX_CH 8

typedef struct {
    uint16_t format;          /* T5_WAV_PCM or T5_WAV_FLOAT (extensible resolved) */
    uint16_t channels;
    uint32_t rate;
    uint16_t bits;            /* per sample, as stored */
    uint16_t block;           /* bytes per frame */
    uint32_t data_off;        /* where the samples start in the file */
    uint32_t data_len;        /* bytes of samples (cut to whole frames) */
} t5_wav_t;

/* Parse the header from the first n bytes of the file (the chunks before
 * "data"; 4 KB covers an ordinary file). T5_ERR_STATE: the chunks before
 * "data" go past n, try again with more. T5_ERR_NOT_SUPPORTED: a WAVE file
 * in another encoding. T5_ERR_INVALID_ARG: not a WAV file. */
t5_err_t t5_wav_parse(const uint8_t *buf, size_t n, t5_wav_t *out);
/* Channels the decoder gives: the file's, at most 2. */
uint8_t t5_wav_out_channels(const t5_wav_t *w);
/* frames of the file's samples (in) to 16-bit, t5_wav_out_channels(w)
 * channels, into out. */
void t5_wav_convert(const t5_wav_t *w, const uint8_t *in, size_t frames, int16_t *out);

#ifdef __cplusplus
}
#endif
