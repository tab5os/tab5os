/* SPDX-License-Identifier: Apache-2.0
 * t5_audio's hardware layer: I2S, the two codecs through esp_codec_dev
 * (register access through the t5_i2c broker), the speaker rail and the
 * headphone-detect pin. Always T5_AUDIO_RATE, 16-bit stereo.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_audio/dsp.h"
#include "t5_core/err.h"

/* The I2S DMA: 4 descriptors of 5 ms, so output reaches the DAC 20 ms after
 * it is written (the echo canceller's delay, and what a drain waits out). */
#define AUDIO_HW_DMA_DESC 4
#define AUDIO_HW_DMA_FRAMES 240
#define AUDIO_HW_OUT_LATENCY_MS (AUDIO_HW_DMA_DESC * AUDIO_HW_DMA_FRAMES * 1000 / T5_AUDIO_RATE)

/* Probe both codecs and create the I2S channels (codecs stay powered down). */
t5_err_t audio_hw_init(bool *dac_ok, bool *adc_ok);
t5_err_t audio_hw_out_open(int volume, bool muted);
void audio_hw_out_close(void);
/* Blocking: returns once the DMA has taken the block. */
t5_err_t audio_hw_write(const int16_t *pcm, size_t bytes);
t5_err_t audio_hw_in_open(float gain_db);
void audio_hw_in_close(void);
t5_err_t audio_hw_read(int16_t *pcm, size_t bytes);
void audio_hw_set_volume(int pct);
void audio_hw_set_mute(bool on);
void audio_hw_set_in_gain(float db);
bool audio_hw_headphones(void);
t5_err_t audio_hw_speaker(bool on);
