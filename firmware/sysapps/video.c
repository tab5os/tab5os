/* SPDX-License-Identifier: Apache-2.0
 * Full-screen video player, shared by Files, the Camera's gallery and the
 * Videos app, over the calling app's screen.
 *
 * Motion JPEG in AVI: what the Camera records, and what a computer makes
 * with, for example,
 *     ffmpeg -i in.mp4 -vf scale=1280:-2 -c:v mjpeg -q:v 4 -c:a pcm_s16le out.avi
 * Each frame is a JPEG for the hardware decoder (1280x720 in ~10 ms). The
 * P4 has no H.264 decoder, and the software one manages ~4 fps at 720p.
 * Sound plays if it is PCM (8 or 16 bit, mono or stereo); other sound
 * formats play silently, with a note.
 *
 * A worker task reads the file, decodes frames into one of two buffers
 * and posts them to a video plane (t5_ui/plane.h), which scales each one
 * into the framebuffer as it draws it, and keeps time: by the sound when
 * there is any (what has played of it), else by the clock. Frames that are
 * late are skipped without decoding them, so a slow card costs smoothness,
 * not sync. The UI task only draws the controls, which hide while playing.
 */
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "t5/audio.h"
#include "t5/camera.h"
#include "t5/shell.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_gui/t5ui_icons.h"
#include "t5_gui/t5ui_sys.h"
#include "t5_power/power.h"
#include "t5_ui/plane.h"
#include "sysapps_priv.h"

#define HIDE_MS 3000
#define AUDIO_AHEAD_MS 400
#define TICK_MS 50

enum { ST_OPENING, ST_READY, ST_PLAYING, ST_ERROR };

typedef struct {
    uint32_t off, len;                /* the data's file offset, and its length (0: repeat the last frame) */
} vchunk_t;

typedef struct {
    uint32_t off, len;
    uint64_t start;                   /* where it starts in the sound, in bytes */
} achunk_t;

typedef struct {
    char path[160];
    void (*on_close)(void);
    bool was_fullscreen;
    /* UI task */
    t5ui_obj_t *root, *view, *msg, *top, *bar, *play, *slider, *pos_lbl, *dur_lbl;
    t5ui_timer_t *timer;
    uint32_t touched_ms, away_ms;
    bool controls, dragging, ready_seen;
    t5_wakelock_t *lock;
    /* shared (mx guards the plane and its geometry) */
    t5_mutex_t mx;
    t5_plane_t *plane;
    int k, pw, ph;
    volatile int state;
    char err[96];
    volatile bool paused, quit, ended;
    volatile int32_t seek_ms;         /* -1: none */
    volatile uint32_t pos_ms;
    uint32_t dur_ms;
    char note[64];                    /* about the sound, shown once ready */
    /* the worker's */
    SemaphoreHandle_t exited;
    FILE *f;
    vchunk_t *v;
    uint32_t nv, v_cap;
    achunk_t *a;
    uint32_t na, a_cap;
    uint64_t a_total;
    uint32_t us_per_frame;
    uint16_t jw, jh, jpw, jph;        /* the frames' real and decoded (rounded to 16) size */
    int vstream, astream;
    bool mjpeg, has_audio;
    uint16_t a_fmt, a_ch, a_bits, a_align;
    uint32_t a_rate, a_bps;
    uint32_t max_v, max_a;
    uint32_t late, shown;
} video_t;

static video_t *s_v;

bool sa_video_is_open(void)
{
    return s_v != NULL;
}

bool sa_video_can_open(const char *path)
{
    return sa_ends_with(path, ".avi");
}

/* ---- AVI ---------------------------------------------------------------------------- */
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static bool is4(const uint8_t *p, const char *cc)
{
    return memcmp(p, cc, 4) == 0;
}

static bool is4i(const uint8_t *p, const char *cc)
{
    for (int i = 0; i < 4; i++) {
        char c = (char)p[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c != cc[i]) return false;
    }
    return true;
}

static bool read_at(video_t *v, uint32_t off, void *buf, size_t n)
{
    return fseek(v->f, (long)off, SEEK_SET) == 0 && fread(buf, 1, n, v->f) == n;
}

static void parse_strl(video_t *v, const uint8_t *p, uint32_t n, int stream)
{
    const uint8_t *strh = NULL;
    for (uint32_t pos = 0; pos + 8 <= n;) {
        uint32_t sz = rd32(p + pos + 4);
        if (sz > n - pos - 8) break;
        const uint8_t *d = p + pos + 8;
        if (is4(p + pos, "strh") && sz >= 36) {
            strh = d;
        } else if (is4(p + pos, "strf") && strh) {
            if (is4(strh, "vids") && v->vstream < 0 && sz >= 20) {
                v->vstream = stream;
                v->mjpeg = is4i(strh + 4, "MJPG") || is4i(d + 16, "MJPG") || is4i(strh + 4, "JPEG") ||
                           is4i(d + 16, "JPEG");
                uint32_t scale = rd32(strh + 20), rate = rd32(strh + 24);
                if (scale && rate) v->us_per_frame = (uint32_t)((uint64_t)scale * 1000000u / rate);
                if (!v->mjpeg) {
                    snprintf(v->err, sizeof v->err, "It isn't Motion JPEG (its video is %.4s).",
                             is4(strh + 4, "\0\0\0\0") ? (const char *)d + 16 : (const char *)strh + 4);
                }
            } else if (is4(strh, "auds") && v->astream < 0 && sz >= 16) {
                v->astream = stream;
                v->a_fmt = rd16(d);
                v->a_ch = rd16(d + 2);
                v->a_rate = rd32(d + 4);
                v->a_bps = rd32(d + 8);
                v->a_align = rd16(d + 12);
                v->a_bits = rd16(d + 14);
                /* WAVE_FORMAT_EXTENSIBLE: PCM if its subformat says so. */
                if (v->a_fmt == 0xFFFE && sz >= 26) v->a_fmt = rd16(d + 24);
            }
        }
        pos += 8 + sz + (sz & 1);
    }
}

