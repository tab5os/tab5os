/* SPDX-License-Identifier: Apache-2.0
 * Audio service (plan 7.1-7.3): the ES8388 DAC with the speaker amp and the
 * headphone jack, the ES7210 microphones, a mixer for system sounds and one
 * app stream, echo cancellation, and power handling.
 *
 * The codecs run at 48 kHz, 16-bit stereo, and are powered down by register
 * whenever nothing plays or records. Streams in other formats are converted.
 * The audio task runs at the top priority. An open output stream holds an
 * audio wake lock, so playback continues with the screen off; the device
 * still sleeps once the stream is closed or paused.
 *
 * Speaker or headphones: the amp (SPK_EN rail) is on only while output runs
 * and no headphones are plugged in (HP_DET, polled).
 *
 * All functions are safe from any task.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_audio/dsp.h"
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

t5_err_t t5_audio_init(void);

/* ---- output ---------------------------------------------------------------- */
int t5_audio_volume(void);              /* 0..100, persisted */
t5_err_t t5_audio_set_volume(int pct);
bool t5_audio_muted(void);
t5_err_t t5_audio_set_muted(bool on);
bool t5_audio_headphones(void);
/* System sounds (clicks, notifications): mixed over the app stream, skipped
 * when muted or turned off in Settings. */
void t5_audio_play_sound(t5_sound_t s);
bool t5_audio_system_sounds(void);
void t5_audio_set_system_sounds(bool on);

/* One app output stream at a time (T5_ERR_BUSY otherwise). rate 8000..48000,
 * channels 1 or 2, 16-bit signed little-endian. */
typedef struct t5_audio_out t5_audio_out_t;
typedef struct {
    uint32_t rate;
    uint8_t channels;
    uint32_t buffer_ms;                 /* 0 = 200 ms */
} t5_audio_out_cfg_t;
t5_err_t t5_audio_out_open(const t5_audio_out_cfg_t *cfg, t5_audio_out_t **out);
/* Queue PCM. Blocks up to timeout_ms for room; returns bytes accepted, or a
 * negative t5_err_t. */
int t5_audio_out_write(t5_audio_out_t *o, const void *pcm, size_t bytes, uint32_t timeout_ms);
/* Pause keeps the stream but releases the wake lock. */
t5_err_t t5_audio_out_pause(t5_audio_out_t *o, bool paused);
/* Wait until everything queued has played (up to timeout_ms). */
t5_err_t t5_audio_out_drain(t5_audio_out_t *o, uint32_t timeout_ms);
uint32_t t5_audio_out_queued_ms(const t5_audio_out_t *o);
t5_err_t t5_audio_out_close(t5_audio_out_t *o);

/* Play a tone through the app stream path (test and examples). */
t5_err_t t5_audio_tone(uint32_t hz, uint32_t ms);

/* ---- input ------------------------------------------------------------------ */
/* Needs the mic permission (asked on first use). rate 8000..48000, channels
 * 1 or 2. aec: cancel the device's own output (only at 16 kHz or lower,
 * mono). While a stream is open the status bar shows the mic indicator. */
typedef struct t5_audio_in t5_audio_in_t;
typedef struct {
    uint32_t rate;
    uint8_t channels;
    bool aec;
    uint32_t buffer_ms;                 /* 0 = 200 ms */
} t5_audio_in_cfg_t;
t5_err_t t5_audio_in_open(const t5_audio_in_cfg_t *cfg, t5_audio_in_t **out);
/* Read PCM, waiting up to timeout_ms for the first byte. Returns bytes read
 * or a negative t5_err_t. */
int t5_audio_in_read(t5_audio_in_t *in, void *pcm, size_t bytes, uint32_t timeout_ms);
t5_err_t t5_audio_in_close(t5_audio_in_t *in);
bool t5_audio_in_active(void);
float t5_audio_in_gain_db(void);
t5_err_t t5_audio_in_set_gain_db(float db);   /* 0..37.5, persisted */

/* ---- diagnostics -------------------------------------------------------------- */
typedef struct {
    bool ready, out_running, in_running;
    bool speaker_on, headphones, muted;
    int volume;
    uint32_t underruns;                 /* the app stream ran dry while playing */
    uint32_t overruns;                  /* input dropped: the reader was too slow */
    uint32_t out_frames, in_frames;
    uint32_t loop_max_us;               /* longest output loop */
    int out_peak_dbfs, in_peak_dbfs;
    float aec_erle_db;
    uint32_t codec_errors;
    uint32_t opens;                     /* output codec power-ups */
} t5_audio_stats_t;
void t5_audio_stats(t5_audio_stats_t *out);

#ifdef __cplusplus
}
#endif
