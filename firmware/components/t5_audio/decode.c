/* SPDX-License-Identifier: Apache-2.0
 * Sound files (t5_audio/decode.h): a WAV decoder over a file or a memory
 * buffer, and a player that runs a decoder into an output stream on a task
 * of its own.
 *
 * Ownership: a decoder or player an app opens is the app's resource and is
 * closed when it exits. A player's decoder and stream belong to the player
 * (not tracked on their own), so the exit cleanup closes the player, which
 * stops its task and then frees them, in that order.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "t5_audio/decode.h"
#include "t5_audio/wav.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_core/res.h"

static const char *TAG = "t5.audio.dec";

#define DEC_MAGIC 0x41444543u
#define PLAYER_MAGIC 0x41504c59u
#define HEAD_FIRST 4096
#define HEAD_MAX 65536                  /* chunks before "data" past this: not a file we take */
#define READ_FRAMES 480                 /* 10 ms at 48 kHz */
#define PLAYER_STACK 4096
#define PLAYER_PRIO 6                   /* above apps (2), below audio (top) */
#define WRITE_WAIT_MS 100
#define STOP_WAIT_MS 1000

struct t5_audio_dec {
    uint32_t magic;
    void *app;
    FILE *f;                            /* or mem */
    const uint8_t *mem;
    size_t mem_len;
    t5_wav_t wav;
    uint32_t pos;                       /* bytes into the samples */
    uint8_t raw[READ_FRAMES * T5_WAV_MAX_CH * 8];
};

/* ---- decoder ------------------------------------------------------------- */
static t5_mutex_t s_mx;
static struct {
    void *app;
    int n;
} s_counts[8];

static bool count_take(void *app)
{
    t5_mutex_t m = t5_mutex_once(&s_mx);
    if (!m) return false;
    t5_mutex_lock(m);
    int at = -1;
    for (int i = 0; i < 8; i++) {
        if (s_counts[i].n && s_counts[i].app == app) at = i;
        else if (at < 0 && !s_counts[i].n) at = i;
    }
    bool ok = at >= 0 && (!app || s_counts[at].n < T5_AUDIO_DEC_MAX);
    if (ok) {
        s_counts[at].app = app;
        s_counts[at].n++;
    }
    t5_mutex_unlock(m);
    return ok;
}

static void count_give(void *app)
{
    t5_mutex_t m = t5_mutex_once(&s_mx);
    t5_mutex_lock(m);
    for (int i = 0; i < 8; i++) {
        if (s_counts[i].n && s_counts[i].app == app) {
            s_counts[i].n--;
            break;
        }
    }
    t5_mutex_unlock(m);
}

static size_t src_read(t5_audio_dec_t *d, size_t off, void *buf, size_t n)
{
    if (d->mem) {
        if (off >= d->mem_len) return 0;
        if (n > d->mem_len - off) n = d->mem_len - off;
        memcpy(buf, d->mem + off, n);
        return n;
    }
    if (fseek(d->f, (long)off, SEEK_SET)) return 0;
    return fread(buf, 1, n, d->f);
}

static size_t src_len(t5_audio_dec_t *d)
{
    if (d->mem) return d->mem_len;
    long here = ftell(d->f);
    fseek(d->f, 0, SEEK_END);
    long len = ftell(d->f);
    fseek(d->f, here, SEEK_SET);
    return len < 0 ? 0 : (size_t)len;
}

static bool is_mp3(const uint8_t *h, size_t n)
{
    return (n >= 3 && !memcmp(h, "ID3", 3)) || (n >= 2 && h[0] == 0xff && (h[1] & 0xe0) == 0xe0);
}

/* The header, growing the buffer while the chunks before "data" need it. */
static t5_err_t parse_head(t5_audio_dec_t *d)
{
    size_t cap = HEAD_FIRST;
    for (;;) {
        uint8_t *h = malloc(cap);
        if (!h) return T5_ERR_NO_MEM;
        size_t n = src_read(d, 0, h, cap);
        t5_err_t e = t5_wav_parse(h, n, &d->wav);
        bool mp3 = e == T5_ERR_INVALID_ARG && is_mp3(h, n);
        free(h);
        if (mp3) return T5_ERR_NOT_SUPPORTED;          /* not in this build */
        if (e != T5_ERR_STATE || n < cap || cap >= HEAD_MAX) return e == T5_ERR_STATE ? T5_ERR_INVALID_ARG : e;
        cap *= 4;
    }
}

static void dec_release(void *h)
{
    T5_IGNORE(t5_audio_dec_close(h));   /* exit cleanup: nothing to report to */
}

