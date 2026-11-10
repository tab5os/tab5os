/* The player (player.h). */
#include "player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <t5/audio.h>
#include <t5/shell.h>
#include "decoder.h"

#define CHUNK 1024                 /* frames per decode */
#define STREAM_MS 400              /* the output stream's buffer */
#define PREV_RESTART_MS 3000
#define TASK_STACK (48 * 1024)     /* minimp3 decodes on the stack */

static struct {
    t5_app_t *app;
    t5_mutex_t mx;
    volatile bool run, done;
    t5_media_session_t *session;
    bool session_failed;
    /* the queue */
    char **paths;
    int n, cap;
    int *order;                    /* play order -> paths index */
    int cur;                       /* play-order position, -1 if none */
    bool shuffle;
    repeat_t repeat;
    /* requests for the task */
    bool want_load;                /* (re)open order[cur] */
    bool want_play;                /* play (true) or pause once loaded */
    bool want_seek;
    uint32_t seek_ms;
    uint32_t start_ms;             /* where a load starts (a restored position) */
    /* what's playing (task side, published under mx) */
    bool playing;
    bool loaded;
    char path[256];
    tags_t tags;
    uint32_t pos_ms, dur_ms;
    uint32_t version;
} s;

static void lock(void) { t5_mutex_lock(s.mx); }
static void unlock(void) { t5_mutex_unlock(s.mx); }

/* ---- the queue (s.mx held) ------------------------------------------------------- */
static void shuffle_from(int from)
{
    for (int i = s.n - 1; i > from; i--) {
        int j = from + rand() % (i - from + 1);
        int t = s.order[i];
        s.order[i] = s.order[j];
        s.order[j] = t;
    }
}

static void clear_queue(void)
{
    for (int i = 0; i < s.n; i++) free(s.paths[i]);
    s.n = 0;
    s.cur = -1;
}

static bool grow(int need)
{
    if (need <= s.cap) return true;
    int cap = s.cap ? s.cap : 64;
    while (cap < need) cap *= 2;
    char **p = realloc(s.paths, (size_t)cap * sizeof *p);
    if (!p) return false;
    s.paths = p;
    int *o = realloc(s.order, (size_t)cap * sizeof *o);
    if (!o) return false;
    s.order = o;
    s.cap = cap;
    return true;
}

static void changed(void)
{
    s.version++;
}

/* ---- media session ---------------------------------------------------------------- */
/* Opened with the first track, so the standby screen has no empty card
 * before anything is queued. */
static void session_open(void)
{
    if (s.session || s.session_failed) return;
    if (t5_media_session_open(&s.session) != T5_OK) {
        s.session = NULL;
        s.session_failed = true;
        T5_IGNORE(t5_shell_toast("Another app is playing audio"));
    }
}

static void publish_info(void)
{
    session_open();
    if (!s.session) return;
    t5_media_info_t mi = { 0 };
    tag_copy(mi.title, sizeof mi.title, s.tags.title, sizeof s.tags.title);   /* cut on a character */
    tag_copy(mi.artist, sizeof mi.artist, s.tags.artist, sizeof s.tags.artist);
    tag_copy(mi.album, sizeof mi.album, s.tags.album, sizeof s.tags.album);
    mi.duration_ms = s.dur_ms;
    T5_IGNORE(t5_media_session_set_info(s.session, &mi));     /* a stale card is all a failure costs */
}

static void publish_state(void)
{
    if (!s.session) return;
    T5_IGNORE(t5_media_session_set_state(s.session,
                                         !s.loaded ? T5_MEDIA_STOPPED : s.playing ? T5_MEDIA_PLAYING : T5_MEDIA_PAUSED,
                                         s.pos_ms));
}

/* ---- the task ----------------------------------------------------------------------- */
typedef struct {
    dec_t *dec;
    t5_audio_out_t *out;
    uint32_t out_rate;
    uint8_t out_ch;
    bool out_paused;
    int16_t *pcm;
    size_t have, at;               /* decoded frames not yet written */
} eng_t;

static size_t file_read(void *ctx, uint32_t off, void *buf, size_t n)
{
    FILE *f = ctx;
    if (fseek(f, (long)off, SEEK_SET)) return 0;
    return fread(buf, 1, n, f);
}

