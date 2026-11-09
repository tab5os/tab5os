/* Music: a small player built on the playback API (t5_audio/media.h).
 *
 * It plays WAV files from the SD card's Music folder (once the person allows
 * shared files) and three short built-in tunes, so it works without any.
 * It publishes what is playing through a media session, so the standby
 * screen's music card shows it and its buttons (previous, play/pause, next)
 * come back here as commands, the same ones its own buttons send. Music
 * keeps playing under the standby screen: the app stays in front, and its
 * open stream keeps the device awake.
 */
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <t5/app.h>
#include <t5/audio.h>
#include <t5/shell.h>
#include <t5/ui.h>

#define MAX_TRACKS 24
#define MUSIC_DIR "/sd/Music"
#define TUNE_RATE 16000
#define TUNE_SECONDS 20

typedef struct {
    char title[T5_MEDIA_TEXT_LEN];
    char path[160];           /* "" for a built-in tune */
    int tune;
} track_t;

static struct {
    t5_app_t *app;
    t5_mutex_t mx;
    track_t tracks[MAX_TRACKS];
    int n, cur;
    t5_media_session_t *session;
    t5_audio_player_t *player;
    uint8_t *tune_wav;        /* the built-in tune playing, as a WAV in memory */
    bool playing;
    lv_obj_t *title, *artist, *bar, *play, *list;
} s;

/* ---- built-in tunes: a few bars of sine notes, made into a WAV --------------- */
static const uint8_t s_notes[3][16] = {
    { 60, 62, 64, 65, 67, 69, 71, 72, 71, 69, 67, 65, 64, 62, 60, 0 },
    { 57, 60, 64, 69, 64, 60, 57, 0, 55, 59, 62, 67, 62, 59, 55, 0 },
    { 64, 64, 65, 67, 67, 65, 64, 62, 60, 60, 62, 64, 64, 62, 62, 0 },
};
static const char *const s_tune_names[3] = { "Scale Up and Down", "Arpeggios", "Ode to Joy" };

static uint8_t *make_tune(int t, size_t *len)
{
    const uint32_t frames = TUNE_RATE * TUNE_SECONDS, data = frames * 2;
    uint8_t *w = malloc(44 + data);
    if (!w) return NULL;
    const uint32_t riff = 36 + data, fmtlen = 16, rate = TUNE_RATE, bps = TUNE_RATE * 2;
    const uint16_t fmt = 1, ch = 1, block = 2, bits = 16;
    memcpy(w, "RIFF", 4); memcpy(w + 4, &riff, 4); memcpy(w + 8, "WAVEfmt ", 8); memcpy(w + 16, &fmtlen, 4);
    memcpy(w + 20, &fmt, 2); memcpy(w + 22, &ch, 2); memcpy(w + 24, &rate, 4); memcpy(w + 28, &bps, 4);
    memcpy(w + 32, &block, 2); memcpy(w + 34, &bits, 2); memcpy(w + 36, "data", 4); memcpy(w + 40, &data, 4);
    int16_t *pcm = (int16_t *)(w + 44);
    const uint32_t note_frames = TUNE_RATE / 4;    /* quarter-second notes, looped */
    float phase = 0;
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t k = (i / note_frames) % 16, in = i % note_frames;
        uint8_t midi = s_notes[t][k];
        float env = in < 160 ? in / 160.0f : (float)(note_frames - in) / note_frames;   /* no clicks */
        if (midi) {
            phase += 2.0f * (float)M_PI * 440.0f * powf(2.0f, (midi - 69) / 12.0f) / TUNE_RATE;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }
        pcm[i] = midi ? (int16_t)(sinf(phase) * 6000.0f * env) : 0;
    }
    *len = 44 + data;
    return w;
}

/* ---- playback (any task; s.mx held by callers) -------------------------------- */
static void publish(t5_media_state_t st)
{
    t5_media_info_t info = { 0 };
    snprintf(info.title, sizeof info.title, "%s", s.tracks[s.cur].title);
    snprintf(info.artist, sizeof info.artist, "%s", s.tracks[s.cur].path[0] ? "Music folder" : "Tab5 demo tunes");
    snprintf(info.album, sizeof info.album, "Music example");
    uint32_t pos = 0;
    if (s.player) t5_audio_player_state(s.player, &pos);      /* only the position */
    info.duration_ms = s.tracks[s.cur].path[0] ? 0 : TUNE_SECONDS * 1000;
    T5_IGNORE(t5_media_session_set_info(s.session, &info));      /* a stale card is all a failure costs */
    T5_IGNORE(t5_media_session_set_state(s.session, st, pos));
}

