/* M3U playlists: reading (.m3u, .m3u8: UTF-8 with or without a BOM, CRLF,
 * #EXTINF titles, relative or absolute paths, Windows separators, file://
 * URLs) and the paths to write. Plain C, host tested. */
#pragma once
#include <stddef.h>

#define M3U_PATH_MAX 256

/* One entry: the path resolved against the playlist's folder and cleaned
 * ("a/./b/../c" -> "a/c", '\\' -> '/'), and the #EXTINF title ("" if none). */
typedef void (*m3u_entry_fn)(const char *path, const char *title, void *user);

/* Parse text (len bytes); dir is the playlist's folder ("/sd/music/playlists").
 * Returns the entries seen. */
int m3u_parse(const char *text, size_t len, const char *dir, m3u_entry_fn fn, void *user);

/* Clean a path in place: separators, "." and ".." (never above the root). */
void m3u_clean(char *path);

/* path relative to dir, both absolute ("/sd/music/a/b.mp3" from
 * "/sd/music/playlists" -> "../a/b.mp3"). Case-insensitive (FAT). */
void m3u_relative(char *out, size_t cap, const char *dir, const char *path);