static t5_err_t dec_open(FILE *f, const void *mem, size_t len, bool track, t5_audio_dec_t **out)
{
    void *app = t5_app_ctx_get();
    if (!count_take(app)) {
        if (f) fclose(f);
        return T5_ERR_BUSY;
    }
    t5_audio_dec_t *d = heap_caps_calloc(1, sizeof *d, MALLOC_CAP_SPIRAM);
    if (!d) {
        if (f) fclose(f);
        count_give(app);
        return T5_ERR_NO_MEM;
    }
    d->magic = DEC_MAGIC;
    d->app = app;
    d->f = f;
    d->mem = mem;
    d->mem_len = len;
    t5_err_t e = parse_head(d);
    if (e == T5_OK) {
        /* A size past the end (a streamed file says 0xffffffff): what's there. */
        size_t have = src_len(d);
        size_t avail = have > d->wav.data_off ? have - d->wav.data_off : 0;
        if (d->wav.data_len > avail) d->wav.data_len = (uint32_t)(avail - avail % d->wav.block);
    }
    if (e != T5_OK) {
        if (f) fclose(f);
        d->magic = 0;
        free(d);
        count_give(app);
        return e;
    }
    if (track && app) t5_res_add(T5_RES_OTHER, d, dec_release);
    *out = d;
    return T5_OK;
}

bool t5_audio_format_supported(t5_audio_format_t fmt)
{
    return fmt == T5_AUDIO_FMT_WAV;
}

static t5_err_t open_path(const char *path, FILE **f)
{
    if (!path || !*path) return T5_ERR_INVALID_ARG;
    /* The OS opens the file for the app: its file rules apply. */
    if (!t5_core_path_check(path, false)) return T5_ERR_DENIED;
    *f = fopen(path, "rb");
    return *f ? T5_OK : T5_ERR_NOT_FOUND;
}

t5_err_t t5_audio_dec_open(const char *path, t5_audio_dec_t **out)
{
    if (!out) return T5_ERR_INVALID_ARG;
    *out = NULL;
    FILE *f = NULL;
    T5_RETURN_ON_ERR(open_path(path, &f));
    return dec_open(f, NULL, 0, true, out);
}

t5_err_t t5_audio_dec_open_mem(const void *data, size_t len, t5_audio_dec_t **out)
{
    if (!out || !data || !len) return T5_ERR_INVALID_ARG;
    *out = NULL;
    return dec_open(NULL, data, len, true, out);
}

static bool dec_ok(t5_audio_dec_t *d)
{
    return d && d->magic == DEC_MAGIC;
}

t5_err_t t5_audio_dec_info(t5_audio_dec_t *d, t5_audio_info_t *out)
{
    if (!dec_ok(d) || !out) return T5_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    out->format = T5_AUDIO_FMT_WAV;
    out->rate = d->wav.rate;
    out->channels = t5_wav_out_channels(&d->wav);
    out->file_channels = (uint8_t)d->wav.channels;
    out->bits = (uint8_t)d->wav.bits;
    out->frames = d->wav.data_len / d->wav.block;
    out->duration_ms = (uint32_t)((uint64_t)out->frames * 1000 / d->wav.rate);
    return T5_OK;
}

int t5_audio_dec_read(t5_audio_dec_t *d, int16_t *pcm, size_t frames)
{
    if (!dec_ok(d) || !pcm) return T5_ERR_INVALID_ARG;
    size_t done = 0;
    while (done < frames) {
        size_t left = (d->wav.data_len - d->pos) / d->wav.block;
        size_t n = frames - done;
        if (n > READ_FRAMES) n = READ_FRAMES;
        if (n > left) n = left;
        if (!n) break;
        size_t got = src_read(d, d->wav.data_off + d->pos, d->raw, n * d->wav.block) / d->wav.block;
        if (!got) return done ? (int)done : T5_ERR_IO;
        t5_wav_convert(&d->wav, d->raw, got, pcm + done * t5_wav_out_channels(&d->wav));
        d->pos += (uint32_t)(got * d->wav.block);
        done += got;
    }
    return (int)done;
}

t5_err_t t5_audio_dec_seek(t5_audio_dec_t *d, uint32_t ms)
{
    if (!dec_ok(d)) return T5_ERR_INVALID_ARG;
    uint64_t frame = (uint64_t)ms * d->wav.rate / 1000;
    uint64_t byte = frame * d->wav.block;
    d->pos = byte > d->wav.data_len ? d->wav.data_len : (uint32_t)byte;
    return T5_OK;
}

