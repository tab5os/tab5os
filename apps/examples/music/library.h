/* The music library: every MP3, FLAC and WAV under /sd/music, with its tags,
 * grouped into albums (album name and folder) and artists, plus the M3U
 * playlists found there.
 *
 * Scanning reads tags only for files that are new or changed (path, size and
 * modification time against the last scan's index, which is cached in the
 * app's data folder). A library is built whole on the scanning task and
 * handed to the UI task, which owns it from then on. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "tags.h"

#define MUSIC_DIR "/sd/music"
#define PLAYLIST_DIR MUSIC_DIR "/playlists"
#define LIB_MAX_TRACKS 6000

typedef struct {
    const char *path;          /* absolute */
    const char *title, *artist, *album, *album_artist, *genre;
    uint32_t duration_ms, size, mtime, art_off, art_len;
    uint16_t track, disc, year;
    uint8_t fmt;
    int album_ix, artist_ix;
} lib_track_t;

typedef struct {
    const char *name;
    const char *artist;        /* the album artist, the shared artist, or "Various artists" */
    uint16_t year;
    int *tracks;               /* by disc, track number, title */
    int n;
    int cover;                 /* a track with an embedded cover, or the first */
} lib_album_t;

typedef struct {
    const char *name;
    int *tracks;               /* by album, then album order */
    int n;
    int nalbums;
} lib_artist_t;

typedef struct {
    const char *path;
    const char *name;          /* the file name without .m3u / .m3u8 */
    int n;                     /* entries in it */
} lib_playlist_t;

typedef struct lib lib_t;
struct lib {
    lib_track_t *t;
    int n;
    int *by_title;             /* track indices sorted by title */
    lib_album_t *albums;
    int nalbums;
    lib_artist_t *artists;
    int nartists;
    lib_playlist_t *pl;
    int npl;
    void *arena;
    int *hash;                 /* path -> track, open addressing */
    int hcap;
};

typedef void (*lib_progress_fn)(int files, void *user);

/* The cached index (no file checks): NULL if there is none. */
lib_t *lib_load_cache(const char *file);
/* Walk /sd/music. prev (may be NULL) supplies tags for unchanged files. */
lib_t *lib_scan(const lib_t *prev, lib_progress_fn progress, void *user);
/* Write the index (UI task: /data is internal flash). */
bool lib_save_cache(const lib_t *l, const char *file);
void lib_free(lib_t *l);

/* A track by path: exact, then case-insensitive, then by its last folders
 * and name (playlists made on a computer). -1 if it isn't in the library. */
int lib_find(const lib_t *l, const char *path);

/* A playlist's tracks as library indices (in order; missing files left
 * out). *missing counts them. malloc'd; NULL if it can't be read. */
int *lib_playlist_tracks(const lib_t *l, const char *path, int *n, int *missing);
/* Write a playlist (tracks as library indices). */
bool lib_playlist_write(const lib_t *l, const char *path, const int *tracks, int n);
/* Rescan just the playlists (after one is made, renamed or deleted). */
void lib_rescan_playlists(lib_t *l);

/* "3:07" or "1:02:03" */
void lib_fmt_time(char *out, size_t cap, uint32_t ms);
