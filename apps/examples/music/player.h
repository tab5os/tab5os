/* The player: a queue of files, decoded on a task of its own into an output
 * stream, published as a media session (the standby screen's music card,
 * whose buttons come back here).
 *
 * The queue keeps paths in the order they were given and a play order over
 * them (shuffled or not). Positions below are play-order positions.
 * Consecutive tracks with the same rate and channels share a stream, so
 * there's no gap between them. Any task; the calls only set what the
 * player's task does next.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <t5/app.h>
#include "tags.h"

typedef enum { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE } repeat_t;

typedef struct {
    uint32_t version;          /* changes with the track or the queue */
    bool has_track;
    bool playing;
    uint32_t pos_ms, dur_ms;
    bool shuffle;
    repeat_t repeat;
    int qpos, qlen;            /* the current play-order position, the queue's length */
    char path[256];
    tags_t tags;               /* the current track's */
} player_state_t;

bool player_start(t5_app_t *app);
void player_stop(void);

/* Replace the queue with n paths and play from start (in that order; with
 * shuffle, start plays first and the rest is shuffled). */
void player_set_queue(const char *const *paths, int n, int start, bool shuffle);
/* After the current track (next), or at the end. */
void player_add(const char *path, bool next);
void player_remove(int qpos);
/* Drop what keep() says is gone (a rescan found the files missing). */
void player_retain(bool (*keep)(const char *path));
void player_jump(int qpos);

void player_play(void);
void player_pause(void);
void player_toggle(void);
void player_next(void);
void player_prev(void);          /* restarts the track if it's past 3 s */
void player_seek(uint32_t ms);
void player_set_shuffle(bool on);
void player_set_repeat(repeat_t r);

void player_get(player_state_t *out);
/* The path at a play-order position: false past the end. */
bool player_queue_item(int qpos, char *out, size_t cap);

/* Save and restore what's queued, where, and the modes (text; the caller
 * writes and reads the file). Restored paused. */
char *player_save_text(void);    /* malloc'd */
void player_load_text(const char *text);