static uint32_t dec_pos_ms(t5_audio_dec_t *d)
{
    return (uint32_t)((uint64_t)(d->pos / d->wav.block) * 1000 / d->wav.rate);
}

t5_err_t t5_audio_dec_close(t5_audio_dec_t *d)
{
    if (!dec_ok(d)) return T5_ERR_INVALID_ARG;
    t5_res_remove(d);
    d->magic = 0;
    if (d->f) fclose(d->f);
    count_give(d->app);
    free(d);
    return T5_OK;
}

/* ---- player -------------------------------------------------------------- */
struct t5_audio_player {
    uint32_t magic;
    t5_audio_dec_t *dec;
    t5_audio_out_t *out;
    TaskHandle_t task;
    SemaphoreHandle_t ended;            /* given once: end, error or stop */
    SemaphoreHandle_t gone;             /* the task has let go */
    volatile t5_audio_player_state_t state;
    volatile bool paused, stop;
    volatile int32_t seek_ms;           /* -1: none */
    volatile uint32_t pos_ms;
};

static void player_task(void *arg)
{
    t5_audio_player_t *p = arg;
    int16_t *pcm = heap_caps_malloc(READ_FRAMES * 2 * sizeof *pcm, MALLOC_CAP_SPIRAM);
    uint8_t ch = t5_wav_out_channels(&p->dec->wav);
    bool ok = pcm != NULL;
    while (ok && !p->stop) {
        if (p->seek_ms >= 0) {
            T5_IGNORE(t5_audio_dec_seek(p->dec, (uint32_t)p->seek_ms));   /* a valid decoder: can't fail */
            p->seek_ms = -1;
        }
        if (p->paused) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WRITE_WAIT_MS));
            continue;
        }
        int n = t5_audio_dec_read(p->dec, pcm, READ_FRAMES);
        if (n < 0) {
            ok = false;
            break;
        }
        if (n == 0) break;
        size_t bytes = (size_t)n * ch * sizeof *pcm, done = 0;
        while (done < bytes && !p->stop && p->seek_ms < 0) {
            int w = t5_audio_out_write(p->out, (const uint8_t *)pcm + done, bytes - done, WRITE_WAIT_MS);
            if (w < 0) {
                ok = false;
                break;
            }
            done += (size_t)w;
        }
        uint32_t at = dec_pos_ms(p->dec), queued = t5_audio_out_queued_ms(p->out);
        p->pos_ms = at > queued ? at - queued : 0;
    }
    /* The tail, in short waits so a close isn't kept waiting. */
    for (int i = 0; ok && !p->stop && t5_audio_out_queued_ms(p->out) && i < 200; i++) vTaskDelay(pdMS_TO_TICKS(10));
    p->pos_ms = dec_pos_ms(p->dec);
    p->state = !ok ? T5_AUDIO_PLAYER_ERROR : T5_AUDIO_PLAYER_ENDED;
    if (!ok) T5_LOGW(TAG, "playback stopped: read or write failed");
    free(pcm);
    xSemaphoreGive(p->ended);
    xSemaphoreGive(p->gone);
    vTaskSuspend(NULL);                 /* deleted by close */
}

static void player_free(t5_audio_player_t *p)
{
    if (p->out) T5_IGNORE(t5_audio_out_close(p->out));       /* the player's own: nothing to report */
    if (p->dec) T5_IGNORE(t5_audio_dec_close(p->dec));
    if (p->ended) vSemaphoreDelete(p->ended);
    if (p->gone) vSemaphoreDelete(p->gone);
    p->magic = 0;
    free(p);
}

static void player_release(void *h)
{
    T5_IGNORE(t5_audio_player_close(h));
}

