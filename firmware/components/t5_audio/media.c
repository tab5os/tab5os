/* SPDX-License-Identifier: Apache-2.0
 * Media sessions (t5_audio/media.h, media_sys.h): a small table, one per
 * app, the active one the most recently opened or set playing. Each is the
 * opening app's resource, closed when it exits.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "t5_audio/media_sys.h"
#include "t5_core/os.h"
#include "t5_core/res.h"

#define MAX_SESSIONS 4
#define CMD_QUEUE 8
#define SESSION_MAGIC 0x4d534553u

struct t5_media_session {
    uint32_t magic;
    void *app;
    char app_id[64];
    t5_media_info_t info;
    t5_media_state_t state;
    uint32_t position_ms;
    int64_t since_us;                   /* when position_ms was set */
    uint32_t order;                     /* higher: more recently made active */
    QueueHandle_t cmds;
};

static t5_media_session_t *s_sessions[MAX_SESSIONS];
static uint32_t s_order;
static t5_mutex_t s_mx;

static t5_mutex_t mx(void)
{
    return t5_mutex_once(&s_mx);
}

static bool valid_locked(const t5_media_session_t *s)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s && s_sessions[i] == s) return s->magic == SESSION_MAGIC;
    }
    return false;
}

static void release(void *h)
{
    T5_IGNORE(t5_media_session_close(h));   /* exit cleanup: nothing to report to */
}

t5_err_t t5_media_session_open(t5_media_session_t **out)
{
    if (!out) return T5_ERR_INVALID_ARG;
    *out = NULL;
    t5_mutex_t m = mx();
    if (!m) return T5_ERR_NO_MEM;
    void *app = t5_app_ctx_get();
    t5_media_session_t *s = heap_caps_calloc(1, sizeof *s, MALLOC_CAP_SPIRAM);
    if (!s) return T5_ERR_NO_MEM;
    s->magic = SESSION_MAGIC;
    s->app = app;
    const t5_app_ctx_hdr_t *h = app;
    snprintf(s->app_id, sizeof s->app_id, "%s", h && h->magic == T5_APP_CTX_MAGIC ? h->id : "system");
    s->cmds = xQueueCreateWithCaps(CMD_QUEUE, sizeof(t5_media_cmd_t), MALLOC_CAP_SPIRAM);
    t5_mutex_lock(m);
    int slot = -1;
    t5_err_t e = s->cmds ? T5_OK : T5_ERR_NO_MEM;
    for (int i = 0; e == T5_OK && i < MAX_SESSIONS; i++) {
        if (app && s_sessions[i] && s_sessions[i]->app == app) e = T5_ERR_BUSY;
        if (!s_sessions[i] && slot < 0) slot = i;
    }
    if (e == T5_OK && slot < 0) e = T5_ERR_BUSY;
    if (e == T5_OK) {
        s->order = ++s_order;
        s->since_us = t5_time_us();
        s_sessions[slot] = s;
        if (app) t5_res_add(T5_RES_OTHER, s, release);
    }
    t5_mutex_unlock(m);
    if (e != T5_OK) {
        if (s->cmds) vQueueDeleteWithCaps(s->cmds);
        free(s);
        return e;
    }
    *out = s;
    return T5_OK;
}

t5_err_t t5_media_session_set_info(t5_media_session_t *s, const t5_media_info_t *info)
{
    if (!info) return T5_ERR_INVALID_ARG;
    t5_mutex_lock(mx());
    t5_err_t e = valid_locked(s) ? T5_OK : T5_ERR_INVALID_ARG;
    if (e == T5_OK) {
        s->info = *info;
        s->info.title[sizeof s->info.title - 1] = 0;
        s->info.artist[sizeof s->info.artist - 1] = 0;
        s->info.album[sizeof s->info.album - 1] = 0;
    }
    t5_mutex_unlock(mx());
    return e;
}

t5_err_t t5_media_session_set_state(t5_media_session_t *s, t5_media_state_t state, uint32_t position_ms)
{
    if ((unsigned)state > T5_MEDIA_PAUSED) return T5_ERR_INVALID_ARG;
    t5_mutex_lock(mx());
    t5_err_t e = valid_locked(s) ? T5_OK : T5_ERR_INVALID_ARG;
    if (e == T5_OK) {
        if (state == T5_MEDIA_PLAYING && s->state != T5_MEDIA_PLAYING) s->order = ++s_order;
        s->state = state;
        s->position_ms = position_ms;
        s->since_us = t5_time_us();
    }
    t5_mutex_unlock(mx());
    return e;
}

t5_err_t t5_media_session_next(t5_media_session_t *s, t5_media_cmd_t *cmd, uint32_t timeout_ms)
{
    if (!cmd) return T5_ERR_INVALID_ARG;
    t5_mutex_lock(mx());
    QueueHandle_t q = valid_locked(s) ? s->cmds : NULL;
    t5_mutex_unlock(mx());
    if (!q) return T5_ERR_INVALID_ARG;
    /* The queue lives until close, which only the owner calls (or the exit
     * cleanup, after the app's tasks are gone). */
    return xQueueReceive(q, cmd, pdMS_TO_TICKS(timeout_ms)) == pdTRUE ? T5_OK : T5_ERR_TIMEOUT;
}

t5_err_t t5_media_session_close(t5_media_session_t *s)
{
    t5_mutex_t m = mx();
    if (!m) return T5_ERR_STATE;
    t5_mutex_lock(m);
    bool ok = valid_locked(s);
    if (ok) {
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (s_sessions[i] == s) s_sessions[i] = NULL;
        }
        s->magic = 0;
    }
    t5_mutex_unlock(m);
    if (!ok) return T5_ERR_INVALID_ARG;
    t5_res_remove(s);
    vQueueDeleteWithCaps(s->cmds);
    free(s);
    return T5_OK;
}

/* The active session (s_mx held). */
static t5_media_session_t *active_locked(void)
{
    t5_media_session_t *best = NULL;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i] && (!best || s_sessions[i]->order > best->order)) best = s_sessions[i];
    }
    return best;
}

bool t5_media_now(t5_media_now_t *out)
{
    memset(out, 0, sizeof *out);
    t5_mutex_t m = mx();
    if (!m) return false;
    t5_mutex_lock(m);
    t5_media_session_t *s = active_locked();
    if (s) {
        out->active = true;
        snprintf(out->app_id, sizeof out->app_id, "%s", s->app_id);
        out->info = s->info;
        out->state = s->state;
        uint64_t pos = s->position_ms;
        if (s->state == T5_MEDIA_PLAYING) pos += (uint64_t)(t5_time_us() - s->since_us) / 1000;
        if (s->info.duration_ms && pos > s->info.duration_ms) pos = s->info.duration_ms;
        out->position_ms = (uint32_t)pos;
    }
    t5_mutex_unlock(m);
    return s != NULL;
}

t5_err_t t5_media_command(const t5_media_cmd_t *cmd)
{
    if (!cmd) return T5_ERR_INVALID_ARG;
    t5_mutex_t m = mx();
    if (!m) return T5_ERR_STATE;
    t5_mutex_lock(m);
    t5_media_session_t *s = active_locked();
    t5_err_t e = !s ? T5_ERR_STATE : xQueueSend(s->cmds, cmd, 0) == pdTRUE ? T5_OK : T5_ERR_BUSY;
    t5_mutex_unlock(m);
    return e;
}