static void parse_hdrl(video_t *v, const uint8_t *p, uint32_t n)
{
    int stream = 0;
    for (uint32_t pos = 0; pos + 8 <= n;) {
        uint32_t sz = rd32(p + pos + 4);
        if (sz > n - pos - 8) break;
        const uint8_t *d = p + pos + 8;
        if (is4(p + pos, "avih") && sz >= 40) {
            if (!v->us_per_frame) v->us_per_frame = rd32(d);
        } else if (is4(p + pos, "LIST") && sz >= 4 && is4(d, "strl")) {
            parse_strl(v, d + 4, sz - 4, stream++);
        }
        pos += 8 + sz + (sz & 1);
    }
}

static bool add_chunk(video_t *v, const uint8_t id[4], uint32_t off, uint32_t len)
{
    if (id[0] < '0' || id[0] > '9' || id[1] < '0' || id[1] > '9') return true;
    int stream = (id[0] - '0') * 10 + (id[1] - '0');
    if (stream == v->vstream && (id[2] == 'd') && (id[3] == 'c' || id[3] == 'b')) {
        if (v->nv == v->v_cap) {
            uint32_t cap = v->v_cap ? v->v_cap * 2 : 4096;
            vchunk_t *n = heap_caps_realloc(v->v, cap * sizeof *n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!n) return false;
            v->v = n;
            v->v_cap = cap;
        }
        v->v[v->nv++] = (vchunk_t){ off, len };
        if (len > v->max_v) v->max_v = len;
    } else if (stream == v->astream && id[2] == 'w' && id[3] == 'b' && len) {
        if (v->na == v->a_cap) {
            uint32_t cap = v->a_cap ? v->a_cap * 2 : 4096;
            achunk_t *n = heap_caps_realloc(v->a, cap * sizeof *n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!n) return false;
            v->a = n;
            v->a_cap = cap;
        }
        v->a[v->na++] = (achunk_t){ off, len, v->a_total };
        v->a_total += len;
        if (len > v->max_a) v->max_a = len;
    }
    return true;
}

/* idx1: one entry per chunk. Offsets count from the 'movi' fourcc, or
 * (some writers) from the start of the file. */
static t5_err_t read_idx1(video_t *v, uint32_t at, uint32_t n, uint32_t movi, uint32_t movi_end)
{
    enum { BATCH = 256 };
    uint8_t *buf = malloc(BATCH * 16);
    if (!buf) return T5_ERR_NO_MEM;
    uint32_t base = 0;
    bool based = false;
    t5_err_t e = T5_OK;
    for (uint32_t i = 0; i < n && e == T5_OK; i += BATCH) {
        uint32_t m = n - i < BATCH ? n - i : BATCH;
        if (!read_at(v, at + i * 16, buf, m * 16)) {
            e = T5_ERR_IO;
            break;
        }
        for (uint32_t j = 0; j < m; j++) {
            const uint8_t *en = buf + j * 16;
            uint32_t off = rd32(en + 8), len = rd32(en + 12);
            if (!based) {
                based = true;
                base = off >= movi && off < movi_end ? 0 : movi;
            }
            if (!add_chunk(v, en, base + off + 8, len)) {
                e = T5_ERR_NO_MEM;
                break;
            }
        }
    }
    free(buf);
    return e;
}

/* No index: walk the movi list. */
static t5_err_t scan_movi(video_t *v, uint32_t pos, uint32_t end)
{
    uint8_t h[12];
    while (pos + 8 <= end && !v->quit) {
        if (!read_at(v, pos, h, 12)) break;
        uint32_t sz = rd32(h + 4);
        if (is4(h, "LIST")) {          /* 'rec ' groups */
            pos += 12;
            continue;
        }
        if (!add_chunk(v, h, pos + 8, sz)) return T5_ERR_NO_MEM;
        pos += 8 + sz + (sz & 1);
    }
    return T5_OK;
}