static void stop_locked(void)
{
    if (s.player) T5_IGNORE(t5_audio_player_close(s.player));   /* ours: nothing to report */
    s.player = NULL;
    free(s.tune_wav);
    s.tune_wav = NULL;
    s.playing = false;
}

static void start_locked(int i)
{
    stop_locked();
    s.cur = (i % s.n + s.n) % s.n;
    track_t *t = &s.tracks[s.cur];
    t5_err_t e;
    if (t->path[0]) {
        e = t5_audio_play_file(t->path, T5_AUDIO_USAGE_MEDIA, &s.player);
    } else {
        size_t len = 0;
        s.tune_wav = make_tune(t->tune, &len);
        e = s.tune_wav ? t5_audio_play_mem(s.tune_wav, len, T5_AUDIO_USAGE_MEDIA, &s.player) : T5_ERR_NO_MEM;
    }
    s.playing = e == T5_OK;
    if (e != T5_OK) T5_IGNORE(t5_shell_toast("Can't play that track"));
    publish(s.playing ? T5_MEDIA_PLAYING : T5_MEDIA_STOPPED);
}

static void command(t5_media_cmd_type_t c)
{
    t5_mutex_lock(s.mx);
    if (c == T5_MEDIA_CMD_TOGGLE) c = s.playing ? T5_MEDIA_CMD_PAUSE : T5_MEDIA_CMD_PLAY;
    switch (c) {
    case T5_MEDIA_CMD_PLAY:
        if (!s.player) start_locked(s.cur);
        else if (t5_audio_player_state(s.player, NULL) == T5_AUDIO_PLAYER_ENDED) start_locked(s.cur + 1);
        else if (t5_audio_player_pause(s.player, false) == T5_OK) s.playing = true;
        publish(T5_MEDIA_PLAYING);
        break;
    case T5_MEDIA_CMD_PAUSE:
        if (s.player && t5_audio_player_pause(s.player, true) == T5_OK) s.playing = false;
        publish(T5_MEDIA_PAUSED);
        break;
    case T5_MEDIA_CMD_NEXT: start_locked(s.cur + 1); break;
    case T5_MEDIA_CMD_PREVIOUS: start_locked(s.cur - 1); break;
    case T5_MEDIA_CMD_STOP:
        stop_locked();
        publish(T5_MEDIA_STOPPED);
        break;
    default: break;
    }
    t5_mutex_unlock(s.mx);
}

/* Commands from the system (the standby card), and the end of each track. */
static void worker(void *arg)
{
    (void)arg;
    while (t5_app_running(s.app)) {
        t5_media_cmd_t c;
        if (t5_media_session_next(s.session, &c, 250) == T5_OK) command(c.type);
        t5_mutex_lock(s.mx);
        /* Paused at the very end (the tail already out): stay paused, Play moves on. */
        bool ended = s.playing && s.player && t5_audio_player_state(s.player, NULL) == T5_AUDIO_PLAYER_ENDED;
        if (ended) start_locked(s.cur + 1);
        t5_mutex_unlock(s.mx);
    }
}

/* ---- UI ------------------------------------------------------------------------ */
static void refresh(lv_timer_t *t)
{
    (void)t;
    t5_mutex_lock(s.mx);
    uint32_t pos = 0;
    if (s.player) t5_audio_player_state(s.player, &pos);
    lv_label_set_text(s.title, s.tracks[s.cur].title);
    lv_label_set_text(s.artist, s.tracks[s.cur].path[0] ? "Music folder" : "Tab5 demo tunes");
    lv_label_set_text(lv_obj_get_child(s.play, 0), s.playing ? MD_ICON_PAUSE : MD_ICON_PLAY_ARROW);
    bool tune = !s.tracks[s.cur].path[0];
    lv_bar_set_value(s.bar, tune ? (int32_t)(pos / (TUNE_SECONDS * 10)) : 0, LV_ANIM_OFF);
    t5_mutex_unlock(s.mx);
}