static bool read_tags(const char *path, tags_t *t)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    tag_src_t src = { file_read, f, size > 0 ? (uint32_t)size : 0 };
    bool ok = tags_read(&src, t);
    fclose(f);
    if (ok && !t->title[0]) {                          /* the file name, without the extension */
        const char *b = strrchr(path, '/');
        b = b ? b + 1 : path;
        const char *dot = strrchr(b, '.');
        tag_copy(t->title, sizeof t->title, b, dot ? (size_t)(dot - b) : strlen(b));
    }
    return ok;
}

static void close_out(eng_t *e)
{
    if (e->out) T5_IGNORE(t5_audio_out_close(e->out));
    e->out = NULL;
    e->have = e->at = 0;
}

static bool open_out(eng_t *e)
{
    uint32_t rate = dec_rate(e->dec);
    uint8_t ch = dec_channels(e->dec);
    if (e->out && rate == e->out_rate && ch == e->out_ch) return true;
    close_out(e);
    t5_audio_out_cfg_t c = { .rate = rate, .channels = ch, .buffer_ms = STREAM_MS };
    if (t5_audio_out_open(&c, &e->out) != T5_OK) return false;
    e->out_rate = rate;
    e->out_ch = ch;
    e->out_paused = false;
    return true;
}

/* Open the track at s.cur. keep_stream: the old track ended by itself (gapless). */
static bool load(eng_t *e, bool keep_stream)
{
    char path[256];
    uint32_t start;
    lock();
    bool any = s.cur >= 0 && s.cur < s.n;
    if (any) snprintf(path, sizeof path, "%s", s.paths[s.order[s.cur]]);
    start = s.start_ms;
    s.start_ms = 0;
    unlock();
    dec_close(e->dec);
    e->dec = NULL;
    if (!keep_stream) close_out(e);
    if (!any) return false;
    tags_t t;
    if (!read_tags(path, &t) || !(e->dec = dec_open(path, &t))) return false;
    if (start) dec_seek(e->dec, start);
    lock();
    snprintf(s.path, sizeof s.path, "%s", path);
    s.tags = t;
    s.dur_ms = t.duration_ms;
    s.pos_ms = start;
    s.loaded = true;
    changed();
    publish_info();
    publish_state();                                   /* the new track's position (a gapless change too) */
    unlock();
    return true;
}

/* The next play-order position after the current one ends by itself, or -1. */
static int after_end(void)
{
    if (s.repeat == REPEAT_ONE) return s.cur;
    if (s.cur + 1 < s.n) return s.cur + 1;
    if (s.repeat == REPEAT_ALL && s.n) {
        if (s.shuffle) shuffle_from(0);
        return 0;
    }
    return -1;
}

static void media_command(const t5_media_cmd_t *c)
{
    switch (c->type) {
    case T5_MEDIA_CMD_PLAY: player_play(); break;
    case T5_MEDIA_CMD_PAUSE: player_pause(); break;
    case T5_MEDIA_CMD_TOGGLE: player_toggle(); break;
    case T5_MEDIA_CMD_NEXT: player_next(); break;
    case T5_MEDIA_CMD_PREVIOUS: player_prev(); break;
    case T5_MEDIA_CMD_STOP: player_pause(); break;
    case T5_MEDIA_CMD_SEEK: player_seek(c->position_ms); break;
    }
}