static t5_err_t parse(video_t *v)
{
    v->f = fopen(v->path, "rb");
    if (!v->f) {
        snprintf(v->err, sizeof v->err, "The file couldn't be opened.");
        return T5_ERR_NOT_FOUND;
    }
    fseek(v->f, 0, SEEK_END);
    long fsize = ftell(v->f);
    uint8_t h[12];
    if (fsize < 12 || !read_at(v, 0, h, 12) || !is4(h, "RIFF") || !is4(h + 8, "AVI ")) {
        snprintf(v->err, sizeof v->err, "It isn't an AVI file.");
        return T5_ERR_NOT_SUPPORTED;
    }
    uint32_t end = (uint32_t)fsize, riff_end = 8 + rd32(h + 4);
    if (riff_end < end) end = riff_end;
    uint32_t movi = 0, movi_end = 0, idx_at = 0, idx_n = 0;
    v->vstream = v->astream = -1;
    for (uint32_t pos = 12; pos + 8 <= end;) {
        if (!read_at(v, pos, h, 12)) break;
        uint32_t sz = rd32(h + 4);
        if (is4(h, "LIST") && is4(h + 8, "hdrl") && sz >= 4 && sz < 256 * 1024) {
            uint8_t *b = malloc(sz - 4);
            if (b && read_at(v, pos + 12, b, sz - 4)) parse_hdrl(v, b, sz - 4);
            free(b);
        } else if (is4(h, "LIST") && is4(h + 8, "movi")) {
            movi = pos + 8;
            movi_end = pos + 8 + sz;
        } else if (is4(h, "idx1")) {
            idx_at = pos + 8;
            idx_n = sz / 16;
        }
        pos += 8 + sz + (sz & 1);
    }
    if (v->vstream < 0 || !movi) {
        snprintf(v->err, sizeof v->err, "It has no video in it.");
        return T5_ERR_NOT_SUPPORTED;
    }
    if (!v->mjpeg) return T5_ERR_NOT_SUPPORTED;
    t5_err_t e = idx_n ? read_idx1(v, idx_at, idx_n, movi, movi_end) : T5_OK;
    if (e == T5_OK && !v->nv) e = scan_movi(v, movi + 4, movi_end > end ? end : movi_end);
    if (e != T5_OK || !v->nv) {
        snprintf(v->err, sizeof v->err, "Its index couldn't be read.");
        return e != T5_OK ? e : T5_ERR_IO;
    }
    if (!v->us_per_frame) v->us_per_frame = 33333;
    v->dur_ms = (uint32_t)((uint64_t)v->nv * v->us_per_frame / 1000);
    /* Sound: PCM only. */
    if (v->astream >= 0 && v->na) {
        bool pcm = v->a_fmt == 1 && (v->a_bits == 8 || v->a_bits == 16) && v->a_ch >= 1 && v->a_ch <= 2 &&
                   v->a_rate >= 8000 && v->a_rate <= 48000;
        if (pcm) {
            v->has_audio = true;
            v->a_bps = v->a_rate * v->a_ch * (v->a_bits / 8);
            if (!v->a_align) v->a_align = (uint16_t)(v->a_ch * (v->a_bits / 8));
        } else {
            snprintf(v->note, sizeof v->note, "No sound: only PCM sound plays (this is format 0x%04x)", v->a_fmt);
        }
    }
    return T5_OK;
}

/* The first real frame tells the size (headers often round or lie). */
static t5_err_t probe(video_t *v)
{
    for (uint32_t i = 0; i < v->nv && i < 64; i++) {
        if (!v->v[i].len) continue;
        uint32_t n = v->v[i].len < 64 * 1024 ? v->v[i].len : 64 * 1024;
        uint8_t *b = malloc(n);
        bool ok = b && read_at(v, v->v[i].off, b, n);
        t5_err_t e = ok ? t5_jpeg_size(b, n, &v->jpw, &v->jph) : T5_ERR_IO;
        if (e == T5_OK) e = t5_jpeg_dims(b, n, &v->jw, &v->jh);
        free(b);
        if (e != T5_OK || !v->jw || !v->jh) {
            snprintf(v->err, sizeof v->err, "Its frames aren't JPEGs this decoder can read.");
            return T5_ERR_NOT_SUPPORTED;
        }
        return T5_OK;
    }
    snprintf(v->err, sizeof v->err, "It has no frames.");
    return T5_ERR_NOT_SUPPORTED;
}

/* ---- playback (the worker) ---------------------------------------------------------- */
typedef struct {
    uint8_t *in;
    /* Two decoded frames: one the screen may still read (until the plane
     * releases it), one being decoded. */
    uint16_t *dec[2];
    size_t dec_bytes;
    t5_plane_t *dec_plane[2];         /* where each was posted (a reference), and as which frame */
    uint32_t dec_seq[2];
    int next;
    uint8_t *abuf;
    int16_t *a16;
    t5_audio_out_t *out;
    bool out_paused;
    uint32_t ai, a_skip;
    uint64_t a_written;               /* output bytes since a_base_us */
    int64_t a_base_us;
    int64_t clk_base;                 /* media time = now - clk_base, without sound */
} play_t;

static uint32_t frame_at(const video_t *v, uint32_t ms)
{
    uint64_t f = (uint64_t)ms * 1000 / v->us_per_frame;
    return f >= v->nv ? v->nv - 1 : (uint32_t)f;
}

static void audio_seek(video_t *v, play_t *pl, int64_t us)
{
    if (pl->out) t5_audio_out_close(pl->out);
    pl->out = NULL;
    pl->out_paused = false;
    pl->a_written = 0;
    pl->a_base_us = us;
    pl->ai = pl->a_skip = 0;
    if (!v->has_audio) return;
    uint64_t byte = (uint64_t)us * v->a_bps / 1000000;
    byte -= byte % v->a_align;
    uint32_t lo = 0, hi = v->na;
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (v->a[mid].start <= byte) lo = mid;
        else hi = mid;
    }
    pl->ai = lo;
    uint64_t skip = byte > v->a[lo].start ? byte - v->a[lo].start : 0;
    pl->a_skip = skip < v->a[lo].len ? (uint32_t)skip : 0;
    if (skip >= v->a[lo].len) pl->ai++;
}

static uint32_t out_bps(const video_t *v)
{
    return v->a_rate * v->a_ch * 2;
}

