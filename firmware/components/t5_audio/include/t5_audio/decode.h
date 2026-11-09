/* SPDX-License-Identifier: Apache-2.0
 * Sound files: a decoder that gives PCM, and a player that plays a file on
 * its own (in the SDK through t5/audio.h).
 *
 * Formats: WAV (PCM 8/16/24/32-bit, float, WAVE_FORMAT_EXTENSIBLE; any rate,
 * up to 8 channels, of which the first two are used). MP3 is part of the
 * API but not in this OS build (t5_audio_format_supported() says so; the
 * open calls return T5_ERR_NOT_SUPPORTED): an app that needs it can compile
 * a decoder in and play its output through t5_audio_out.
 *
 * Files follow the calling app's file rules (its data and install folders,
 * shared folders with the permission, a file from t5_file_pick). Every
 * decoder and player closes itself when the app exits. At most
 * T5_AUDIO_DEC_MAX of them per app. Any task.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_audio/audio.h"
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_AUDIO_DEC_MAX 4

typedef enum {
    T5_AUDIO_FMT_UNKNOWN,
    T5_AUDIO_FMT_WAV,
    T5_AUDIO_FMT_MP3,
} t5_audio_format_t;

bool t5_audio_format_supported(t5_audio_format_t fmt);

typedef struct {
    t5_audio_format_t format;
    uint32_t rate;            /* Hz */
    uint8_t channels;         /* what t5_audio_dec_read gives: 1 or 2 */
    uint8_t file_channels;    /* in the file */
    uint8_t bits;             /* per sample in the file */
    uint32_t frames;          /* total, 0 if unknown */
    uint32_t duration_ms;     /* 0 if unknown */
} t5_audio_info_t;

/* ---- decoder: PCM out, at the file's rate --------------------------------- */
typedef struct t5_audio_dec t5_audio_dec_t;
/* By content, not by name. T5_ERR_NOT_FOUND, T5_ERR_DENIED (file rules),
 * T5_ERR_NOT_SUPPORTED (a format or encoding this build can't decode),
 * T5_ERR_INVALID_ARG (not a sound file), T5_ERR_BUSY (too many open). */
T5_MUST_CHECK t5_err_t t5_audio_dec_open(const char *path, t5_audio_dec_t **out);
/* From memory (an asset loaded by the app); data must stay valid until the
 * decoder is closed. */
T5_MUST_CHECK t5_err_t t5_audio_dec_open_mem(const void *data, size_t len, t5_audio_dec_t **out);
t5_err_t t5_audio_dec_info(t5_audio_dec_t *d, t5_audio_info_t *out);
/* Up to frames frames of 16-bit interleaved PCM (info.channels per frame).
 * Returns the frames read, 0 at the end, or a negative t5_err_t. */
int t5_audio_dec_read(t5_audio_dec_t *d, int16_t *pcm, size_t frames);
/* Jump to ms from the start (clamped to the end). */
t5_err_t t5_audio_dec_seek(t5_audio_dec_t *d, uint32_t ms);
t5_err_t t5_audio_dec_close(t5_audio_dec_t *d);

/* ---- player: decodes and plays on its own task ----------------------------- */
typedef struct t5_audio_player t5_audio_player_t;
typedef enum {
    T5_AUDIO_PLAYER_PLAYING,
    T5_AUDIO_PLAYER_PAUSED,
    T5_AUDIO_PLAYER_ENDED,    /* played to the end */
    T5_AUDIO_PLAYER_ERROR,    /* a read failed partway */
} t5_audio_player_state_t;

/* Start playing at once, as an output stream of the given usage (counts
 * against T5_AUDIO_OUT_MAX). */
T5_MUST_CHECK t5_err_t t5_audio_play_file(const char *path, t5_audio_usage_t usage, t5_audio_player_t **out);
T5_MUST_CHECK t5_err_t t5_audio_play_mem(const void *data, size_t len, t5_audio_usage_t usage,
                                         t5_audio_player_t **out);
t5_err_t t5_audio_player_pause(t5_audio_player_t *p, bool paused);
/* 0..100, the stream's level under the system volume (default 100). */
t5_err_t t5_audio_player_set_volume(t5_audio_player_t *p, int pct);
t5_err_t t5_audio_player_seek(t5_audio_player_t *p, uint32_t ms);
/* The state, and (pos_ms, may be NULL) how far it has played. */
t5_audio_player_state_t t5_audio_player_state(t5_audio_player_t *p, uint32_t *pos_ms);
/* Wait until it has ended (or failed), up to timeout_ms: T5_OK, T5_ERR_TIMEOUT. */
t5_err_t t5_audio_player_wait(t5_audio_player_t *p, uint32_t timeout_ms);
/* Stop and free it. */
t5_err_t t5_audio_player_close(t5_audio_player_t *p);

#ifdef __cplusplus
}
#endif