static void task(void *arg)
{
    (void)arg;
    eng_t e = { 0 };
    e.pcm = malloc(CHUNK * 2 * sizeof(int16_t));
    int failures = 0;
    while (s.run && t5_app_running(s.app) && e.pcm) {
        t5_media_cmd_t mc;
        if (s.session && t5_media_session_next(s.session, &mc, 0) == T5_OK) media_command(&mc);

        lock();
        bool want_load = s.want_load, want_seek = s.want_seek, want_play = s.want_play;
        uint32_t seek_ms = s.seek_ms;
        s.want_load = s.want_seek = false;
        unlock();

        if (want_load) {
            if (load(&e, false)) failures = 0;
            else if (s.n && ++failures < s.n) {            /* skip what won't open */
                lock();
                int nx = after_end();
                if (nx == s.cur && s.repeat == REPEAT_ONE) nx = s.cur + 1 < s.n ? s.cur + 1 : -1;
                if (nx >= 0) s.cur = nx, s.want_load = true;
                changed();
                unlock();
                if (nx < 0) T5_IGNORE(t5_shell_toast("Couldn't play that track"));
                continue;
            } else {
                T5_IGNORE(t5_shell_toast("Couldn't play those tracks"));
            }
        }
        if (want_seek && e.dec) {
            if (dec_seek(e.dec, seek_ms)) close_out(&e);    /* drop what's queued from before */
            lock();
            s.pos_ms = seek_ms;
            publish_state();
            unlock();
        }
        if (!e.dec || !want_play) {
            if (e.out && !e.out_paused) T5_IGNORE(t5_audio_out_pause(e.out, true));
            e.out_paused = true;
            lock();
            if (s.playing) {
                s.playing = false;
                changed();
                publish_state();
            }
            unlock();
            t5_delay_ms(40);
            continue;
        }
        if (!open_out(&e)) {
            T5_IGNORE(t5_shell_toast("Audio output is busy"));
            lock();
            s.want_play = false;
            unlock();
            continue;
        }
        if (e.out_paused) {
            T5_IGNORE(t5_audio_out_pause(e.out, false));
            e.out_paused = false;
        }
        lock();
        if (!s.playing) {
            s.playing = true;
            changed();
            publish_state();
        }
        unlock();

        if (e.at == e.have) {
            int n = dec_read(e.dec, e.pcm, CHUNK);
            if (n <= 0) {                                    /* the end: on to the next */
                lock();
                int nx = after_end();
                if (nx >= 0) s.cur = nx;
                changed();
                unlock();
                if (nx >= 0) {
                    if (!load(&e, true)) {                   /* the same stream if it fits: no gap */
                        lock();
                        s.want_load = true;                  /* the skip-what-won't-open path */
                        unlock();
                    }
                } else {
                    T5_IGNORE(t5_audio_out_drain(e.out, STREAM_MS + 500));
                    close_out(&e);
                    lock();
                    s.cur = s.n ? 0 : -1;
                    s.want_play = false;
                    s.want_load = s.n > 0;
                    unlock();
                }
                continue;
            }
            e.have = (size_t)n;
            e.at = 0;
        }
        size_t fb = e.out_ch * sizeof(int16_t);
        int w = t5_audio_out_write(e.out, (const uint8_t *)e.pcm + e.at * fb, (e.have - e.at) * fb, 50);
        if (w > 0) e.at += (size_t)w / fb;
        uint32_t queued = t5_audio_out_queued_ms(e.out) + (uint32_t)((e.have - e.at) * 1000 / e.out_rate);
        uint32_t dpos = dec_pos_ms(e.dec);
        lock();
        s.pos_ms = dpos > queued ? dpos - queued : 0;
        unlock();
    }
    close_out(&e);
    dec_close(e.dec);
    free(e.pcm);
    s.done = true;
}

/* ---- API --------------------------------------------------------------------------- */
bool player_start(t5_app_t *app)
{
    s.app = app;
    s.mx = t5_mutex_create();
    s.cur = -1;
    s.run = true;
    srand((unsigned)t5_time_us());
    return s.mx && t5_task_create("music.play", task, NULL, TASK_STACK, 5, T5_CORE_ANY, NULL) == T5_OK;
}

void player_stop(void)
{
    s.run = false;
    for (int i = 0; i < 100 && !s.done; i++) t5_delay_ms(10);
}

void player_set_queue(const char *const *paths, int n, int start, bool shuffle)
{
    lock();
    clear_queue();
    if (n > 0 && grow(n)) {
        for (int i = 0; i < n; i++) {
            s.paths[s.n] = strdup(paths[i]);
            if (!s.paths[s.n]) break;
            s.order[s.n] = s.n;
            s.n++;
        }
    }
    if (start < 0 || start >= s.n) start = 0;
    s.shuffle = shuffle;
    if (s.n) {
        if (shuffle) {                                     /* start first, the rest shuffled */
            s.order[0] = start;
            s.order[start] = 0;
            shuffle_from(1);
            s.cur = 0;
        } else {
            s.cur = start;
        }
    }
    s.want_load = s.want_play = s.n > 0;
    changed();
    unlock();
}