/* Keep AUDIO_AHEAD_MS of sound queued. */
static void audio_fill(video_t *v, play_t *pl)
{
    if (!v->has_audio || pl->ai >= v->na) return;
    if (!pl->out) {
        t5_audio_out_cfg_t cfg = { .rate = v->a_rate, .channels = (uint8_t)v->a_ch, .buffer_ms = 600 };
        if (t5_audio_out_open(&cfg, &pl->out) != T5_OK) {
            v->has_audio = false;
            snprintf(v->note, sizeof v->note, "No sound: the audio output is busy");
            return;
        }
    }
    while (pl->ai < v->na && !v->quit && t5_audio_out_queued_ms(pl->out) < AUDIO_AHEAD_MS) {
        const achunk_t *c = &v->a[pl->ai];
        uint32_t n = c->len - pl->a_skip;
        if (!read_at(v, c->off + pl->a_skip, pl->abuf, n)) {
            pl->ai = v->na;               /* stop the sound, keep the picture */
            break;
        }
        const void *pcm = pl->abuf;
        size_t bytes = n;
        if (v->a_bits == 8) {
            for (uint32_t i = 0; i < n; i++) pl->a16[i] = (int16_t)(((int)pl->abuf[i] - 128) << 8);
            pcm = pl->a16;
            bytes = (size_t)n * 2;
        }
        size_t done = 0;
        while (done < bytes && !v->quit) {
            int w = t5_audio_out_write(pl->out, (const uint8_t *)pcm + done, bytes - done, 100);
            if (w < 0) break;
            done += (size_t)w;
        }
        pl->a_written += done;
        pl->ai++;
        pl->a_skip = 0;
    }
}

/* Where playback is now (µs): what has been heard of the sound while it
 * plays, else the clock (kept in step with the sound, so it can take over
 * when the sound ends first). */
static int64_t media_now(video_t *v, play_t *pl)
{
    int64_t now = t5_time_us();
    if (pl->out && pl->a_written) {
        uint32_t q = t5_audio_out_queued_ms(pl->out);
        if (q || pl->ai < v->na) {
            int64_t t = pl->a_base_us + (int64_t)(pl->a_written * 1000000 / out_bps(v)) - (int64_t)q * 1000;
            pl->clk_base = now - t;
            return t;
        }
    }
    return now - pl->clk_base;
}

/* The current plane, held, and its scale. */
static t5_plane_t *get_plane(video_t *v, int *k)
{
    t5_mutex_lock(v->mx);
    t5_plane_t *p = v->plane;
    t5_plane_hold(p);
    *k = v->k;
    t5_mutex_unlock(v->mx);
    return p;
}

/* Wait until slot i's frame is no longer read by the screen. */
static bool slot_free(video_t *v, play_t *pl, int i)
{
    while (pl->dec_plane[i] && !t5_plane_released(pl->dec_plane[i], pl->dec_seq[i])) {
        if (v->quit) return false;
        t5_delay_ms(2);
    }
    t5_plane_release(pl->dec_plane[i]);
    pl->dec_plane[i] = NULL;
    return true;
}

/* Decode frame i into the next slot; returns the slot, or -1. */
static int decode(video_t *v, play_t *pl, uint32_t i)
{
    const vchunk_t *c = &v->v[i];
    int s = pl->next;
    if (!read_at(v, c->off, pl->in, c->len) || !slot_free(v, pl, s)) return -1;
    uint16_t w, h;
    if (t5_jpeg_decode_dma(pl->in, c->len, pl->dec[s], pl->dec_bytes, &w, &h) != T5_OK || w != v->jpw || h != v->jph)
        return -1;
    return s;
}

/* Show slot s: the screen scales it as it draws it. */
static void post(video_t *v, play_t *pl, int s)
{
    int k;
    t5_plane_t *p = get_plane(v, &k);
    /* The screen takes the last frame within a frame or two. */
    for (int t = 0; p && !t5_plane_ready(p) && t < 100 && !v->quit && !t5_plane_closed(p); t++) t5_delay_ms(2);
    t5_plane_src_t src = {
        .buf = pl->dec[s], .fmt = T5_PLANE_RGB565, .pic_w = v->jpw, .pic_h = v->jph,
        .w = v->jw, .h = v->jh, .k = (uint16_t)k,
    };
    uint32_t seq = p ? t5_plane_post(p, &src) : 0;
    if (!seq) {
        t5_plane_release(p);
        return;
    }
    pl->dec_plane[s] = p;
    pl->dec_seq[s] = seq;
    pl->next = s ^ 1;
    v->shown++;
}

