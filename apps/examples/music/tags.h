/* Track metadata: what a music file says about itself.
 *
 * MP3: ID3v2.2, 2.3 and 2.4 (text in Latin-1, UTF-16 or UTF-8, unsynchronised
 * tags, the front cover picture) and ID3v1; the duration from the Xing, Info
 * or VBRI header, else from the bitrate. FLAC: STREAMINFO, Vorbis comments
 * and the PICTURE block (an ID3v2 tag in front of it is skipped). WAV: the
 * fmt and data chunks, LIST/INFO and an embedded "id3 " chunk.
 *
 * Plain C over a read callback, so the same code reads a file on the device
 * and a buffer in the host tests (test/host/test_music.c). Text comes out as
 * UTF-8, cut on a character boundary.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TAG_TEXT 96

typedef enum { TRACK_UNKNOWN, TRACK_MP3, TRACK_FLAC, TRACK_WAV } track_fmt_t;

typedef struct {
    track_fmt_t fmt;
    char title[TAG_TEXT];
    char artist[TAG_TEXT];
    char album[TAG_TEXT];
    char album_artist[TAG_TEXT];
    char genre[48];
    uint16_t track, disc, year;
    uint32_t duration_ms;       /* 0 if unknown */
    uint32_t rate;              /* Hz */
    uint8_t channels;
    uint32_t art_off, art_len;  /* an embedded JPEG cover in the file; len 0 if none */
    uint32_t audio_off;         /* MP3: the first byte after any ID3v2 tag */
    uint32_t audio_end;         /* MP3: the end of the audio (before ID3v1) */
} tags_t;

/* Reads n bytes at off into buf; returns how many it read (short at the end). */
typedef size_t (*tag_read_fn)(void *ctx, uint32_t off, void *buf, size_t n);

typedef struct {
    tag_read_fn read;
    void *ctx;
    uint32_t size;              /* the file's length */
} tag_src_t;

/* By content, not by name. False if it isn't a format we play. */
bool tags_read(const tag_src_t *src, tags_t *out);

/* An MP3 frame header at p (4 bytes): false if it isn't one. */
typedef struct {
    uint32_t rate;
    uint16_t samples;           /* per frame: 384, 576 or 1152 */
    uint16_t bytes;             /* the frame's length */
    uint32_t kbps;
    uint8_t channels;
    uint8_t mpeg;               /* 1, 2, or 25 for 2.5 */
} mp3_frame_t;
bool mp3_frame_parse(const uint8_t *p, mp3_frame_t *out);

/* Helpers the library shares. */
/* Copy UTF-8 into dst (cap bytes with the NUL), cutting on a character. */
void tag_copy(char *dst, size_t cap, const char *src, size_t len);
/* Case-insensitive ASCII compare. */
int tag_casecmp(const char *a, const char *b);