void player_add(const char *path, bool next)
{
    lock();
    if (grow(s.n + 1)) {
        char *p = strdup(path);
        if (p) {
            s.paths[s.n] = p;
            int at = next && s.cur >= 0 ? s.cur + 1 : s.n;
            memmove(s.order + at + 1, s.order + at, (size_t)(s.n - at) * sizeof *s.order);
            s.order[at] = s.n;
            s.n++;
            if (s.cur < 0) {
                s.cur = 0;
                s.want_load = true;
            }
            changed();
        }
    }
    unlock();
}

void player_remove(int qpos)
{
    lock();
    if (qpos >= 0 && qpos < s.n) {
        int idx = s.order[qpos];
        free(s.paths[idx]);
        /* close the gap in paths, renumber order */
        memmove(s.paths + idx, s.paths + idx + 1, (size_t)(s.n - idx - 1) * sizeof *s.paths);
        memmove(s.order + qpos, s.order + qpos + 1, (size_t)(s.n - qpos - 1) * sizeof *s.order);
        s.n--;
        for (int i = 0; i < s.n; i++) {
            if (s.order[i] > idx) s.order[i]--;
        }
        if (qpos < s.cur) s.cur--;
        else if (qpos == s.cur) {
            if (s.cur >= s.n) s.cur = s.n ? 0 : -1;
            s.want_load = true;
            if (s.cur < 0) s.want_play = false;
        }
        changed();
    }
    unlock();
}

void player_retain(bool (*keep)(const char *path))
{
    lock();
    int playing = s.cur >= 0 && s.cur < s.n ? s.order[s.cur] : -1;
    int *map = malloc((size_t)(s.n ? s.n : 1) * sizeof *map);   /* old index -> new, or -1 */
    if (!map) {
        unlock();
        return;
    }
    int n = 0;
    for (int i = 0; i < s.n; i++) {
        if (keep(s.paths[i])) {
            map[i] = n;
            s.paths[n++] = s.paths[i];
        } else {
            map[i] = -1;
            free(s.paths[i]);
        }
    }
    if (n != s.n) {
        /* the play order without the dropped ones; the current one, or the next still there */
        int o = 0, cur = -1;
        for (int i = 0; i < s.n; i++) {
            int m = map[s.order[i]];
            if (m < 0) continue;
            if (cur < 0 && i >= s.cur) cur = o;
            s.order[o++] = m;
        }
        bool moved = playing < 0 || map[playing] < 0;
        s.n = n;
        s.cur = n ? (cur >= 0 ? cur : 0) : -1;
        if (moved) {
            s.loaded = false;
            s.want_load = s.cur >= 0;
            if (s.cur < 0) s.want_play = false;
        }
        changed();
    }
    free(map);
    unlock();
}

void player_jump(int qpos)
{
    lock();
    if (qpos >= 0 && qpos < s.n) {
        s.cur = qpos;
        s.want_load = s.want_play = true;
        changed();
    }
    unlock();
}

void player_play(void)
{
    lock();
    if (s.cur >= 0) s.want_play = true;
    unlock();
}

void player_pause(void)
{
    lock();
    s.want_play = false;
    unlock();
}

void player_toggle(void)
{
    lock();
    if (s.cur >= 0) s.want_play = !s.want_play;
    unlock();
}

void player_next(void)
{
    lock();
    if (s.n) {
        s.cur = s.cur + 1 < s.n ? s.cur + 1 : 0;
        if (s.cur == 0 && s.shuffle && s.repeat == REPEAT_ALL) shuffle_from(0);
        s.want_load = true;
        changed();
    }
    unlock();
}

void player_prev(void)
{
    lock();
    if (s.n) {
        if (s.loaded && s.pos_ms > PREV_RESTART_MS) {
            s.want_seek = true;
            s.seek_ms = 0;
        } else {
            s.cur = s.cur > 0 ? s.cur - 1 : (s.repeat == REPEAT_ALL ? s.n - 1 : 0);
            s.want_load = true;
            changed();
        }
    }
    unlock();
}

void player_seek(uint32_t ms)
{
    lock();
    s.want_seek = true;
    s.seek_ms = s.dur_ms && ms > s.dur_ms ? s.dur_ms : ms;
    s.pos_ms = s.seek_ms;                                  /* show it at once */
    unlock();
}

