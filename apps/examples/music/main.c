/* Music: a music player for the SD card's music folder.
 *
 *   Library    every MP3, FLAC and WAV under /sd/music, by song, album and
 *              artist, read from their tags (library.c, tags.c); album art
 *              from the files or the album folder (art.c)
 *   Playlists  the M3U files there; new ones, and edits, go to
 *              /sd/music/playlists (m3u.c)
 *   Playing    a queue with shuffle and repeat, a seekable progress bar,
 *              gapless when consecutive tracks share a format (player.c);
 *              the decoders are in the app: the OS image has none for MP3 or
 *              FLAC (decoder.c, third_party/)
 *   Standby    a media session: the standby screen's music card shows the
 *              track and its buttons come back here
 *
 * The queue and position are kept across launches (queue.txt in the app's
 * data folder), and the library index too (library.bin), so the next start
 * reads tags only for new or changed files.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <t5/app.h>
#include <t5/audio.h>
#include <t5/shell.h>
#include <t5/ui.h>
#include "art.h"
#include "library.h"
#include "m3u.h"
#include "music_icons.h"
#include "player.h"

#define ART_NP_DP_LAND 208
#define ART_NP_DP_PORT 280
#define THUMB_DP 48
#define MINI_DP 64
#define THUMBS_MAX 48
#define SAVE_EVERY_MS 10000

typedef enum { V_SONGS, V_ALBUMS, V_ARTISTS, V_PLAYLISTS, V_TRACKS, V_QUEUE } vkind_t;

typedef struct view {
    vkind_t kind;
    lv_obj_t *list;
    lv_obj_t *page;               /* detail views: the page they live on */
    lv_obj_t *notice;             /* tab views: what to say when there's nothing */
    int *tracks;                  /* V_TRACKS: library indices */
    int n;
    char pl_path[M3U_PATH_MAX];   /* a playlist's tracks */
    int album;                    /* an album's tracks: its index, else -1 */
} view_t;

typedef struct {
    uint32_t key;
    lv_image_dsc_t *img;          /* NULL: no art (looked) */
    uint32_t used;
} thumb_t;

static struct {
    t5_app_t *app;
    lib_t *lib;
    uint32_t lib_gen;             /* bumped on every library swap */
    bool scanning;
    int scan_files;
    bool have_read;
    char data[160];

    lv_obj_t *root, *tabs;
    view_t songs, albums, artists, playlists;
    lv_obj_t *songs_count;

    lv_obj_t *mini, *mini_art, *mini_icon, *mini_title, *mini_sub, *mini_play;
    lv_image_dsc_t *mini_img;
    uint32_t mini_key;

    struct {
        lv_obj_t *page, *art, *art_icon, *title, *sub, *slider, *pos, *dur, *play, *shuffle, *repeat;
        lv_image_dsc_t *img;
        uint32_t key;
        int art_dp;
    } np;
    view_t *queue_view;           /* the queue page's view, while open */
    view_t *detail;               /* the album/artist/playlist page on top, while open */

    player_state_t ps;
    uint32_t shown_version;
    char shown_path[M3U_PATH_MAX];
    uint32_t last_save, saved_version;

    thumb_t thumbs[THUMBS_MAX];
    uint32_t thumb_clock;
    struct {
        lv_obj_t *row, *img;
        int album;
    } thumb_rows[64];

    /* a track the next menu acts on */
    int menu_track;
    view_t *menu_view;
    int menu_index;
    int menu_npl;
    int menu_pl[16];
} ui;

static void refresh_lists(void);
static void open_np(void);
static void open_queue(void);
static void open_album(int a);
static void open_artist(int a);
static void open_playlist(const char *path);
static void start_scan(void);

/* ---- small helpers -------------------------------------------------------------------- */
static const lib_track_t *track(int i)
{
    return ui.lib && i >= 0 && i < ui.lib->n ? &ui.lib->t[i] : NULL;
}

static const char *or_unknown(const char *s, const char *d)
{
    return s && s[0] ? s : d;
}

static uint32_t path_key(const char *p)
{
    uint32_t h = 2166136261u;
    while (*p) h = (h ^ (uint8_t)*p++) * 16777619u;
    return h | 1u;                                   /* never 0 */
}

static lv_obj_t *icon_label(lv_obj_t *parent, const char *glyph, bool app_font, int dp)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, glyph);
    lv_obj_set_style_text_font(l, app_font ? (dp >= 40 ? &music_icons_80 : &music_icons_48) : md_icon_font(dp), 0);
    lv_obj_set_style_text_color(l, md_color(md_scheme()->c.on_surface), 0);
    return l;
}

/* A round, flat icon button (48 dp touch target). */
static lv_obj_t *icon_btn(lv_obj_t *parent, const char *glyph, bool app_font, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, T5_DP(48), T5_DP(48));
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_clickable(b, true);
    md_ripple_attach(b);
    lv_obj_t *l = icon_label(b, glyph, app_font, 24);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static void set_icon(lv_obj_t *btn, const char *glyph)
{
    lv_label_set_text(lv_obj_get_child(btn, 0), glyph);
}

static void set_active(lv_obj_t *btn, bool on)
{
    lv_obj_t *l = lv_obj_get_child(btn, 0);
    const md_scheme_t *sc = md_scheme();
    lv_obj_set_style_text_color(l, md_color(on ? sc->c.secondary : sc->c.on_surface), 0);
    lv_obj_set_style_text_opa(l, on ? LV_OPA_COVER : sc->on_surface.medium, 0);
}

/* One line, cut with dots. LVGL's dots mode still wraps onto more lines
 * unless the height is fixed, so fix it to one line of the label's font. */
static void one_line(lv_obj_t *label)
{
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(label, lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN)));
}

static void toast(const char *t)
{
    T5_IGNORE(t5_shell_toast(t));                    /* a lost toast is harmless */
}

static void data_path(char *out, size_t cap, const char *name)
{
    snprintf(out, cap, "%s/%s", ui.data, name);
}

/* ---- saving --------------------------------------------------------------------------- */
static void save_state(void)
{
    char *t = player_save_text();
    if (!t) return;
    char p[200], tmp[210];
    data_path(p, sizeof p, "queue.txt");
    snprintf(tmp, sizeof tmp, "%s.new", p);
    FILE *f = fopen(tmp, "wb");
    if (f) {
        bool ok = fputs(t, f) >= 0;
        ok &= fclose(f) == 0;
        if (ok) {
            remove(p);
            rename(tmp, p);
        } else {
            remove(tmp);
        }
    }
    free(t);
    ui.saved_version = ui.ps.version;
    ui.last_save = t5_time_ms();
}