static void button(lv_event_t *e)
{
    command((t5_media_cmd_type_t)(intptr_t)lv_event_get_user_data(e));
}

static lv_obj_t *ctl(lv_obj_t *row, const char *icon, t5_media_cmd_type_t c)
{
    lv_obj_t *b = md_fab_create(row, icon, false);
    lv_obj_add_event_cb(b, button, LV_EVENT_CLICKED, (void *)(intptr_t)c);
    return b;
}

static void row_bind(md_list_row_t *row, uint32_t i, void *ud)
{
    (void)ud;
    lv_label_set_text(row->primary, s.tracks[i].title);
    if (row->secondary) lv_label_set_text(row->secondary, s.tracks[i].path[0] ? "Music folder" : "Built in");
}

static void row_click(uint32_t i, void *ud)
{
    (void)ud;
    t5_mutex_lock(s.mx);
    start_locked((int)i);
    t5_mutex_unlock(s.mx);
}

static void add_folder(void)
{
    DIR *d = opendir(MUSIC_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && s.n < MAX_TRACKS) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcasecmp(e->d_name + l - 4, ".wav")) continue;
        track_t *t = &s.tracks[s.n++];
        snprintf(t->path, sizeof t->path, MUSIC_DIR "/%s", e->d_name);
        snprintf(t->title, sizeof t->title, "%.*s", (int)(l - 4), e->d_name);
    }
    closedir(d);
}

static void allowed(t5_app_t *app, uint32_t perm, bool granted, void *user)
{
    (void)app; (void)perm; (void)user;
    if (!granted) return;
    t5_mutex_lock(s.mx);
    add_folder();
    t5_mutex_unlock(s.mx);
    md_list_set_count(s.list, (uint32_t)s.n);
    md_list_refresh(s.list);
}

static void on_create(t5_app_t *app)
{
    s.app = app;
    s.mx = t5_mutex_create();
    for (int i = 0; i < 3; i++) {
        snprintf(s.tracks[i].title, sizeof s.tracks[i].title, "%s", s_tune_names[i]);
        s.tracks[i].tune = i;
    }
    s.n = 3;
    if (t5_media_session_open(&s.session) != T5_OK) {
        T5_IGNORE(t5_shell_toast("Another player has the media session"));
    }
    lv_obj_t *scr = t5_app_screen(app);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(scr, T5_DP(16), 0);
    lv_obj_set_style_pad_row(scr, T5_DP(8), 0);
    s.title = md_label_create(scr, "", MD_TYPE_HEADLINE5);
    s.artist = md_label_create(scr, "", MD_TYPE_BODY2);
    s.bar = lv_bar_create(scr);
    lv_obj_set_size(s.bar, LV_PCT(100), T5_DP(4));
    lv_bar_set_range(s.bar, 0, 100);
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, T5_DP(24), 0);
    ctl(row, MD_ICON_SKIP_PREVIOUS, T5_MEDIA_CMD_PREVIOUS);
    s.play = ctl(row, MD_ICON_PLAY_ARROW, T5_MEDIA_CMD_TOGGLE);
    ctl(row, MD_ICON_SKIP_NEXT, T5_MEDIA_CMD_NEXT);
    s.list = md_list_create(scr, (uint32_t)s.n, 64, row_bind, row_click, NULL);
    lv_obj_set_width(s.list, LV_PCT(100));
    lv_obj_set_flex_grow(s.list, 1);
    refresh(NULL);
    lv_timer_create(refresh, 500, NULL);
    T5_IGNORE(t5_app_request_permission(app, T5_PERM_SHARED_READ, allowed, NULL));   /* the tunes play without it */
    if (s.session) T5_IGNORE(t5_task_create("music", worker, NULL, 4096, 2, T5_CORE_ANY, NULL));
    t5_mutex_lock(s.mx);
    publish(T5_MEDIA_STOPPED);
    t5_mutex_unlock(s.mx);
}

T5_APP(.on_create = on_create);