void player_set_shuffle(bool on)
{
    lock();
    if (on != s.shuffle && s.n) {
        int playing = s.cur >= 0 ? s.order[s.cur] : -1;
        if (on) {                                          /* the current first, the rest shuffled */
            for (int i = 0; i < s.n; i++) s.order[i] = i;
            if (playing >= 0) {
                s.order[0] = playing;
                s.order[playing] = 0;
                s.cur = 0;
            }
            shuffle_from(playing >= 0 ? 1 : 0);
        } else {
            for (int i = 0; i < s.n; i++) s.order[i] = i;
            s.cur = playing;
        }
    }
    s.shuffle = on;
    changed();
    unlock();
}

void player_set_repeat(repeat_t r)
{
    lock();
    s.repeat = r;
    changed();
    unlock();
}

void player_get(player_state_t *o)
{
    lock();
    o->version = s.version;
    o->has_track = s.loaded && s.cur >= 0;
    o->playing = s.playing;
    o->pos_ms = s.pos_ms;
    o->dur_ms = s.dur_ms;
    o->shuffle = s.shuffle;
    o->repeat = s.repeat;
    o->qpos = s.cur;
    o->qlen = s.n;
    snprintf(o->path, sizeof o->path, "%s", s.path);
    o->tags = s.tags;
    unlock();
}

bool player_queue_item(int qpos, char *out, size_t cap)
{
    lock();
    bool ok = qpos >= 0 && qpos < s.n;
    if (ok) snprintf(out, cap, "%s", s.paths[s.order[qpos]]);
    unlock();
    return ok;
}

/* "musicq 1\n<shuffle> <repeat> <cur> <pos_ms>\n<order...>\n<path>\n..." */
char *player_save_text(void)
{
    lock();
    size_t cap = 64 + (size_t)s.n * 12;
    for (int i = 0; i < s.n; i++) cap += strlen(s.paths[i]) + 1;
    char *t = malloc(cap);
    if (t) {
        size_t o = (size_t)snprintf(t, cap, "musicq 1\n%d %d %d %u\n", s.shuffle, (int)s.repeat, s.cur, (unsigned)s.pos_ms);
        for (int i = 0; i < s.n; i++) o += (size_t)snprintf(t + o, cap - o, "%d%c", s.order[i], i + 1 < s.n ? ' ' : '\n');
        if (!s.n) o += (size_t)snprintf(t + o, cap - o, "\n");
        for (int i = 0; i < s.n; i++) o += (size_t)snprintf(t + o, cap - o, "%s\n", s.paths[i]);
    }
    unlock();
    return t;
}

void player_load_text(const char *text)
{
    if (strncmp(text, "musicq 1\n", 9)) return;
    const char *p = text + 9;
    int sh = 0, rep = 0, cur = -1;
    unsigned pos = 0;
    if (sscanf(p, "%d %d %d %u", &sh, &rep, &cur, &pos) != 4) return;
    p = strchr(p, '\n');
    if (!p) return;
    const char *orders = ++p;
    p = strchr(p, '\n');
    if (!p) return;
    p++;
    lock();
    clear_queue();
    while (*p) {                                           /* the paths */
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n && grow(s.n + 1)) {
            char *q = malloc(n + 1);
            if (!q) break;
            memcpy(q, p, n);
            q[n] = 0;
            s.paths[s.n] = q;
            s.order[s.n] = s.n;
            s.n++;
        }
        if (!e) break;
        p = e + 1;
    }
    /* the play order, if it is a permutation of what we read */
    int i = 0;
    bool ok = true;
    char *seen = calloc((size_t)s.n + 1, 1);
    for (const char *o = orders; *o && *o != '\n' && i < s.n && seen;) {
        char *end;
        long v = strtol(o, &end, 10);
        if (end == o || v < 0 || v >= s.n || seen[v]) {
            ok = false;
            break;
        }
        seen[v] = 1;
        s.order[i++] = (int)v;
        o = end;
        while (*o == ' ') o++;
    }
    free(seen);
    if (!ok || i != s.n) {
        for (int k = 0; k < s.n; k++) s.order[k] = k;
    }
    s.shuffle = sh != 0;
    s.repeat = rep >= REPEAT_OFF && rep <= REPEAT_ONE ? (repeat_t)rep : REPEAT_OFF;
    s.cur = cur >= 0 && cur < s.n ? cur : (s.n ? 0 : -1);
    s.start_ms = s.cur >= 0 ? pos : 0;
    s.want_load = s.cur >= 0;
    s.want_play = false;
    changed();
    unlock();
}
