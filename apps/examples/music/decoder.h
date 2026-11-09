/* One decoder over the three formats: MP3 (minimp3), FLAC (dr_flac) and WAV
 * (the OS decoder). 16-bit interleaved PCM, mono or stereo, at most 48 kHz:
 * the output streams take no more, so 88.2, 96, 176.4 and 192 kHz files are
 * decimated by 2 or 4 here. Runs on the player's task. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tags.h"

typedef struct dec dec_t;

/* t: the file's tags (format, MP3 audio range, duration). NULL on failure. */
dec_t *dec_open(const char *path, const tags_t *t);
uint32_t dec_rate(const dec_t *d);          /* what dec_read gives */
uint8_t dec_channels(const dec_t *d);       /* 1 or 2 */
/* Up to frames frames; 0 at the end, negative on an error. */
int dec_read(dec_t *d, int16_t *pcm, size_t frames);
/* Jump to ms from the start (MP3: by the Xing table of contents, or by bytes
 * for constant bitrate; it lands within a frame). */
bool dec_seek(dec_t *d, uint32_t ms);
uint32_t dec_pos_ms(const dec_t *d);        /* how far decoding has got */
void dec_close(dec_t *d);