static void play(video_t *v)
{
    play_t pl = { 0 };
    pl.in = t5_jpeg_alloc(v->max_v + 16);
    pl.dec_bytes = ((size_t)v->jpw * v->jph * 2 + 127) & ~(size_t)127;
    pl.dec[0] = t5_jpeg_alloc(pl.dec_bytes);
    pl.dec[1] = t5_jpeg_alloc(pl.dec_bytes);
    if (v->has_audio) {
        pl.abuf = heap_caps_malloc(v->max_a, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        pl.a16 = v->a_bits == 8 ? heap_caps_malloc((size_t)v->max_a * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
        if (!pl.abuf || (v->a_bits == 8 && !pl.a16)) {
            v->has_audio = false;
            snprintf(v->note, sizeof v->note, "No sound: not enough memory");
        }
    }
    if (!pl.in || !pl.dec[0] || !pl.dec[1]) {
        snprintf(v->err, sizeof v->err, "Not enough memory to play it.");
        v->state = ST_ERROR;
        goto out;
    }
    v->state = ST_PLAYING;
    uint32_t vi = 0;
    bool show_now = true;             /* the next frame goes up at once (start, a seek) */
    int64_t paused_at = 0;
    pl.clk_base = t5_time_us();
    while (!v->quit) {
        int32_t sk = __atomic_exchange_n(&v->seek_ms, -1, __ATOMIC_ACQ_REL);
        if (sk >= 0) {
            vi = frame_at(v, (uint32_t)sk);
            int64_t us = (int64_t)vi * v->us_per_frame;
            audio_seek(v, &pl, us);
            pl.clk_base = t5_time_us() - us;
            if (paused_at) paused_at = t5_time_us();
            show_now = true;
            v->ended = false;
        }
        if (v->paused && !show_now) {
            if (!paused_at) paused_at = t5_time_us();
            if (pl.out && !pl.out_paused) {
                t5_audio_out_pause(pl.out, true);
                pl.out_paused = true;
            }
            t5_delay_ms(20);
            continue;
        }
        if (!v->paused && paused_at) {
            pl.clk_base += t5_time_us() - paused_at;
            paused_at = 0;
            if (pl.out && pl.out_paused) {
                t5_audio_out_pause(pl.out, false);
                pl.out_paused = false;
            }
        }
        if (vi >= v->nv) {
            /* The end, once the sound has played out too. */
            if (!pl.out || !t5_audio_out_queued_ms(pl.out) || pl.out_paused) {
                v->ended = true;
                v->paused = true;
                v->pos_ms = v->dur_ms;
            }
            t5_delay_ms(20);
            continue;
        }
        if (!v->paused) audio_fill(v, &pl);
        int64_t now = media_now(v, &pl);
        int64_t pts = (int64_t)vi * v->us_per_frame;
        if (!show_now && pts + v->us_per_frame < now) {
            vi++;                     /* late: skip it, don't decode it */
            v->late++;
            continue;
        }
        int slot = -1;
        if (v->v[vi].len) {
            slot = decode(v, &pl, vi);
            if (slot < 0) {
                vi++;                 /* a bad frame: the last one stays up */
                continue;
            }
        }
        /* Wait for its time, keeping the sound topped up. */
        bool interrupted = false;
        while (!show_now) {
            if (v->quit || v->paused || __atomic_load_n(&v->seek_ms, __ATOMIC_ACQUIRE) >= 0) {
                interrupted = true;
                break;
            }
            audio_fill(v, &pl);
            int64_t d = pts - media_now(v, &pl);
            if (d <= 1000) break;
            t5_delay_ms(d > 20000 ? 20 : (uint32_t)(d / 1000));
        }
        if (interrupted) continue;    /* not posted: decoded again after */
        if (slot >= 0) post(v, &pl, slot);
        v->pos_ms = (uint32_t)(pts / 1000);
        vi++;
        show_now = false;
    }
out:
    T5_LOGI("t5.video", "%ux%u, %u frames at %u us: %u shown, %u skipped late", v->jw, v->jh, (unsigned)v->nv,
            (unsigned)v->us_per_frame, (unsigned)v->shown, (unsigned)v->late);
    if (pl.out) t5_audio_out_close(pl.out);
    /* The screen keeps a copy of the last frame before the buffers go (or
     * has already let go, when the player is being closed). */
    for (int i = 0; i < 2; i++) {
        if (pl.dec_plane[i]) t5_plane_end(pl.dec_plane[i]);
        t5_plane_release(pl.dec_plane[i]);
    }
    free(pl.in);
    free(pl.dec[0]);
    free(pl.dec[1]);
    heap_caps_free(pl.abuf);
    heap_caps_free(pl.a16);
}

static void worker(void *arg)
{
    video_t *v = arg;
    t5_err_t e = parse(v);
    if (e == T5_OK) e = probe(v);
    if (e != T5_OK) {
        if (!v->err[0]) snprintf(v->err, sizeof v->err, "%s.", t5_err_str(e));
        v->state = ST_ERROR;
    } else {
        v->state = ST_READY;
        /* The UI makes the plane once it knows the size. */
        while (!v->quit) {
            t5_mutex_lock(v->mx);
            bool have = v->plane != NULL;
            t5_mutex_unlock(v->mx);
            if (have) break;
            t5_delay_ms(10);
        }
        if (!v->quit) play(v);
    }
    if (v->f) fclose(v->f);
    v->f = NULL;
    xSemaphoreGive(v->exited);
}

/* ---- the UI ----------------------------------------------------------------------------- */
static void fmt_time(char *out, size_t n, uint32_t ms)
{
    uint32_t s = ms / 1000;
    if (s >= 3600) snprintf(out, n, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    else snprintf(out, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

static void show_controls(video_t *v, bool on)
{
    v->controls = on;
    v->touched_ms = t5_time_ms();
    t5ui_set_int(v->top, T5UI_P_HIDDEN, !on);
    if (v->state == ST_PLAYING || v->state == ST_READY) t5ui_set_int(v->bar, T5UI_P_HIDDEN, !on);
}

/* The plane: the video scaled to fit (in 1/16 steps) and centred. */
static void make_view(video_t *v)
{
    t5ui_rect_t rc;
    t5ui_coords(v->root, &rc);
    int32_t rw = rc.x2 - rc.x1 + 1, rh = rc.y2 - rc.y1 + 1;
    if (rw < 16 || rh < 16 || !v->jw) return;
    int k = (int)(16 * rw / v->jw);
    int ky = (int)(16 * rh / v->jh);
    if (ky < k) k = ky;
    if (k > 64) k = 64;               /* small videos up to 4x */
    if (k < 1) k = 1;
    int pw = sa_ppa_scaled(v->jw, k), ph = sa_ppa_scaled(v->jh, k);
    if (pw < 1 || ph < 1) return;
    t5_plane_t *p = t5_plane_new(pw, ph);
    if (!p) return;
    /* The plane's view is platform code's: an object of its own, taken in here. */
    t5ui_obj_t *view = t5ui_sys_obj(t5_ui_plane_create(t5ui_sys_native(v->root), p));
    if (!view) {
        t5_plane_release(p);
        return;
    }
    t5ui_set_int(view, T5UI_P_ALIGN, T5UI_ALIGN_CENTER);
    t5ui_set_int(view, T5UI_P_X, 0);
    t5ui_set_int(view, T5UI_P_Y, 0);
    t5ui_move_to(view, 0);            /* under the controls */
    t5_mutex_lock(v->mx);
    t5_plane_t *old = v->plane;
    v->plane = p;
    v->k = k;
    v->pw = pw;
    v->ph = ph;
    t5_mutex_unlock(v->mx);
    t5_plane_release(old);
    if (v->view) t5ui_delete(v->view);
    v->view = view;
    /* A new plane starts black: put the current frame back up. */
    if (old && v->state == ST_PLAYING) v->seek_ms = (int32_t)v->pos_ms;
}

static void resized(t5ui_event_t *e)
{
    video_t *v = t5ui_event_user(e);
    if (v->plane) make_view(v);
}

static void paint_play(video_t *v)
{
    t5ui_obj_t *icon = t5ui_child(v->play, 0);
    const char *want = v->paused || v->ended ? T5UI_ICON_PLAY_ARROW : T5UI_ICON_PAUSE;
    sa_set_text(icon, want);
}

/* The screen obj is on: its topmost ancestor. */
static t5ui_obj_t *screen_of(t5ui_obj_t *o)
{
    for (t5ui_obj_t *up; (up = t5ui_parent(o));) o = up;
    return o;
}

static void tick(t5ui_timer_t *t, void *user)
{
    (void)t;
    video_t *v = user;
    if (v->state == ST_ERROR) {
        char m[160];
        snprintf(m, sizeof m, "Can't play this video. %s", v->err);
        sa_set_text(v->msg, m);
        t5ui_set_int(v->msg, T5UI_P_HIDDEN, false);
        t5ui_set_int(v->bar, T5UI_P_HIDDEN, true);
        t5ui_set_int(v->top, T5UI_P_HIDDEN, false);
        if (v->lock) t5_wakelock_release(v->lock);
        v->lock = NULL;
        return;
    }
    if (v->state >= ST_READY && !v->ready_seen) {
        v->ready_seen = true;
        make_view(v);
        if (!v->plane) {
            snprintf(v->err, sizeof v->err, "Not enough memory to show it.");
            v->state = ST_ERROR;
            return;
        }
        t5ui_set_int(v->msg, T5UI_P_HIDDEN, true);
        t5ui_set_int(v->slider, T5UI_P_MIN, 0);
        t5ui_set_int(v->slider, T5UI_P_MAX, v->dur_ms ? (int32_t)v->dur_ms : 1);
        char b[16];
        fmt_time(b, sizeof b, v->dur_ms);
        t5ui_set_str(v->dur_lbl, T5UI_P_TEXT, b);
        show_controls(v, true);
        if (v->note[0]) t5ui_snackbar(v->note, NULL, NULL, NULL);
    }
    if (!v->ready_seen) return;
    /* Off screen for a second (the app went to the background, not just a
     * screen animation): pause. */
    if (screen_of(v->root) != t5ui_sys_layer(T5UI_LAYER_SCREEN)) {
        if (!v->away_ms) v->away_ms = t5_time_ms() | 1;
        else if (t5_time_ms() - v->away_ms > 1000) v->paused = true;
    } else {
        v->away_ms = 0;
    }
    bool playing = !v->paused && !v->ended;
    if (playing && !v->lock) v->lock = t5_wakelock_acquire(T5_LOCK_SCREEN, "video");
    if (!playing && v->lock) {
        t5_wakelock_release(v->lock);
        v->lock = NULL;
    }
    paint_play(v);
    if (!v->dragging) {
        t5ui_set_int(v->slider, T5UI_P_VALUE, (int32_t)v->pos_ms);
        char b[16];
        fmt_time(b, sizeof b, v->pos_ms);
        sa_set_text(v->pos_lbl, b);
    }
    if (v->controls && playing && !v->dragging && t5_time_ms() - v->touched_ms > HIDE_MS) show_controls(v, false);
    if (!playing && !v->controls) show_controls(v, true);
}

static void root_clicked(t5ui_event_t *e)
{
    video_t *v = t5ui_event_user(e);
    /* Until it plays, back stays in reach. */
    if (v->ready_seen && v->state != ST_ERROR) show_controls(v, !v->controls);
}

static void play_clicked(t5ui_event_t *e)
{
    video_t *v = t5ui_event_user(e);
    if (v->ended) {
        v->seek_ms = 0;
        v->ended = false;
        v->paused = false;
    } else {
        v->paused = !v->paused;
    }
    v->touched_ms = t5_time_ms();
    paint_play(v);
}

static void slider_event(t5ui_event_t *e)
{
    video_t *v = t5ui_event_user(e);
    t5ui_ev_t c = t5ui_event_kind(e);
    int32_t ms = t5ui_get_int(v->slider, T5UI_P_VALUE, 0);
    v->touched_ms = t5_time_ms();
    if (c == T5UI_EV_PRESS) {
        v->dragging = true;
    } else if (c == T5UI_EV_VALUE) {
        v->seek_ms = ms;                  /* the worker shows the newest */
        char b[16];
        fmt_time(b, sizeof b, (uint32_t)ms);
        sa_set_text(v->pos_lbl, b);
    } else if (c == T5UI_EV_RELEASE || c == T5UI_EV_PRESS_LOST) {
        v->dragging = false;
        v->seek_ms = ms;
        v->pos_ms = (uint32_t)ms;
        if (v->ended) {
            v->ended = false;
            v->paused = false;
        }
    }
}

static void close_clicked(t5ui_event_t *e)
{
    (void)e;
    sa_video_close();
}

static t5ui_obj_t *round_button(t5ui_obj_t *parent, const char *icon, t5ui_event_cb_t cb, void *ud)
{
    t5ui_obj_t *b = t5ui_create(parent, T5UI_BUTTON);
    t5ui_style_clear(b);
    t5ui_set_int(b, T5UI_P_WIDTH, t5ui_dp(44));
    t5ui_set_int(b, T5UI_P_HEIGHT, t5ui_dp(44));
    t5ui_set_int(b, T5UI_P_RADIUS, T5UI_RADIUS_CIRCLE);
    t5ui_set_color(b, T5UI_P_BG_COLOR, 0x000000);
    t5ui_set_int(b, T5UI_P_BG_OPA, 127);                                         /* 50% */
    t5ui_set_int_at(b, T5UI_P_BG_OPA, T5UI_AT(T5UI_PART_MAIN, T5UI_STATE_PRESSED), 204);   /* 80% */
    t5ui_obj_t *l = t5ui_create(b, T5UI_MD_ICON);
    t5ui_set_str(l, T5UI_P_ICON, icon);
    t5ui_set_color(l, T5UI_P_TEXT_COLOR, 0xffffff);
    t5ui_set_int(l, T5UI_P_ALIGN, T5UI_ALIGN_CENTER);
    t5ui_on(b, T5UI_EV_CLICK, cb, ud);
    return b;
}

static t5ui_obj_t *white_label(t5ui_obj_t *parent, const char *text, t5ui_text_t type)
{
    t5ui_obj_t *l = t5ui_create(parent, T5UI_MD_TEXT);
    t5ui_set_int(l, T5UI_P_TEXT_STYLE, type);
    t5ui_set_str(l, T5UI_P_TEXT, text);
    t5ui_set_color(l, T5UI_P_TEXT_COLOR, 0xffffff);
    return l;
}

static void build(video_t *v, t5ui_obj_t *screen)
{
    v->root = t5ui_create(screen, T5UI_BOX);
    t5ui_set_int(v->root, T5UI_P_FLOATING, true);
    t5ui_set_int(v->root, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(v->root, T5UI_P_HEIGHT, T5UI_PCT(100));
    t5ui_set_color(v->root, T5UI_P_BG_COLOR, 0x000000);
    t5ui_set_int(v->root, T5UI_P_BG_OPA, T5UI_OPA_COVER);
    t5ui_set_int(v->root, T5UI_P_SCROLLABLE, false);
    t5ui_set_int(v->root, T5UI_P_CLICKABLE, true);
    t5ui_on(v->root, T5UI_EV_CLICK, root_clicked, v);

    v->msg = white_label(v->root, "Opening...", T5UI_TEXT_BODY1);
    t5ui_set_int(v->msg, T5UI_P_WIDTH, T5UI_PCT(80));
    t5ui_set_int(v->msg, T5UI_P_TEXT_ALIGN, T5UI_TEXT_CENTER);
    t5ui_set_int(v->msg, T5UI_P_ALIGN, T5UI_ALIGN_CENTER);

    /* Top: back and the file's name. Plain containers draw nothing, so only
     * the button and the caption are redrawn over each frame. */
    v->top = t5ui_create(v->root, T5UI_BOX);
    t5ui_set_int(v->top, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(v->top, T5UI_P_HEIGHT, T5UI_CONTENT);
    t5ui_set_int(v->top, T5UI_P_PAD_ALL, t5ui_dp(12));
    t5ui_set_int(v->top, T5UI_P_PAD_COLUMN, t5ui_dp(12));
    t5ui_set_int(v->top, T5UI_P_FLOW, T5UI_ROW);
    t5ui_set_int(v->top, T5UI_P_PLACE_MAIN, T5UI_PLACE_START);
    t5ui_set_int(v->top, T5UI_P_PLACE_CROSS, T5UI_PLACE_CENTER);
    t5ui_set_int(v->top, T5UI_P_PLACE_TRACK, T5UI_PLACE_CENTER);
    t5ui_set_int(v->top, T5UI_P_SCROLLABLE, false);
    t5ui_set_int(v->top, T5UI_P_CLICKABLE, false);
    round_button(v->top, T5UI_ICON_ARROW_BACK, close_clicked, v);
    const char *name = strrchr(v->path, '/');
    t5ui_obj_t *title = white_label(v->top, name ? name + 1 : v->path, T5UI_TEXT_SUBTITLE1);
    t5ui_set_color(title, T5UI_P_BG_COLOR, 0x000000);
    t5ui_set_int(title, T5UI_P_BG_OPA, 127);                     /* 50% */
    t5ui_set_int(title, T5UI_P_RADIUS, T5UI_RADIUS_CIRCLE);
    t5ui_set_int(title, T5UI_P_PAD_LEFT, t5ui_dp(12));
    t5ui_set_int(title, T5UI_P_PAD_RIGHT, t5ui_dp(12));
    t5ui_set_int(title, T5UI_P_PAD_TOP, t5ui_dp(4));
    t5ui_set_int(title, T5UI_P_PAD_BOTTOM, t5ui_dp(4));
    t5ui_set_int(title, T5UI_P_LONG_MODE, T5UI_LONG_DOTS);
    t5ui_set_int(title, T5UI_P_GROW, 0);
    t5ui_set_int(title, T5UI_P_MAX_WIDTH, T5UI_PCT(70));

    /* Bottom: play, position, the seek bar, length. */
    v->bar = t5ui_create(v->root, T5UI_BOX);
    t5ui_set_int(v->bar, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(v->bar, T5UI_P_HEIGHT, t5ui_dp(64));
    t5ui_set_int(v->bar, T5UI_P_ALIGN, T5UI_ALIGN_BOTTOM_MID);
    t5ui_set_color(v->bar, T5UI_P_BG_COLOR, 0x000000);
    t5ui_set_int(v->bar, T5UI_P_BG_OPA, 153);                    /* 60% */
    t5ui_set_int(v->bar, T5UI_P_PAD_LEFT, t5ui_dp(12));
    t5ui_set_int(v->bar, T5UI_P_PAD_RIGHT, t5ui_dp(12));
    t5ui_set_int(v->bar, T5UI_P_PAD_COLUMN, t5ui_dp(16));
    t5ui_set_int(v->bar, T5UI_P_FLOW, T5UI_ROW);
    t5ui_set_int(v->bar, T5UI_P_PLACE_MAIN, T5UI_PLACE_START);
    t5ui_set_int(v->bar, T5UI_P_PLACE_CROSS, T5UI_PLACE_CENTER);
    t5ui_set_int(v->bar, T5UI_P_PLACE_TRACK, T5UI_PLACE_CENTER);
    t5ui_set_int(v->bar, T5UI_P_SCROLLABLE, false);
    v->play = round_button(v->bar, T5UI_ICON_PAUSE, play_clicked, v);
    t5ui_set_int(v->play, T5UI_P_BG_OPA, T5UI_OPA_TRANSP);
    v->pos_lbl = white_label(v->bar, "0:00", T5UI_TEXT_BODY2);
    v->slider = t5ui_create(v->bar, T5UI_MD_SLIDER);
    t5ui_set_int(v->slider, T5UI_P_MIN, 0);
    t5ui_set_int(v->slider, T5UI_P_MAX, 1);
    t5ui_set_int(v->slider, T5UI_P_VALUE, 0);
    t5ui_set_int(v->slider, T5UI_P_GROW, 1);
    t5ui_on(v->slider, T5UI_EV_PRESS, slider_event, v);
    t5ui_on(v->slider, T5UI_EV_VALUE, slider_event, v);
    t5ui_on(v->slider, T5UI_EV_RELEASE, slider_event, v);
    t5ui_on(v->slider, T5UI_EV_PRESS_LOST, slider_event, v);
    v->dur_lbl = white_label(v->bar, "0:00", T5UI_TEXT_BODY2);
    t5ui_set_int(v->bar, T5UI_P_HIDDEN, true);
    t5ui_on(v->root, T5UI_EV_RESIZE, resized, v);
}

bool sa_video_open(t5ui_obj_t *screen, const char *path, void (*on_close)(void))
{
    sa_video_close();
    if (!sa_video_can_open(path)) return false;
    video_t *v = calloc(1, sizeof *v);
    if (!v) return false;
    snprintf(v->path, sizeof v->path, "%s", path);
    v->on_close = on_close;
    v->seek_ms = -1;
    v->mx = t5_mutex_create();
    v->exited = xSemaphoreCreateBinary();
    if (!v->mx || !v->exited) {
        if (v->exited) vSemaphoreDelete(v->exited);
        if (v->mx) t5_mutex_delete(v->mx);
        free(v);
        sa_report(T5_ERR_NO_MEM, "open this video");
        return false;
    }
    build(v, screen);
    v->was_fullscreen = t5_shell_fullscreen();
    t5_shell_set_fullscreen(true);
    v->timer = t5ui_timer(TICK_MS, tick, v);
    if (t5_task_create("video", worker, v, 6144, 6, T5_CORE_ANY, NULL) != T5_OK) {
        t5ui_timer_delete(v->timer);
        t5ui_delete(v->root);
        t5_shell_set_fullscreen(v->was_fullscreen);
        vSemaphoreDelete(v->exited);
        t5_mutex_delete(v->mx);
        free(v);
        sa_report(T5_ERR_NO_MEM, "open this video");
        return false;
    }
    s_v = v;
    return true;
}

void sa_video_close(void)
{
    video_t *v = s_v;
    if (!v) return;
    s_v = NULL;
    v->quit = true;
    t5ui_timer_delete(v->timer);
    t5ui_delete(v->root);             /* the view lets go of the plane; the worker sees it closed */
    if (v->lock) t5_wakelock_release(v->lock);
    t5_shell_set_fullscreen(v->was_fullscreen);
    void (*cb)(void) = v->on_close;
    /* The worker stops within a frame (it checks quit while it waits). */
    if (xSemaphoreTake(v->exited, pdMS_TO_TICKS(3000)) == pdTRUE) {
        t5_plane_release(v->plane);
        heap_caps_free(v->v);
        heap_caps_free(v->a);
        vSemaphoreDelete(v->exited);
        t5_mutex_delete(v->mx);
        free(v);
    } else {
        T5_LOGE("t5.video", "player task didn't stop: leaking it");
    }
    if (cb) cb();
}