static t5_err_t play(t5_audio_dec_t *dec, t5_audio_usage_t usage, t5_audio_player_t **out)
{
    void *app = t5_app_ctx_get();
    t5_audio_player_t *p = heap_caps_calloc(1, sizeof *p, MALLOC_CAP_SPIRAM);
    if (!p) {
        T5_IGNORE(t5_audio_dec_close(dec));
        return T5_ERR_NO_MEM;
    }
    p->magic = PLAYER_MAGIC;
    p->dec = dec;
    p->seek_ms = -1;
    p->ended = xSemaphoreCreateBinary();
    p->gone = xSemaphoreCreateBinary();
    t5_audio_out_cfg_t cfg = { .rate = dec->wav.rate, .channels = t5_wav_out_channels(&dec->wav), .buffer_ms = 300 };
    t5_err_t e = p->ended && p->gone ? T5_OK : T5_ERR_NO_MEM;
    if (e == T5_OK) e = t5_audio_out_open_usage(&cfg, usage, &p->out);
    if (e == T5_OK) {
        t5_res_remove(p->out);          /* the player's, not the app's */
        if (xTaskCreatePinnedToCoreWithCaps(player_task, "audio.play", PLAYER_STACK, p, PLAYER_PRIO, &p->task,
                                            tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            e = T5_ERR_NO_MEM;
        }
    }
    if (e != T5_OK) {
        player_free(p);
        return e;
    }
    if (app) t5_res_add(T5_RES_OTHER, p, player_release);
    *out = p;
    return T5_OK;
}

/* A rate the output can't take (over 48 kHz, under 8 kHz). */
static t5_err_t playable(t5_audio_dec_t *d)
{
    if (d->wav.rate >= 8000 && d->wav.rate <= 48000) return T5_OK;
    T5_IGNORE(t5_audio_dec_close(d));
    return T5_ERR_NOT_SUPPORTED;
}

t5_err_t t5_audio_play_file(const char *path, t5_audio_usage_t usage, t5_audio_player_t **out)
{
    if (!out) return T5_ERR_INVALID_ARG;
    *out = NULL;
    FILE *f = NULL;
    T5_RETURN_ON_ERR(open_path(path, &f));
    t5_audio_dec_t *d;
    T5_RETURN_ON_ERR(dec_open(f, NULL, 0, false, &d));
    T5_RETURN_ON_ERR(playable(d));
    return play(d, usage, out);
}

t5_err_t t5_audio_play_mem(const void *data, size_t len, t5_audio_usage_t usage, t5_audio_player_t **out)
{
    if (!out || !data || !len) return T5_ERR_INVALID_ARG;
    *out = NULL;
    t5_audio_dec_t *d;
    T5_RETURN_ON_ERR(dec_open(NULL, data, len, false, &d));
    T5_RETURN_ON_ERR(playable(d));
    return play(d, usage, out);
}

static bool player_ok(t5_audio_player_t *p)
{
    return p && p->magic == PLAYER_MAGIC;
}

t5_err_t t5_audio_player_pause(t5_audio_player_t *p, bool paused)
{
    if (!player_ok(p)) return T5_ERR_INVALID_ARG;
    if (p->state == T5_AUDIO_PLAYER_ENDED || p->state == T5_AUDIO_PLAYER_ERROR) return T5_ERR_STATE;
    p->paused = paused;
    p->state = paused ? T5_AUDIO_PLAYER_PAUSED : T5_AUDIO_PLAYER_PLAYING;
    T5_RETURN_ON_ERR(t5_audio_out_pause(p->out, paused));
    xTaskNotifyGive(p->task);
    return T5_OK;
}

t5_err_t t5_audio_player_set_volume(t5_audio_player_t *p, int pct)
{
    if (!player_ok(p)) return T5_ERR_INVALID_ARG;
    return t5_audio_out_set_volume(p->out, pct);
}

t5_err_t t5_audio_player_seek(t5_audio_player_t *p, uint32_t ms)
{
    if (!player_ok(p)) return T5_ERR_INVALID_ARG;
    if (p->state == T5_AUDIO_PLAYER_ENDED || p->state == T5_AUDIO_PLAYER_ERROR) return T5_ERR_STATE;
    p->seek_ms = (int32_t)(ms > INT32_MAX ? INT32_MAX : ms);
    xTaskNotifyGive(p->task);
    return T5_OK;
}

t5_audio_player_state_t t5_audio_player_state(t5_audio_player_t *p, uint32_t *pos_ms)
{
    if (!player_ok(p)) return T5_AUDIO_PLAYER_ERROR;
    if (pos_ms) *pos_ms = p->pos_ms;
    return p->state;
}

t5_err_t t5_audio_player_wait(t5_audio_player_t *p, uint32_t timeout_ms)
{
    if (!player_ok(p)) return T5_ERR_INVALID_ARG;
    if (xSemaphoreTake(p->ended, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return T5_ERR_TIMEOUT;
    xSemaphoreGive(p->ended);           /* others waiting see it too */
    return T5_OK;
}

t5_err_t t5_audio_player_close(t5_audio_player_t *p)
{
    if (!player_ok(p)) return T5_ERR_INVALID_ARG;
    t5_res_remove(p);
    p->stop = true;
    xTaskNotifyGive(p->task);
    if (xSemaphoreTake(p->gone, pdMS_TO_TICKS(STOP_WAIT_MS)) != pdTRUE) {
        T5_LOGE(TAG, "player task didn't stop: leaking it");
        return T5_ERR_TIMEOUT;
    }
    vTaskDeleteWithCaps(p->task);
    player_free(p);
    return T5_OK;
}