static void load_state(void)
{
    char p[200];
    data_path(p, sizeof p, "queue.txt");
    FILE *f = fopen(p, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *t = n > 0 && n < 1024 * 1024 ? malloc((size_t)n + 1) : NULL;
    if (t && fread(t, 1, (size_t)n, f) == (size_t)n) {
        t[n] = 0;
        player_load_text(t);
    }
    free(t);
    fclose(f);
}

/* ---- playing from the library ------------------------------------------------------------ */
static void play_tracks(const int *tracks, int n, int start, bool shuffle)
{
    if (n <= 0) return;
    const char **paths = malloc((size_t)n * sizeof *paths);
    if (!paths) return;
    for (int i = 0; i < n; i++) paths[i] = ui.lib->t[tracks[i]].path;
    player_set_queue(paths, n, start, shuffle);
    free(paths);
}

static const int *view_tracks(view_t *v, int *n)
{
    if (v->kind == V_SONGS) {
        *n = ui.lib ? ui.lib->n : 0;
        return ui.lib ? ui.lib->by_title : NULL;
    }
    *n = v->n;
    return v->tracks;
}

/* ---- album thumbnails ------------------------------------------------------------------------ */
static thumb_t *thumb_find(uint32_t key)
{
    for (int i = 0; i < THUMBS_MAX; i++) {
        if (ui.thumbs[i].key == key) return &ui.thumbs[i];
    }
    return NULL;
}

static bool thumb_on_screen(uint32_t key)
{
    for (int i = 0; i < 64; i++) {
        if (ui.thumb_rows[i].row && ui.thumb_rows[i].album >= 0 &&
            ((ui.lib_gen << 20) | (uint32_t)ui.thumb_rows[i].album) == key) {
            return true;
        }
    }
    return false;
}

static void thumb_show(lv_obj_t *row_img, lv_obj_t *icon, const thumb_t *t)
{
    bool has = t && t->img;
    lv_obj_set_hidden(row_img, !has);
    lv_label_set_text(icon, has ? "" : MI_ALBUM);
    if (has) lv_image_set_src(row_img, t->img);
}

static void thumb_done(uint32_t key, lv_image_dsc_t *img, void *user)
{
    (void)user;
    if (key >> 20 != (ui.lib_gen & 0xfff)) {         /* from before a rescan */
        art_free(img);
        return;
    }
    thumb_t *t = thumb_find(key);
    if (!t) {
        /* an empty slot, else the least recently used that isn't on screen */
        for (int i = 0; i < THUMBS_MAX && !t; i++) {
            if (!ui.thumbs[i].key) t = &ui.thumbs[i];
        }
        if (!t) {
            uint32_t oldest = UINT32_MAX;
            for (int i = 0; i < THUMBS_MAX; i++) {
                if (ui.thumbs[i].used < oldest && !thumb_on_screen(ui.thumbs[i].key)) {
                    oldest = ui.thumbs[i].used;
                    t = &ui.thumbs[i];
                }
            }
        }
        if (!t) {
            art_free(img);
            return;
        }
        art_free(t->img);
    } else {
        art_free(t->img);
    }
    t->key = key;
    t->img = img;
    t->used = ++ui.thumb_clock;
    for (int i = 0; i < 64; i++) {
        if (ui.thumb_rows[i].row && ((ui.lib_gen << 20) | (uint32_t)ui.thumb_rows[i].album) == key) {
            thumb_show(ui.thumb_rows[i].img, lv_obj_get_child(ui.thumb_rows[i].row, 0), t);
        }
    }
}

static void thumbs_clear(void)
{
    art_cancel(thumb_done);                             /* not the mini player's or Now playing's */
    for (int i = 0; i < 64; i++) {
        if (ui.thumb_rows[i].img) {
            lv_image_set_src(ui.thumb_rows[i].img, NULL);
            ui.thumb_rows[i].album = -1;
        }
    }
    for (int i = 0; i < THUMBS_MAX; i++) {
        art_free(ui.thumbs[i].img);
        ui.thumbs[i] = (thumb_t){ 0 };
    }
}

/* The image on an album row (made the first time the row is bound). */
static int thumb_row(lv_obj_t *row)
{
    int free_slot = -1;
    for (int i = 0; i < 64; i++) {
        if (ui.thumb_rows[i].row == row) return i;
        if (!ui.thumb_rows[i].row && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return -1;
    lv_obj_t *img = lv_image_create(row);
    lv_obj_set_size(img, T5_DP(THUMB_DP), T5_DP(THUMB_DP));
    lv_obj_align(img, LV_ALIGN_LEFT_MID, -T5_DP(4), 0);
    lv_obj_set_style_radius(img, T5_DP(4), 0);
    lv_obj_set_style_clip_corner(img, true, 0);
    lv_obj_set_hidden(img, true);
    ui.thumb_rows[free_slot].row = row;
    ui.thumb_rows[free_slot].img = img;
    ui.thumb_rows[free_slot].album = -1;
    return free_slot;
}

/* ---- list binding ---------------------------------------------------------------------- */
/* A row's text: from 56 dp to the row's end. The first bind can come
 * before the list is laid out, so the width is the display's then. */
static void label_fit(lv_obj_t *label, lv_obj_t *row)
{
    int32_t w = lv_obj_get_content_width(row);
    if (w <= 0) w = lv_display_get_horizontal_resolution(NULL) - T5_DP(32);
    w -= T5_DP(56);
    if (w > 0 && lv_obj_get_width(label) != w) {
        lv_obj_set_width(label, w);
        one_line(label);
    }
}

static void bind_track(md_list_row_t *r, int ti, bool in_album)
{
    const lib_track_t *t = track(ti);
    char sub[TAG_TEXT * 2 + 24], d[16];
    if (!t) {
        lv_label_set_text(r->primary, "");
        if (r->secondary) lv_label_set_text(r->secondary, "");
        return;
    }
    lib_fmt_time(d, sizeof d, t->duration_ms);
    if (in_album) {
        if (t->track) snprintf(sub, sizeof sub, "%u \xc2\xb7 %s", t->track, d);
        else snprintf(sub, sizeof sub, "%s", d);
    } else {
        snprintf(sub, sizeof sub, "%s \xc2\xb7 %s \xc2\xb7 %s", or_unknown(t->artist, "Unknown artist"),
                 or_unknown(t->album, "Unknown album"), d);
    }
    lv_label_set_text(r->primary, t->title);
    if (r->secondary) lv_label_set_text(r->secondary, sub);
    bool now = ui.ps.has_track && !strcmp(t->path, ui.ps.path);
    lv_label_set_text(r->icon, now ? MD_ICON_GRAPHIC_EQ : MD_ICON_MUSIC_NOTE);
    lv_obj_set_style_text_color(r->icon, md_color(now ? md_scheme()->c.secondary : md_scheme()->c.on_surface), 0);
    label_fit(r->primary, r->row);
    if (r->secondary) label_fit(r->secondary, r->row);
}

static void bind(md_list_row_t *r, uint32_t index, void *user)
{
    view_t *v = user;
    int i = (int)index;
    if (!ui.lib) return;
    char sub[160];
    switch (v->kind) {
    case V_SONGS:
        if (i < ui.lib->n) bind_track(r, ui.lib->by_title[i], false);
        break;
    case V_TRACKS:
        if (i < v->n) bind_track(r, v->tracks[i], v->album >= 0);
        break;
    case V_QUEUE: {
        char p[M3U_PATH_MAX];
        if (!player_queue_item(i, p, sizeof p)) break;
        int ti = lib_find(ui.lib, p);
        if (ti >= 0) {
            bind_track(r, ti, false);
        } else {
            const char *b = strrchr(p, '/');
            lv_label_set_text(r->primary, b ? b + 1 : p);
            if (r->secondary) lv_label_set_text(r->secondary, "Not in the library");
        }
        bool now = i == ui.ps.qpos;
        lv_label_set_text(r->icon, now ? MD_ICON_GRAPHIC_EQ : MD_ICON_MUSIC_NOTE);
        lv_obj_set_style_text_color(r->icon, md_color(now ? md_scheme()->c.secondary : md_scheme()->c.on_surface), 0);
        break;
    }
    case V_ALBUMS: {
        if (i >= ui.lib->nalbums) break;
        const lib_album_t *a = &ui.lib->albums[i];
        lv_label_set_text(r->primary, a->name);
        if (a->year) snprintf(sub, sizeof sub, "%s \xc2\xb7 %u \xc2\xb7 %d song%s", a->artist, a->year, a->n, a->n == 1 ? "" : "s");
        else snprintf(sub, sizeof sub, "%s \xc2\xb7 %d song%s", a->artist, a->n, a->n == 1 ? "" : "s");
        lv_label_set_text(r->secondary, sub);
        lv_obj_set_style_text_font(r->icon, &music_icons_48, 0);
        label_fit(r->primary, r->row);
        label_fit(r->secondary, r->row);
        int slot = thumb_row(r->row);
        if (slot < 0) break;
        ui.thumb_rows[slot].album = i;
        uint32_t key = (ui.lib_gen << 20) | (uint32_t)i;
        thumb_t *t = thumb_find(key);
        thumb_show(ui.thumb_rows[slot].img, r->icon, t);
        if (t) {
            t->used = ++ui.thumb_clock;
        } else {
            const lib_track_t *c = &ui.lib->t[a->cover];
            art_request(key, c->path, c->art_off, c->art_len, T5_DP(THUMB_DP), thumb_done, NULL);
        }
        break;
    }
    case V_ARTISTS: {
        if (i >= ui.lib->nartists) break;
        const lib_artist_t *a = &ui.lib->artists[i];
        lv_label_set_text(r->primary, a->name);
        snprintf(sub, sizeof sub, "%d album%s \xc2\xb7 %d song%s", a->nalbums, a->nalbums == 1 ? "" : "s", a->n,
                 a->n == 1 ? "" : "s");
        lv_label_set_text(r->secondary, sub);
        lv_obj_set_style_text_font(r->icon, &music_icons_48, 0);
        lv_label_set_text(r->icon, MI_PERSON);
        label_fit(r->primary, r->row);
        break;
    }
    case V_PLAYLISTS: {
        if (i >= ui.lib->npl) break;
        lv_label_set_text(r->primary, ui.lib->pl[i].name);
        const char *p = ui.lib->pl[i].path;
        size_t root = strlen(MUSIC_DIR) + 1;
        lv_label_set_text(r->secondary, strlen(p) > root ? p + root : p);
        lv_obj_set_style_text_font(r->icon, &music_icons_48, 0);
        lv_label_set_text(r->icon, MI_QUEUE_MUSIC);
        label_fit(r->primary, r->row);
        break;
    }
    }
}

/* ---- the track menu ------------------------------------------------------------------------ */
static void playlist_add_to(const char *path, int ti);
static void open_name_page(const char *title, const char *initial, const char *pl_path, int add_track);

static void with_write(void (*fn)(void *), void *arg);

static void add_menu_picked(int item, void *ud)
{
    (void)ud;
    int n = ui.menu_npl;
    if (item < 0) return;
    if (item == n) {                                    /* New playlist... */
        open_name_page("New playlist", "", NULL, ui.menu_track);
        return;
    }
    if (item < n) playlist_add_to(ui.lib->pl[ui.menu_pl[item]].path, ui.menu_track);
}

static void show_add_menu(lv_obj_t *anchor)
{
    static const char *items[16];
    int n = 0;
    for (int i = 0; ui.lib && i < ui.lib->npl && n < 14; i++) {
        items[n] = ui.lib->pl[i].name;
        ui.menu_pl[n] = i;
        n++;
    }
    items[n] = "New playlist\xe2\x80\xa6";
    ui.menu_npl = n;
    md_menu_show(anchor, items, n + 1, add_menu_picked, NULL);
}

static void remove_from_playlist(void *arg);

static void track_menu_picked(int item, void *ud)
{
    lv_obj_t *anchor = ud;
    const lib_track_t *t = track(ui.menu_track);
    view_t *v = ui.menu_view;
    if (item < 0 || !t) return;
    bool queue = v && v->kind == V_QUEUE;
    bool playlist = v && v->kind == V_TRACKS && v->pl_path[0];
    if (queue) {
        if (item == 0) player_remove(ui.menu_index);
        return;
    }
    switch (item) {
    case 0: player_add(t->path, true); toast("Plays next"); break;
    case 1: player_add(t->path, false); toast("Added to the queue"); break;
    case 2: show_add_menu(anchor); break;
    case 3: open_album(t->album_ix); break;
    case 4: open_artist(t->artist_ix); break;
    case 5:
        if (playlist) with_write(remove_from_playlist, NULL);
        break;
    }
}

static void long_pressed(uint32_t index, void *user)
{
    view_t *v = user;
    int n;
    const int *tracks = view_tracks(v, &n);
    ui.menu_view = v;
    ui.menu_index = (int)index;
    ui.menu_track = -1;
    if (v->kind == V_QUEUE) {
        char p[M3U_PATH_MAX];
        if (player_queue_item((int)index, p, sizeof p)) ui.menu_track = lib_find(ui.lib, p);
        static const char *q[] = { "Remove from the queue" };
        ui.menu_track = ui.menu_track < 0 ? 0 : ui.menu_track;
        md_menu_show(lv_indev_get_active_obj(), q, 1, track_menu_picked, NULL);
        return;
    }
    if (v->kind != V_SONGS && v->kind != V_TRACKS) return;
    if (!tracks || (int)index >= n) return;
    ui.menu_track = tracks[index];
    static const char *items[] = { "Play next", "Add to the queue", "Add to a playlist\xe2\x80\xa6", "Go to the album",
                                   "Go to the artist", "Remove from this playlist" };
    lv_obj_t *anchor = lv_indev_get_active_obj();
    md_menu_show(anchor, items, v->pl_path[0] ? 6 : 5, track_menu_picked, anchor);
}

static void clicked(uint32_t index, void *user)
{
    view_t *v = user;
    if (!ui.lib) return;
    int i = (int)index;
    switch (v->kind) {
    case V_SONGS:
    case V_TRACKS: {
        int n;
        const int *tracks = view_tracks(v, &n);
        if (i < n) {
            play_tracks(tracks, n, i, ui.ps.shuffle);
        }
        break;
    }
    case V_QUEUE: player_jump(i); break;
    case V_ALBUMS: open_album(i); break;
    case V_ARTISTS: open_artist(i); break;
    case V_PLAYLISTS:
        if (i < ui.lib->npl) open_playlist(ui.lib->pl[i].path);
        break;
    }
}

static lv_obj_t *make_list(lv_obj_t *parent, view_t *v, uint32_t count, int row_dp)
{
    v->list = md_list_create(parent, count, row_dp, bind, clicked, v);
    md_list_set_long_press(v->list, long_pressed);
    lv_obj_set_width(v->list, LV_PCT(100));
    lv_obj_set_flex_grow(v->list, 1);
    return v->list;
}

/* ---- pages ------------------------------------------------------------------------------- */
/* The back arrow: delete the page it sits on (the screen's child). */
static void page_back(lv_event_t *e)
{
    lv_obj_t *scr = t5_app_screen(ui.app), *o = lv_event_get_target_obj(e);
    while (o && lv_obj_get_parent(o) != scr) o = lv_obj_get_parent(o);
    if (o && o != ui.root) lv_obj_delete(o);
}

static void view_deleted(lv_event_t *e)
{
    view_t *v = lv_event_get_user_data(e);
    if (ui.queue_view == v) ui.queue_view = NULL;
    if (ui.detail == v) ui.detail = NULL;
    free(v->tracks);
    free(v);
}

static lv_obj_t *page_new(const char *title, lv_obj_t **bar)
{
    return md_page_create(t5_app_screen(ui.app), title, page_back, NULL, bar);
}

static lv_obj_t *button_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, T5_DP(16), 0);
    lv_obj_set_style_pad_ver(row, T5_DP(4), 0);
    lv_obj_set_style_pad_column(row, T5_DP(8), 0);
    return row;
}

static void play_view(lv_event_t *e)
{
    view_t *v = lv_event_get_user_data(e);
    int n;
    const int *t = view_tracks(v, &n);
    play_tracks(t, n, 0, false);
}

static void shuffle_view(lv_event_t *e)
{
    view_t *v = lv_event_get_user_data(e);
    int n;
    const int *t = view_tracks(v, &n);
    if (n) play_tracks(t, n, rand() % n, true);
}

/* A page of tracks: a header, Play and Shuffle, the list. Takes tracks. */
static view_t *track_page(const char *title, const char *heading, const char *sub, int *tracks, int n, int album,
                          lv_obj_t **bar_out)
{
    view_t *v = calloc(1, sizeof *v);
    if (!v) {
        free(tracks);
        return NULL;
    }
    v->kind = V_TRACKS;
    v->tracks = tracks;
    v->n = n;
    v->album = album;
    lv_obj_t *bar;
    lv_obj_t *page = page_new(title, &bar);
    if (bar_out) *bar_out = bar;
    v->page = page;
    lv_obj_add_event_cb(page, view_deleted, LV_EVENT_DELETE, v);
    lv_obj_t *body = md_page_body(page);
    lv_obj_set_scrollable(body, false);
    lv_obj_t *head = lv_obj_create(body);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(head, T5_DP(16), 0);
    lv_obj_set_style_pad_column(head, T5_DP(16), 0);
    if (album >= 0) {                                    /* the album's art */
        lv_obj_t *img = lv_image_create(head);
        lv_obj_set_size(img, T5_DP(96), T5_DP(96));
        lv_obj_set_style_radius(img, T5_DP(4), 0);
        lv_obj_set_style_clip_corner(img, true, 0);
        uint32_t key = (ui.lib_gen << 20) | (uint32_t)album;
        thumb_t *t = thumb_find(key);
        if (t && t->img) lv_image_set_src(img, t->img);
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
    }
    lv_obj_t *col = lv_obj_create(head);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *h = md_label_create(col, heading, MD_TYPE_HEADLINE6);
    lv_obj_set_width(h, LV_PCT(100));
    one_line(h);
    lv_obj_t *s = md_label_create(col, sub, MD_TYPE_BODY2);
    lv_obj_set_width(s, LV_PCT(100));
    one_line(s);
    lv_obj_t *br = button_row(col);
    lv_obj_set_style_pad_hor(br, 0, 0);
    lv_obj_add_event_cb(md_button_create(br, "Play", MD_BUTTON_CONTAINED), play_view, LV_EVENT_CLICKED, v);
    lv_obj_add_event_cb(md_button_create(br, "Shuffle", MD_BUTTON_OUTLINED), shuffle_view, LV_EVENT_CLICKED, v);
    make_list(body, v, (uint32_t)n, 64);
    ui.detail = v;
    return v;
}

static uint32_t total_ms(const int *t, int n)
{
    uint32_t ms = 0;
    for (int i = 0; i < n; i++) ms += ui.lib->t[t[i]].duration_ms;
    return ms;
}

static void open_album(int a)
{
    if (!ui.lib || a < 0 || a >= ui.lib->nalbums) return;
    const lib_album_t *al = &ui.lib->albums[a];
    int *t = malloc((size_t)al->n * sizeof *t);
    if (!t) return;
    memcpy(t, al->tracks, (size_t)al->n * sizeof *t);
    char sub[160], d[16];
    lib_fmt_time(d, sizeof d, total_ms(t, al->n));
    if (al->year) snprintf(sub, sizeof sub, "%s \xc2\xb7 %u \xc2\xb7 %d songs, %s", al->artist, al->year, al->n, d);
    else snprintf(sub, sizeof sub, "%s \xc2\xb7 %d songs, %s", al->artist, al->n, d);
    track_page("Album", al->name, sub, t, al->n, a, NULL);
}

static void open_artist(int a)
{
    if (!ui.lib || a < 0 || a >= ui.lib->nartists) return;
    const lib_artist_t *ar = &ui.lib->artists[a];
    int *t = malloc((size_t)ar->n * sizeof *t);
    if (!t) return;
    memcpy(t, ar->tracks, (size_t)ar->n * sizeof *t);
    char sub[96];
    snprintf(sub, sizeof sub, "%d album%s \xc2\xb7 %d song%s", ar->nalbums, ar->nalbums == 1 ? "" : "s", ar->n,
             ar->n == 1 ? "" : "s");
    track_page("Artist", ar->name, sub, t, ar->n, -1, NULL);
}

/* ---- playlists ---------------------------------------------------------------------------- */
static void playlist_menu(lv_event_t *e);

static void open_playlist(const char *path)
{
    if (!ui.lib) return;
    int n = 0, missing = 0;
    int *t = lib_playlist_tracks(ui.lib, path, &n, &missing);
    if (!t) {
        toast("Can't read that playlist");
        return;
    }
    const char *name = path;
    for (int i = 0; i < ui.lib->npl; i++) {
        if (!strcmp(ui.lib->pl[i].path, path)) name = ui.lib->pl[i].name;
    }
    char sub[96], d[16];
    lib_fmt_time(d, sizeof d, total_ms(t, n));
    snprintf(sub, sizeof sub, "%d song%s, %s", n, n == 1 ? "" : "s", d);
    lv_obj_t *bar;
    view_t *v = track_page("Playlist", name, sub, t, n, -1, &bar);
    if (!v) return;
    snprintf(v->pl_path, sizeof v->pl_path, "%s", path);
    md_appbar_add_action(bar, MD_ICON_MORE_VERT, playlist_menu, v);
    if (missing) {
        char m[96];
        snprintf(m, sizeof m, "%d song%s in this playlist %s not in the library", missing, missing == 1 ? "" : "s",
                 missing == 1 ? "is" : "are");
        toast(m);
    }
}

static void playlists_changed(void)
{
    lib_rescan_playlists(ui.lib);
    md_list_set_count(ui.playlists.list, (uint32_t)ui.lib->npl);
    md_list_refresh(ui.playlists.list);
    refresh_lists();
}

static void write_or_toast(const char *path, const int *t, int n, const char *ok_msg)
{
    if (lib_playlist_write(ui.lib, path, t, n)) {
        if (ok_msg) toast(ok_msg);
    } else {
        toast("Can't write the playlist");
    }
}

/* After the person allows writing to shared folders. */
typedef struct {
    void (*fn)(void *);
    void *arg;
} write_job_t;

static void write_allowed(t5_app_t *app, uint32_t perm, bool granted, void *user)
{
    (void)app;
    (void)perm;
    write_job_t *j = user;
    if (granted) j->fn(j->arg);
    else toast("Playlists need permission to change files in the music folder");
    free(j);
}

static void with_write(void (*fn)(void *), void *arg)
{
    if (t5_app_has_permission(ui.app, T5_PERM_SHARED_WRITE)) {
        fn(arg);
        return;
    }
    write_job_t *j = malloc(sizeof *j);
    if (!j) return;
    *j = (write_job_t){ fn, arg };
    if (t5_app_request_permission(ui.app, T5_PERM_SHARED_WRITE, write_allowed, j) != T5_OK) free(j);
}

typedef struct {
    char path[M3U_PATH_MAX];
    int track;
} add_job_t;

static void do_add(void *arg)
{
    add_job_t *j = arg;
    int n = 0;
    int *t = lib_playlist_tracks(ui.lib, j->path, &n, NULL);
    int *t2 = t ? realloc(t, (size_t)(n + 1) * sizeof *t) : NULL;
    if (t2) {
        t2[n] = j->track;
        write_or_toast(j->path, t2, n + 1, "Added to the playlist");
        free(t2);
    } else {
        free(t);
    }
    free(j);
}

static void playlist_add_to(const char *path, int ti)
{
    add_job_t *j = malloc(sizeof *j);
    if (!j) return;
    snprintf(j->path, sizeof j->path, "%s", path);
    j->track = ti;
    with_write(do_add, j);
}

static void remove_from_playlist(void *arg)
{
    (void)arg;
    view_t *v = ui.menu_view;
    if (!v || !v->pl_path[0] || ui.menu_index >= v->n) return;
    memmove(v->tracks + ui.menu_index, v->tracks + ui.menu_index + 1, (size_t)(v->n - ui.menu_index - 1) * sizeof(int));
    v->n--;
    write_or_toast(v->pl_path, v->tracks, v->n, NULL);
    md_list_set_count(v->list, (uint32_t)v->n);
    md_list_refresh(v->list);
}

/* The name page: a new playlist, or a rename. */
typedef struct {
    lv_obj_t *page, *field;
    char old[M3U_PATH_MAX];       /* rename: the playlist's path */
    int add_track;                /* new: a track to start it with, or -1 */
} name_page_t;

static void name_page_deleted(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static bool clean_name(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < cap; p++) {
        if (strchr("/\\:*?\"<>|", *p) || (unsigned char)*p < 0x20) continue;
        out[o++] = *p;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
    size_t s = 0;
    while (out[s] == ' ' || out[s] == '.') s++;
    memmove(out, out + s, o - s + 1);
    return out[0] != 0;
}

static void do_name(void *arg)
{
    name_page_t *np = arg;
    char name[96], path[M3U_PATH_MAX];
    if (!clean_name(md_textfield_text(np->field), name, sizeof name)) {
        md_textfield_set_helper(np->field, "Give it a name", true);
        return;
    }
    snprintf(path, sizeof path, PLAYLIST_DIR "/%s.m3u", name);
    struct stat st;
    if (stat(path, &st) == 0 && tag_casecmp(path, np->old)) {
        md_textfield_set_helper(np->field, "There's a playlist with that name", true);
        return;
    }
    if (np->old[0]) {
        mkdir(PLAYLIST_DIR, 0775);
        if (rename(np->old, path) != 0) {
            toast("Can't rename the playlist");
            return;
        }
        if (ui.detail && !strcmp(ui.detail->pl_path, np->old)) lv_obj_delete(ui.detail->page);
    } else {
        int t = np->add_track;
        write_or_toast(path, &t, t >= 0 ? 1 : 0, t >= 0 ? "Added to the new playlist" : "Playlist made");
    }
    playlists_changed();
    lv_obj_delete(np->page);
}

static void name_done(lv_event_t *e)
{
    with_write(do_name, lv_event_get_user_data(e));
}

static void open_name_page(const char *title, const char *initial, const char *pl_path, int add_track)
{
    name_page_t *np = calloc(1, sizeof *np);
    if (!np) return;
    np->add_track = add_track;
    if (pl_path) snprintf(np->old, sizeof np->old, "%s", pl_path);
    np->page = page_new(title, NULL);
    lv_obj_add_event_cb(np->page, name_page_deleted, LV_EVENT_DELETE, np);
    lv_obj_t *body = md_page_body(np->page);
    lv_obj_set_style_pad_all(body, T5_DP(16), 0);
    lv_obj_set_style_pad_row(body, T5_DP(16), 0);
    np->field = md_textfield_create(body, "Playlist name", false);
    lv_obj_set_width(np->field, LV_PCT(100));
    md_textfield_set_text(np->field, initial);
    lv_obj_add_event_cb(md_button_create(body, pl_path ? "Rename" : "Make playlist", MD_BUTTON_CONTAINED), name_done,
                        LV_EVENT_CLICKED, np);
    lv_obj_add_state(md_textfield_ta(np->field), LV_STATE_FOCUSED);
    t5_shell_keyboard_show(md_textfield_ta(np->field));
}

typedef struct {
    char path[M3U_PATH_MAX];
} del_job_t;

static void do_delete(void *arg)
{
    del_job_t *j = arg;
    if (remove(j->path) == 0) {
        if (ui.detail && !strcmp(ui.detail->pl_path, j->path)) lv_obj_delete(ui.detail->page);
        playlists_changed();
        toast("Playlist deleted");
    } else {
        toast("Can't delete the playlist");
    }
    free(j);
}

static void delete_answer(int button, void *ud)
{
    if (button == 0) with_write(do_delete, ud);
    else free(ud);
}

static void playlist_menu_picked(int item, void *ud)
{
    view_t *v = ud;
    if (item == 0) {
        const char *b = strrchr(v->pl_path, '/');
        char name[96];
        b = b ? b + 1 : v->pl_path;
        const char *dot = strrchr(b, '.');
        snprintf(name, sizeof name, "%.*s", dot ? (int)(dot - b) : (int)strlen(b), b);
        open_name_page("Rename playlist", name, v->pl_path, -1);
    } else if (item == 1) {
        del_job_t *j = malloc(sizeof *j);
        if (!j) return;
        snprintf(j->path, sizeof j->path, "%s", v->pl_path);
        md_dialog_show("Delete this playlist?", "The songs stay in your library.", "Delete", "Cancel", delete_answer, j);
    }
}

static void playlist_menu(lv_event_t *e)
{
    static const char *items[] = { "Rename", "Delete" };
    md_menu_show(lv_event_get_target_obj(e), items, 2, playlist_menu_picked, lv_event_get_user_data(e));
}

static void new_playlist(lv_event_t *e)
{
    (void)e;
    open_name_page("New playlist", "", NULL, -1);
}

/* ---- the queue page -------------------------------------------------------------------------- */
static void open_queue(void)
{
    if (ui.queue_view) return;
    view_t *v = calloc(1, sizeof *v);
    if (!v) return;
    v->kind = V_QUEUE;
    v->album = -1;
    lv_obj_t *page = page_new("Queue", NULL);
    v->page = page;
    lv_obj_add_event_cb(page, view_deleted, LV_EVENT_DELETE, v);
    lv_obj_t *body = md_page_body(page);
    lv_obj_set_scrollable(body, false);
    make_list(body, v, (uint32_t)ui.ps.qlen, 64);
    ui.queue_view = v;
}

/* ---- now playing --------------------------------------------------------------------------- */
static void np_deleted(lv_event_t *e)
{
    (void)e;
    ui.np.page = NULL;
    art_free(ui.np.img);
    ui.np.img = NULL;
    ui.np.key = 0;
}

static void np_art_done(uint32_t key, lv_image_dsc_t *img, void *user)
{
    (void)user;
    if (!ui.np.page || (key ^ 0x80000000u) != ui.np.key) {
        art_free(img);
        return;
    }
    lv_image_set_src(ui.np.art, img);
    lv_obj_set_hidden(ui.np.art_icon, img != NULL);
    art_free(ui.np.img);
    ui.np.img = img;
}

static void mini_art_done(uint32_t key, lv_image_dsc_t *img, void *user)
{
    (void)user;
    if (key != ui.mini_key) {
        art_free(img);
        return;
    }
    lv_image_set_src(ui.mini_art, img);
    lv_obj_set_hidden(ui.mini_icon, img != NULL);
    art_free(ui.mini_img);
    ui.mini_img = img;
}

static void art_for_track(void)
{
    const tags_t *t = &ui.ps.tags;
    uint32_t key = ui.ps.has_track ? path_key(ui.ps.path) : 0;
    if (key != ui.mini_key) {
        ui.mini_key = key;
        lv_image_set_src(ui.mini_art, NULL);
        art_free(ui.mini_img);
        ui.mini_img = NULL;
        lv_obj_set_hidden(ui.mini_icon, false);
        if (key) art_request(key, ui.ps.path, t->art_off, t->art_len, T5_DP(48), mini_art_done, NULL);
    }
    if (ui.np.page && key != ui.np.key) {
        ui.np.key = key;
        lv_image_set_src(ui.np.art, NULL);
        art_free(ui.np.img);
        ui.np.img = NULL;
        lv_obj_set_hidden(ui.np.art_icon, false);
        /* its own request key: the mini player's is the same track at another size */
        if (key) art_request(key ^ 0x80000000u, ui.ps.path, t->art_off, t->art_len, T5_DP(ui.np.art_dp), np_art_done, NULL);
    }
}

static void ctl_prev(lv_event_t *e) { (void)e; player_prev(); }
static void ctl_next(lv_event_t *e) { (void)e; player_next(); }
static void ctl_toggle(lv_event_t *e) { (void)e; player_toggle(); }
static void ctl_shuffle(lv_event_t *e) { (void)e; player_set_shuffle(!ui.ps.shuffle); }
static void ctl_queue(lv_event_t *e) { (void)e; open_queue(); }

static void ctl_repeat(lv_event_t *e)
{
    (void)e;
    repeat_t r = ui.ps.repeat == REPEAT_OFF ? REPEAT_ALL : ui.ps.repeat == REPEAT_ALL ? REPEAT_ONE : REPEAT_OFF;
    player_set_repeat(r);
    toast(r == REPEAT_OFF ? "Repeat off" : r == REPEAT_ALL ? "Repeating the queue" : "Repeating this song");
}

static void ctl_add(lv_event_t *e)
{
    int ti = ui.ps.has_track ? lib_find(ui.lib, ui.ps.path) : -1;
    if (ti < 0) {
        toast("That song isn't in the library");
        return;
    }
    ui.menu_track = ti;
    show_add_menu(lv_event_get_target_obj(e));
}

static void slider_released(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target_obj(e);
    if (ui.ps.dur_ms) player_seek((uint32_t)((uint64_t)lv_slider_get_value(s) * ui.ps.dur_ms / 1000));
}

static void slider_moved(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target_obj(e);
    if (!lv_slider_is_dragged(s) || !ui.ps.dur_ms) return;
    char t[16];
    lib_fmt_time(t, sizeof t, (uint32_t)((uint64_t)lv_slider_get_value(s) * ui.ps.dur_ms / 1000));
    lv_label_set_text(ui.np.pos, t);
}

static void np_build(void)
{
    bool land = lv_display_get_horizontal_resolution(NULL) > lv_display_get_vertical_resolution(NULL);
    ui.np.art_dp = land ? ART_NP_DP_LAND : ART_NP_DP_PORT;
    lv_obj_t *page = page_new("Now playing", NULL);
    ui.np.page = page;
    lv_obj_add_event_cb(page, np_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_t *body = md_page_body(page);
    lv_obj_set_flex_flow(body, land ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(body, T5_DP(16), 0);
    lv_obj_set_style_pad_gap(body, T5_DP(24), 0);

    /* the art, with a placeholder under it */
    lv_obj_t *frame = lv_obj_create(body);
    lv_obj_remove_style_all(frame);
    lv_obj_set_size(frame, T5_DP(ui.np.art_dp), T5_DP(ui.np.art_dp));
    lv_obj_set_style_radius(frame, T5_DP(8), 0);
    lv_obj_set_style_clip_corner(frame, true, 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    md_set_elevation(frame, 2, false);
    ui.np.art_icon = icon_label(frame, MI_ALBUM, true, 40);
    lv_obj_center(ui.np.art_icon);
    ui.np.art = lv_image_create(frame);
    lv_obj_set_size(ui.np.art, LV_PCT(100), LV_PCT(100));

    lv_obj_t *col = lv_obj_create(body);
    lv_obj_remove_style_all(col);
    if (land) lv_obj_set_flex_grow(col, 1);
    lv_obj_set_width(col, land ? LV_SIZE_CONTENT : LV_PCT(100));
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, T5_DP(8), 0);
    ui.np.title = md_label_create(col, "", MD_TYPE_HEADLINE5);
    lv_obj_set_width(ui.np.title, LV_PCT(100));
    one_line(ui.np.title);
    lv_obj_set_style_text_align(ui.np.title, LV_TEXT_ALIGN_CENTER, 0);
    ui.np.sub = md_label_create(col, "", MD_TYPE_SUBTITLE1);
    lv_obj_set_width(ui.np.sub, LV_PCT(100));
    one_line(ui.np.sub);
    lv_obj_set_style_text_align(ui.np.sub, LV_TEXT_ALIGN_CENTER, 0);

    ui.np.slider = md_slider_create(col, 0, 1000, 0);
    lv_obj_set_width(ui.np.slider, LV_PCT(100));
    lv_obj_add_event_cb(ui.np.slider, slider_released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(ui.np.slider, slider_moved, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *times = lv_obj_create(col);
    lv_obj_remove_style_all(times);
    lv_obj_set_size(times, LV_PCT(100), LV_SIZE_CONTENT);
    ui.np.pos = md_label_create(times, "0:00", MD_TYPE_CAPTION);
    lv_obj_align(ui.np.pos, LV_ALIGN_LEFT_MID, 0, 0);
    ui.np.dur = md_label_create(times, "0:00", MD_TYPE_CAPTION);
    lv_obj_align(ui.np.dur, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *ctl = lv_obj_create(col);
    lv_obj_remove_style_all(ctl);
    lv_obj_set_size(ctl, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ctl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctl, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui.np.shuffle = icon_btn(ctl, MI_SHUFFLE, true, ctl_shuffle, NULL);
    icon_btn(ctl, MD_ICON_SKIP_PREVIOUS, false, ctl_prev, NULL);
    ui.np.play = md_fab_create(ctl, MD_ICON_PLAY_ARROW, false);
    lv_obj_add_event_cb(ui.np.play, ctl_toggle, LV_EVENT_CLICKED, NULL);
    icon_btn(ctl, MD_ICON_SKIP_NEXT, false, ctl_next, NULL);
    ui.np.repeat = icon_btn(ctl, MI_REPEAT, true, ctl_repeat, NULL);

    lv_obj_t *more = lv_obj_create(col);
    lv_obj_remove_style_all(more);
    lv_obj_set_size(more, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(more, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(more, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    icon_btn(more, MI_QUEUE_MUSIC, true, ctl_queue, NULL);
    icon_btn(more, MI_PLAYLIST_ADD, true, ctl_add, NULL);
    ui.np.key = 0;
    ui.shown_version--;                                 /* fill it on the next tick */
}

static void open_np(void)
{
    if (!ui.np.page) np_build();
}

/* ---- refreshing ---------------------------------------------------------------------------- */
static void np_update(bool changed)
{
    const player_state_t *p = &ui.ps;
    char t[16];
    if (changed) {
        lv_label_set_text(ui.np.title, p->has_track ? p->tags.title : "Nothing playing");
        char sub[TAG_TEXT * 2 + 8];
        snprintf(sub, sizeof sub, "%s%s%s", p->tags.artist, p->tags.artist[0] && p->tags.album[0] ? " \xc2\xb7 " : "",
                 p->tags.album);
        lv_label_set_text(ui.np.sub, p->has_track ? sub : "");
        set_active(ui.np.shuffle, p->shuffle);
        set_icon(ui.np.repeat, p->repeat == REPEAT_ONE ? MI_REPEAT_ONE : MI_REPEAT);
        set_active(ui.np.repeat, p->repeat != REPEAT_OFF);
    }
    set_icon(ui.np.play, p->playing ? MD_ICON_PAUSE : MD_ICON_PLAY_ARROW);
    lib_fmt_time(t, sizeof t, p->dur_ms);
    lv_label_set_text(ui.np.dur, t);
    if (!lv_slider_is_dragged(ui.np.slider)) {
        lib_fmt_time(t, sizeof t, p->pos_ms);
        lv_label_set_text(ui.np.pos, t);
        int32_t v = p->dur_ms ? (int32_t)((uint64_t)p->pos_ms * 1000 / p->dur_ms) : 0;
        lv_slider_set_value(ui.np.slider, v > 1000 ? 1000 : v, LV_ANIM_OFF);
    }
}

static void refresh_lists(void)
{
    view_t *vs[] = { &ui.songs, &ui.albums, &ui.artists, &ui.playlists, ui.detail, ui.queue_view };
    for (size_t i = 0; i < sizeof vs / sizeof vs[0]; i++) {
        if (vs[i] && vs[i]->list) md_list_refresh(vs[i]->list);
    }
}

static void tick(lv_timer_t *tm)
{
    (void)tm;
    player_get(&ui.ps);
    const player_state_t *p = &ui.ps;
    bool changed = p->version != ui.shown_version;
    if (changed) {
        ui.shown_version = p->version;
        lv_label_set_text(ui.mini_title, p->has_track ? p->tags.title : "Nothing playing");
        lv_label_set_text(ui.mini_sub, p->has_track ? or_unknown(p->tags.artist, "Unknown artist") : "Pick a song");
        art_for_track();
        if (strcmp(ui.shown_path, p->path)) {          /* the playing marks in the lists */
            snprintf(ui.shown_path, sizeof ui.shown_path, "%s", p->path);
            refresh_lists();
        }
        if (ui.queue_view) {
            md_list_set_count(ui.queue_view->list, (uint32_t)p->qlen);
            md_list_refresh(ui.queue_view->list);
        }
    }
    set_icon(ui.mini_play, p->playing ? MD_ICON_PAUSE : MD_ICON_PLAY_ARROW);
    if (ui.np.page) np_update(changed);
    if (p->version != ui.saved_version && t5_time_ms() - ui.last_save > SAVE_EVERY_MS) save_state();
}

/* ---- the library: loading, scanning ----------------------------------------------------------- */
static void set_notice(view_t *v, int count, const char *empty)
{
    const char *msg = NULL;
    if (!ui.have_read) msg = "Music needs to read the SD card's music folder.";
    else if (!count && ui.scanning) msg = "Looking for music\xe2\x80\xa6";
    else if (!count) msg = empty;
    lv_label_set_text(v->notice, msg ? msg : "");
    lv_obj_set_hidden(v->notice, !msg);
}

static void lists_from_lib(void)
{
    int n = ui.lib ? ui.lib->n : 0;
    md_list_set_count(ui.songs.list, (uint32_t)n);
    md_list_set_count(ui.albums.list, ui.lib ? (uint32_t)ui.lib->nalbums : 0);
    md_list_set_count(ui.artists.list, ui.lib ? (uint32_t)ui.lib->nartists : 0);
    md_list_set_count(ui.playlists.list, ui.lib ? (uint32_t)ui.lib->npl : 0);
    refresh_lists();
    const char *none = "No music here yet. Put MP3, FLAC or WAV files in the \"music\" folder on the SD card.";
    set_notice(&ui.songs, n, none);
    set_notice(&ui.albums, ui.lib ? ui.lib->nalbums : 0, none);
    set_notice(&ui.artists, ui.lib ? ui.lib->nartists : 0, none);
    set_notice(&ui.playlists, ui.lib ? ui.lib->npl : 0,
               "No playlists yet. Make one here, or put .m3u files in the music folder.");
    char c[96];
    if (ui.scanning) snprintf(c, sizeof c, "Looking for music\xe2\x80\xa6 %d files", ui.scan_files);
    else snprintf(c, sizeof c, "%d song%s", n, n == 1 ? "" : "s");
    lv_label_set_text(ui.songs_count, c);
}

static void set_lib(lib_t *l)
{
    thumbs_clear();
    /* pages built on the old library hold its indices: close them */
    if (ui.detail) lv_obj_delete(ui.detail->page);
    lib_t *old = ui.lib;
    ui.lib = l;
    ui.lib_gen = (ui.lib_gen + 1) & 0xfff;
    lib_free(old);
    lists_from_lib();
}

typedef struct {
    lib_t *lib;
} scan_done_t;

static void scan_finished(void *arg)
{
    scan_done_t *d = arg;
    ui.scanning = false;
    if (d->lib) {
        set_lib(d->lib);
        char p[200];
        data_path(p, sizeof p, "library.bin");
        if (!lib_save_cache(ui.lib, p)) remove(p);     /* past the data folder's 1 MB: rescan next time */
    } else {
        lists_from_lib();
    }
    free(d);
}

static void scan_progress_ui(void *arg)
{
    ui.scan_files = (int)(intptr_t)arg;
    char c[64];
    snprintf(c, sizeof c, "Looking for music\xe2\x80\xa6 %d files", ui.scan_files);
    lv_label_set_text(ui.songs_count, c);
}

static void scan_progress(int files, void *user)
{
    (void)user;
    T5_IGNORE(t5_ui_async(scan_progress_ui, (void *)(intptr_t)files));   /* a dropped update is fine */
}

static void scan_task(void *arg)
{
    const lib_t *prev = arg;
    scan_done_t *d = malloc(sizeof *d);
    if (!d) return;
    d->lib = lib_scan(prev, scan_progress, NULL);
    if (t5_ui_async(scan_finished, d) != T5_OK) {
        lib_free(d->lib);
        free(d);
    }
}

static void start_scan(void)
{
    if (ui.scanning || !ui.have_read) return;
    ui.scanning = true;
    ui.scan_files = 0;
    lists_from_lib();
    if (t5_task_create("music.scan", scan_task, ui.lib, 16 * 1024, 2, T5_CORE_ANY, NULL) != T5_OK) {
        ui.scanning = false;
        lists_from_lib();
    }
}

static void rescan_clicked(lv_event_t *e)
{
    (void)e;
    start_scan();
}

static void shuffle_all(lv_event_t *e)
{
    (void)e;
    if (!ui.lib || !ui.lib->n) return;
    play_tracks(ui.lib->by_title, ui.lib->n, rand() % ui.lib->n, true);
}

static void allowed(t5_app_t *app, uint32_t perm, bool granted, void *user)
{
    (void)app;
    (void)perm;
    (void)user;
    ui.have_read = granted;
    if (!granted) {
        lists_from_lib();
        return;
    }
    if (!ui.lib) {
        char p[200];
        data_path(p, sizeof p, "library.bin");
        lib_t *l = lib_load_cache(p);
        if (l) set_lib(l);
    }
    start_scan();
}

static void ask_read(lv_event_t *e)
{
    (void)e;
    T5_IGNORE(t5_app_request_permission(ui.app, T5_PERM_SHARED_READ, allowed, NULL));   /* answered in allowed() */
}

/* ---- building --------------------------------------------------------------------------------- */
static lv_obj_t *tab_page(lv_obj_t *tabs, const char *name, view_t *v, vkind_t kind)
{
    lv_obj_t *page = md_tabs_add(tabs, name);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(page, false);
    lv_obj_set_style_pad_all(page, 0, 0);
    v->kind = kind;
    v->album = -1;
    return page;
}

static void add_notice(lv_obj_t *page, view_t *v)
{
    v->notice = md_label_create(page, "", MD_TYPE_BODY1);
    lv_obj_set_width(v->notice, LV_PCT(100));
    lv_label_set_long_mode(v->notice, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_pad_all(v->notice, T5_DP(24), 0);
    lv_obj_set_style_text_align(v->notice, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_floating(v->notice, true);
    lv_obj_align(v->notice, LV_ALIGN_TOP_MID, 0, T5_DP(64));
    lv_obj_set_clickable(v->notice, true);
    lv_obj_add_event_cb(v->notice, ask_read, LV_EVENT_CLICKED, NULL);
}

static void mini_clicked(lv_event_t *e)
{
    (void)e;
    open_np();
}

static void build_mini(lv_obj_t *root)
{
    lv_obj_t *m = lv_obj_create(root);
    lv_obj_remove_style_all(m);
    ui.mini = m;
    lv_obj_set_size(m, LV_PCT(100), T5_DP(MINI_DP));
    md_set_elevation(m, 8, false);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(m, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(m, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(m, T5_DP(8), 0);
    lv_obj_set_style_pad_column(m, T5_DP(8), 0);
    lv_obj_set_clickable(m, true);
    lv_obj_add_event_cb(m, mini_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *art = lv_obj_create(m);
    lv_obj_remove_style_all(art);
    lv_obj_set_size(art, T5_DP(48), T5_DP(48));
    lv_obj_set_style_radius(art, T5_DP(4), 0);
    lv_obj_set_style_clip_corner(art, true, 0);
    lv_obj_set_clickable(art, false);
    ui.mini_icon = icon_label(art, MI_ALBUM, true, 24);
    lv_obj_center(ui.mini_icon);
    ui.mini_art = lv_image_create(art);
    lv_obj_set_size(ui.mini_art, LV_PCT(100), LV_PCT(100));

    lv_obj_t *col = lv_obj_create(m);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_clickable(col, false);
    ui.mini_title = md_label_create(col, "Nothing playing", MD_TYPE_SUBTITLE1);
    lv_obj_set_width(ui.mini_title, LV_PCT(100));
    one_line(ui.mini_title);
    ui.mini_sub = md_label_create(col, "Pick a song", MD_TYPE_BODY2);
    lv_obj_set_width(ui.mini_sub, LV_PCT(100));
    one_line(ui.mini_sub);

    icon_btn(m, MD_ICON_SKIP_PREVIOUS, false, ctl_prev, NULL);
    ui.mini_play = icon_btn(m, MD_ICON_PLAY_ARROW, false, ctl_toggle, NULL);
    icon_btn(m, MD_ICON_SKIP_NEXT, false, ctl_next, NULL);
}

static void on_create(t5_app_t *app)
{
    ui.app = app;
    snprintf(ui.data, sizeof ui.data, "%s", t5_app_data_dir(app));
    for (int i = 0; i < 64; i++) ui.thumb_rows[i].album = -1;
    art_start();
    if (!player_start(app)) toast("Can't start the player");
    load_state();

    lv_obj_t *scr = t5_app_screen(app);
    ui.root = lv_obj_create(scr);
    lv_obj_remove_style_all(ui.root);
    lv_obj_set_size(ui.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(ui.root, LV_FLEX_FLOW_COLUMN);
    ui.tabs = md_tabs_create(ui.root);
    lv_obj_set_width(ui.tabs, LV_PCT(100));
    lv_obj_set_flex_grow(ui.tabs, 1);

    lv_obj_t *p = tab_page(ui.tabs, "Songs", &ui.songs, V_SONGS);
    lv_obj_t *head = button_row(p);
    ui.songs_count = md_label_create(head, "", MD_TYPE_BODY2);
    lv_obj_set_flex_grow(ui.songs_count, 1);
    lv_obj_add_event_cb(md_button_create(head, "Shuffle all", MD_BUTTON_TEXT), shuffle_all, LV_EVENT_CLICKED, NULL);
    icon_btn(head, MD_ICON_REFRESH, false, rescan_clicked, NULL);
    make_list(p, &ui.songs, 0, 64);
    add_notice(p, &ui.songs);

    p = tab_page(ui.tabs, "Albums", &ui.albums, V_ALBUMS);
    make_list(p, &ui.albums, 0, 64);
    add_notice(p, &ui.albums);

    p = tab_page(ui.tabs, "Artists", &ui.artists, V_ARTISTS);
    make_list(p, &ui.artists, 0, 64);
    add_notice(p, &ui.artists);

    p = tab_page(ui.tabs, "Playlists", &ui.playlists, V_PLAYLISTS);
    head = button_row(p);
    lv_obj_add_event_cb(md_button_create(head, "New playlist", MD_BUTTON_TEXT), new_playlist, LV_EVENT_CLICKED, NULL);
    make_list(p, &ui.playlists, 0, 64);
    add_notice(p, &ui.playlists);

    build_mini(ui.root);
    lists_from_lib();
    lv_timer_create(tick, 250, NULL);
    tick(NULL);
    ask_read(NULL);
}

static void on_suspend(t5_app_t *app)
{
    (void)app;
    save_state();
}

static void on_destroy(t5_app_t *app)
{
    (void)app;
    save_state();
    player_stop();
    art_stop();
}

static void on_rotate(t5_app_t *app, int degrees)
{
    (void)app;
    (void)degrees;
    if (ui.np.page) {                                   /* lay it out again for the new shape */
        lv_obj_delete(ui.np.page);
        np_build();
    }
}

T5_APP(.on_create = on_create, .on_suspend = on_suspend, .on_destroy = on_destroy, .on_rotate = on_rotate);
