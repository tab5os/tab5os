/* SPDX-License-Identifier: Apache-2.0
 * Media sessions: what is playing, for the system to show and control (in
 * the SDK through t5/audio.h). No permission.
 *
 * An app that plays music, a podcast or a radio stream opens a session and
 * keeps its information and state current. The OS shows the active session
 * (the standby screen's music card) and sends the person's commands back to
 * it; the app takes them with t5_media_session_next() on its own task and
 * acts on them (pausing its stream, skipping a track...). The app does the
 * playing: a session plays nothing by itself.
 *
 * One session is active at a time, every app's together: the one most
 * recently opened or set playing. A session closes itself when its app exits.
 * Keep playing with the screen off by holding the stream open as usual: an
 * open, unpaused t5_audio_out stream keeps the device awake. Any task.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_MEDIA_TEXT_LEN 64

typedef enum {
    T5_MEDIA_STOPPED,
    T5_MEDIA_PLAYING,
    T5_MEDIA_PAUSED,
} t5_media_state_t;

typedef struct {
    char title[T5_MEDIA_TEXT_LEN];
    char artist[T5_MEDIA_TEXT_LEN];
    char album[T5_MEDIA_TEXT_LEN];
    uint32_t duration_ms;     /* 0: unknown (a stream) */
} t5_media_info_t;

typedef enum {
    T5_MEDIA_CMD_PLAY,
    T5_MEDIA_CMD_PAUSE,
    T5_MEDIA_CMD_TOGGLE,      /* play if paused or stopped, else pause */
    T5_MEDIA_CMD_NEXT,
    T5_MEDIA_CMD_PREVIOUS,
    T5_MEDIA_CMD_STOP,
    T5_MEDIA_CMD_SEEK,        /* to position_ms */
} t5_media_cmd_type_t;

typedef struct {
    t5_media_cmd_type_t type;
    uint32_t position_ms;     /* SEEK */
} t5_media_cmd_t;

typedef struct t5_media_session t5_media_session_t;

/* Open a session; it becomes the active one. One per app (T5_ERR_BUSY). */
t5_err_t t5_media_session_open(t5_media_session_t **out);
/* What is playing (copied). */
t5_err_t t5_media_session_set_info(t5_media_session_t *s, const t5_media_info_t *info);
/* The state and where playback is; the OS moves the position on by itself
 * while PLAYING, so call this on changes (play, pause, seek, a new track),
 * not continuously. Setting PLAYING makes the session the active one. */
t5_err_t t5_media_session_set_state(t5_media_session_t *s, t5_media_state_t state, uint32_t position_ms);
/* The next command for this session: T5_OK, or T5_ERR_TIMEOUT. 8 are queued. */
t5_err_t t5_media_session_next(t5_media_session_t *s, t5_media_cmd_t *cmd, uint32_t timeout_ms);
t5_err_t t5_media_session_close(t5_media_session_t *s);

#ifdef __cplusplus
}
#endif
