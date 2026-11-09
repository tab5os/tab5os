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
 * Look: the OS's dark scheme and type roles, so it sits with the rest of the
 * system. Every list row has the same anatomy: a 40 dp tile (the cover, or a
 * coloured tile with the album's initial), two lines, the duration and a
 * menu button. The accent marks only what's playing and what's on. Now
 * playing takes on the colour of the cover that's playing.
 *
 * The queue and position are kept across launches (queue.txt in the app's
 * data folder), and the library index too (library.bin), so the next start
 * reads tags only for new or changed files.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <t5/app.h>
#include <t5/audio.h>
#include <t5/shell.h>
#include <t5/ui.h>
#include "art.h"
#include "library.h"
#include "m3u.h"
#include "music_icons.h"
#include "player.h"

#define ROW_DP 64
#define TILE_DP 40
#define TEXT_X_DP 56              /* md_list's text column */
#define TRAIL_DP 40               /* the duration */
#define MORE_DP 40                /* the row's menu button */
#define HEAD_ART_DP 120
#define MINI_DP 64
#define THUMBS_MAX 64
#define DECOS 192
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
    lv_obj_t *head_img;           /* the header's cover */
    lv_image_dsc_t *head_dsc;
    uint32_t head_key;
} view_t;

typedef struct {
    uint32_t key;
    lv_image_dsc_t *img;          /* NULL: no art (looked) */
    uint32_t used;
} thumb_t;

/* What we add to an md_list row, made the first time it is bound. */
typedef struct {
    lv_obj_t *row;
    lv_obj_t *tile, *glyph, *img, *eq;
    lv_obj_t *trail, *more;
    view_t *view;
    int index;
    uint32_t art_key;             /* the album cover it shows, 0: none */
} deco_t;

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

    lv_obj_t *mini, *mini_tile, *mini_glyph, *mini_art, *mini_title, *mini_sub, *mini_play, *mini_bar;
    lv_image_dsc_t *mini_img;
    uint32_t mini_key;

    struct {
        lv_obj_t *page, *frame, *glyph, *art, *title, *artist, *album, *slider, *pos, *dur, *play, *shuffle, *repeat;
        lv_image_dsc_t *img;
        uint32_t key;
        int art_px;
        bool land;
    } np;
    view_t *queue_view;
    view_t *detail;

    player_state_t ps;
    uint32_t shown_version;
    char shown_path[M3U_PATH_MAX];
    uint32_t last_save, saved_version;

    thumb_t thumbs[THUMBS_MAX];
    uint32_t thumb_clock;
    deco_t decos[DECOS];

    /* what the next menu acts on */
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
static void row_menu(view_t *v, int index, lv_obj_t *anchor);

/* ---- small helpers -------------------------------------------------------------------- */
static const md_scheme_t *sc(void)
{
    return md_scheme();
}

static const lib_track_t *track(int i)
{
    return ui.lib && i >= 0 && i < ui.lib->n ? &ui.lib->t[i] : NULL;
}

static const char *or_unknown(const char *s, const char *d)
{
    return s && s[0] ? s : d;
}

static uint32_t hash(const char *p)
{
    uint32_t h = 2166136261u;
    while (*p) h = (h ^ (uint8_t)*p++) * 16777619u;
    return h | 1u;                                   /* never 0 */
}

/* Record-sleeve colours for covers we don't have: muted, so white initials
 * read on them and they sit quietly in the dark scheme. A name always gets
 * the same one. */
static md_rgb_t tile_color(const char *name)
{
    static const md_rgb_t pal[] = { 0x3E5C76, 0x5A4E7C, 0x2E6B60, 0x7A5240, 0x6B6234, 0x4B5563 };
    return pal[hash(name ? name : "") % (sizeof pal / sizeof pal[0])];
}

/* The first character, upper case if it's a letter; a note for an album
 * or artist the tags don't name. */
static void initial(const char *name, char out[8])
{
    if (!name || !name[0] || !strcmp(name, "Unknown album") || !strcmp(name, "Unknown artist")) {
        out[0] = 0;                                  /* tile_glyph draws a note */
        return;
    }
    const unsigned char *s = (const unsigned char *)name;
    while (*s == ' ' || *s == '(' || *s == '[' || *s == '"' || *s == '\'') s++;
    size_t n = *s < 0x80 ? 1 : *s < 0xe0 ? 2 : *s < 0xf0 ? 3 : 4;
    if (!*s) {
        strcpy(out, "?");
        return;
    }
    memcpy(out, s, n);
    out[n] = 0;
    if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 32);
}

/* One line, cut with dots. LVGL's dots mode still wraps onto more lines
 * unless the height is fixed, so fix it to one line of the label's font. */
static void one_line(lv_obj_t *label)
{
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(label, lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN)));
}

/* A tile's mark: the name's initial in a text role, or a music note in the
 * icon font when the tags don't name it. */
static void tile_glyph(lv_obj_t *label, const char *name, md_type_role_t role, int icon_dp)
{
    char g[8];
    initial(name, g);
    lv_label_set_text(label, g[0] ? g : MD_ICON_MUSIC_NOTE);
    lv_obj_set_style_text_font(label, g[0] ? md_font(role) : md_icon_font(icon_dp), 0);
}

static lv_obj_t *glyph_label(lv_obj_t *parent, const char *glyph, bool app_font, int dp)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, glyph);
    lv_obj_set_style_text_font(l, app_font ? (dp >= 40 ? &music_icons_80 : &music_icons_48) : md_icon_font(dp), 0);
    lv_obj_set_style_text_color(l, md_color(sc()->c.on_surface), 0);
    return l;
}

/* A round, flat icon button (48 dp touch target). Toggles get a dot under
 * the icon when they're on. */
static lv_obj_t *icon_btn(lv_obj_t *parent, const char *glyph, bool app_font, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, T5_DP(48), T5_DP(48));
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_clickable(b, true);
    md_ripple_attach(b);
    lv_obj_t *l = glyph_label(b, glyph, app_font, 24);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static void set_icon(lv_obj_t *btn, const char *glyph)
{
    lv_label_set_text(lv_obj_get_child(btn, 0), glyph);
}

static void toggle_dot(lv_obj_t *btn)
{
    lv_obj_t *dot = lv_obj_create(btn);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, T5_DP(4), T5_DP(4));
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, md_color(sc()->c.secondary), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, 0, -T5_DP(4));
    lv_obj_set_hidden(dot, true);
}

static void set_on(lv_obj_t *btn, bool on)
{
    lv_obj_t *l = lv_obj_get_child(btn, 0);
    lv_obj_set_style_text_color(l, md_color(on ? sc()->c.secondary : sc()->c.on_surface), 0);
    lv_obj_set_style_text_opa(l, on ? LV_OPA_COVER : sc()->on_surface.medium, 0);
    if (lv_obj_get_child_count(btn) > 1) lv_obj_set_hidden(lv_obj_get_child(btn, 1), !on);
}

static void toast(const char *t)
{
    T5_IGNORE(t5_shell_toast(t));                    /* a lost toast is harmless */
}

static void data_path(char *out, size_t cap, const char *name)
{
    snprintf(out, cap, "%s/%s", ui.data, name);
}

static void count_text(char *out, size_t cap, int n, const char *one, const char *many)
{
    snprintf(out, cap, "%d %s", n, n == 1 ? one : many);
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

static bool is_playing_path(const char *path)
{
    return ui.ps.has_track && !strcmp(path, ui.ps.path);
}

/* ---- covers: thumbnails, by album ---------------------------------------------------------- */
static uint32_t album_key(int album)
{
    return (ui.lib_gen << 20) | (uint32_t)album;
}

static thumb_t *thumb_find(uint32_t key)
{
    for (int i = 0; i < THUMBS_MAX; i++) {
        if (ui.thumbs[i].key == key) return &ui.thumbs[i];
    }
    return NULL;
}

static bool thumb_on_screen(uint32_t key)
{
    for (int i = 0; i < DECOS; i++) {
        if (ui.decos[i].row && ui.decos[i].art_key == key) return true;
    }
    return false;
}

static void deco_show_art(deco_t *d, const thumb_t *t)
{
    bool has = t && t->img;
    lv_obj_set_hidden(d->img, !has);
    if (has) lv_image_set_src(d->img, t->img);
}

static void thumb_done(uint32_t key, lv_image_dsc_t *img, void *user)
{
    (void)user;
    if (key >> 20 != (ui.lib_gen & 0xfff)) {         /* from before a rescan */
        art_free(img);
        return;
    }
    thumb_t *t = thumb_find(key);
    for (int i = 0; i < THUMBS_MAX && !t; i++) {
        if (!ui.thumbs[i].key) t = &ui.thumbs[i];
    }
    if (!t) {                                        /* the least recently used that isn't on screen */
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
    if (t->key != key) art_free(t->img);
    else if (t->img != img) art_free(t->img);
    t->key = key;
    t->img = img;
    t->used = ++ui.thumb_clock;
    for (int i = 0; i < DECOS; i++) {
        if (ui.decos[i].row && ui.decos[i].art_key == key) deco_show_art(&ui.decos[i], t);
    }
}

static void thumbs_clear(void)
{
    art_cancel(thumb_done);                         /* not the mini player's or Now playing's */
    for (int i = 0; i < DECOS; i++) {
        if (ui.decos[i].img) {
            lv_image_set_src(ui.decos[i].img, NULL);
            lv_obj_set_hidden(ui.decos[i].img, true);
            ui.decos[i].art_key = 0;
        }
    }
    for (int i = 0; i < THUMBS_MAX; i++) {
        art_free(ui.thumbs[i].img);
        ui.thumbs[i] = (thumb_t){ 0 };
    }
}

/* Show an album's cover in a decorated row's tile (requesting it if needed). */
static void deco_album_art(deco_t *d, int album)
{
    const lib_album_t *a = &ui.lib->albums[album];
    uint32_t key = album_key(album);
    d->art_key = key;
    thumb_t *t = thumb_find(key);
    deco_show_art(d, t);
    if (t) {
        t->used = ++ui.thumb_clock;
    } else {
        const lib_track_t *c = &ui.lib->t[a->cover];
        art_request(key, c->path, c->art_off, c->art_len, T5_DP(TILE_DP), thumb_done, NULL);
    }
}

/* ---- list rows ------------------------------------------------------------------------- */
static void more_clicked(lv_event_t *e)
{
    deco_t *d = lv_event_get_user_data(e);
    row_menu(d->view, d->index, lv_event_get_target_obj(e));
}

static deco_t *deco_for(md_list_row_t *r, view_t *v)
{
    deco_t *spare = NULL;
    for (int i = 0; i < DECOS; i++) {
        if (ui.decos[i].row == r->row) return &ui.decos[i];
        if (!ui.decos[i].row && !spare) spare = &ui.decos[i];
    }
    if (!spare) return NULL;
    deco_t *d = spare;
    d->row = r->row;
    d->view = v;
    lv_obj_set_hidden(r->icon, true);                   /* the tile takes its place */

    d->tile = lv_obj_create(r->row);
    lv_obj_remove_style_all(d->tile);
    lv_obj_set_size(d->tile, T5_DP(TILE_DP), T5_DP(TILE_DP));
    lv_obj_align(d->tile, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_clip_corner(d->tile, true, 0);
    lv_obj_set_clickable(d->tile, false);
    d->glyph = md_label_create(d->tile, "", MD_TYPE_SUBTITLE1);
    lv_obj_center(d->glyph);
    d->img = lv_image_create(d->tile);
    lv_obj_set_size(d->img, LV_PCT(100), LV_PCT(100));
    lv_obj_set_hidden(d->img, true);
    d->eq = glyph_label(d->tile, MD_ICON_GRAPHIC_EQ, false, 24);
    lv_obj_set_size(d->eq, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(d->eq, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(d->eq, LV_OPA_60, 0);
    lv_obj_set_style_text_color(d->eq, md_color(sc()->c.secondary), 0);
    lv_obj_set_style_text_align(d->eq, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(d->eq, T5_DP(8), 0);
    lv_obj_set_hidden(d->eq, true);

    d->trail = md_label_create(r->row, "", MD_TYPE_CAPTION);
    lv_obj_set_width(d->trail, T5_DP(TRAIL_DP));
    lv_obj_set_style_text_align(d->trail, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_opa(d->trail, sc()->on_surface.medium, 0);
    d->more = icon_btn(r->row, MD_ICON_MORE_VERT, false, more_clicked, d);
    lv_obj_set_size(d->more, T5_DP(MORE_DP), T5_DP(MORE_DP));
    lv_obj_set_style_text_opa(lv_obj_get_child(d->more, 0), sc()->on_surface.medium, 0);
    lv_obj_align(d->more, LV_ALIGN_RIGHT_MID, T5_DP(8), 0);
    return d;
}

/* The row's text from the tile to the trailing parts. */
static void deco_layout(deco_t *d, md_list_row_t *r, bool trail, bool more, bool two_lines)
{
    lv_obj_set_hidden(d->trail, !trail);
    lv_obj_set_hidden(d->more, !more);
    int32_t right = more ? T5_DP(MORE_DP - 8) : 0;
    if (trail) lv_obj_align(d->trail, LV_ALIGN_RIGHT_MID, -right, 0);
    int32_t w = lv_obj_get_content_width(r->row);
    if (w <= 0) w = lv_display_get_horizontal_resolution(NULL) - T5_DP(32);
    w -= T5_DP(TEXT_X_DP) + right + (trail ? T5_DP(TRAIL_DP + 4) : 0);
    lv_obj_t *labels[] = { r->primary, r->secondary };
    for (int i = 0; i < 2; i++) {
        if (labels[i] && w > 0 && lv_obj_get_width(labels[i]) != w) {
            lv_obj_set_width(labels[i], w);
            one_line(labels[i]);
        }
    }
    /* md_list's two-line spots, or centred when there's no second line */
    int32_t h = T5_DP(ROW_DP);
    if (r->secondary) {
        lv_obj_set_hidden(r->secondary, !two_lines);
        if (two_lines) lv_obj_align(r->primary, LV_ALIGN_TOP_LEFT, T5_DP(TEXT_X_DP), h / 2 - T5_DP(22));
        else lv_obj_align(r->primary, LV_ALIGN_LEFT_MID, T5_DP(TEXT_X_DP), 0);
    }
}

/* name: the tile shows its initial; icon: an icon (app font) instead. */
static void deco_tile(deco_t *d, md_rgb_t bg, bool round, const char *name, const char *icon)
{
    lv_obj_set_style_radius(d->tile, round ? LV_RADIUS_CIRCLE : T5_DP(4), 0);
    lv_obj_set_style_bg_color(d->tile, md_color(bg), 0);
    lv_obj_set_style_bg_opa(d->tile, LV_OPA_COVER, 0);
    if (icon) {
        lv_label_set_text(d->glyph, icon);
        lv_obj_set_style_text_font(d->glyph, &music_icons_48, 0);
    } else {
        tile_glyph(d->glyph, name, MD_TYPE_SUBTITLE1, 24);
    }
    lv_obj_set_style_text_color(d->glyph, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_opa(d->glyph, LV_OPA_COVER, 0);
    d->art_key = 0;
    lv_obj_set_hidden(d->img, true);
}

static void title_color(lv_obj_t *label, bool now)
{
    lv_obj_set_style_text_color(label, md_color(now ? sc()->c.secondary : sc()->c.on_surface), 0);
}

/* A song: its album's cover (or tile), title, artist, duration, menu.
 * number >= 0: an album page's row, with the track number in the tile. */
static void bind_song(md_list_row_t *r, view_t *v, int index, int ti, int number)
{
    deco_t *d = deco_for(r, v);
    const lib_track_t *t = track(ti);
    if (!d || !t) return;
    d->index = index;
    bool now = is_playing_path(t->path);
    char g[8], dur[16];
    lib_fmt_time(dur, sizeof dur, t->duration_ms);
    lv_label_set_text(d->trail, dur);
    lv_label_set_text(r->primary, t->title);
    title_color(r->primary, now);
    if (number >= 0) {
        /* the track number, quiet; the playing mark in its place */
        lv_obj_set_style_bg_opa(d->tile, LV_OPA_TRANSP, 0);
        lv_obj_set_hidden(d->img, true);
        d->art_key = 0;
        if (number) snprintf(g, sizeof g, "%d", number);
        else g[0] = 0;
        lv_label_set_text(d->glyph, now ? "" : g);
        lv_obj_set_style_text_font(d->glyph, md_font(MD_TYPE_BODY2), 0);
        lv_obj_set_style_text_color(d->glyph, md_color(sc()->c.on_surface), 0);
        lv_obj_set_style_text_opa(d->glyph, sc()->on_surface.medium, 0);
        lv_obj_set_hidden(d->eq, !now);
        lv_obj_set_style_bg_opa(d->eq, LV_OPA_TRANSP, 0);
        const lib_album_t *a = &ui.lib->albums[t->album_ix];
        bool other = t->artist[0] && tag_casecmp(t->artist, a->artist);   /* a guest on the album */
        if (r->secondary) lv_label_set_text(r->secondary, other ? t->artist : "");
        deco_layout(d, r, true, true, other);
        return;
    }
    const lib_album_t *a = &ui.lib->albums[t->album_ix];
    deco_tile(d, tile_color(a->name), false, a->name, NULL);
    deco_album_art(d, t->album_ix);
    lv_obj_set_hidden(d->eq, !now);
    lv_obj_set_style_bg_opa(d->eq, LV_OPA_60, 0);
    if (r->secondary) lv_label_set_text(r->secondary, or_unknown(t->artist, "Unknown artist"));
    deco_layout(d, r, true, true, true);
}

static void bind(md_list_row_t *r, uint32_t index, void *user)
{
    view_t *v = user;
    int i = (int)index;
    if (!ui.lib) return;
    char sub[96];
    switch (v->kind) {
    case V_SONGS:
        if (i < ui.lib->n) bind_song(r, v, i, ui.lib->by_title[i], -1);
        break;
    case V_TRACKS:
        if (i < v->n) bind_song(r, v, i, v->tracks[i], v->album >= 0 ? ui.lib->t[v->tracks[i]].track : -1);
        break;
    case V_QUEUE: {
        char p[M3U_PATH_MAX];
        if (!player_queue_item(i, p, sizeof p)) break;
        int ti = lib_find(ui.lib, p);
        if (ti >= 0) {
            bind_song(r, v, i, ti, -1);
            bool now = i == ui.ps.qpos;                 /* the queue marks its position, not the path */
            deco_t *d = deco_for(r, v);
            if (d) lv_obj_set_hidden(d->eq, !now);
            title_color(r->primary, now);
        } else {
            deco_t *d = deco_for(r, v);
            if (!d) break;
            d->index = i;
            const char *b = strrchr(p, '/');
            lv_label_set_text(r->primary, b ? b + 1 : p);
            if (r->secondary) lv_label_set_text(r->secondary, "Not in the library");
            deco_tile(d, sc()->c.surface, false, "", NULL);
            lv_label_set_text(d->trail, "");
            deco_layout(d, r, false, true, true);
        }
        break;
    }
    case V_ALBUMS: {
        if (i >= ui.lib->nalbums) break;
        deco_t *d = deco_for(r, v);
        if (!d) break;
        d->index = i;
        const lib_album_t *a = &ui.lib->albums[i];
        lv_label_set_text(r->primary, a->name);
        title_color(r->primary, false);
        lv_label_set_text(r->secondary, a->artist);
        deco_tile(d, tile_color(a->name), false, a->name, NULL);
        deco_album_art(d, i);
        lv_obj_set_hidden(d->eq, true);
        if (a->year) snprintf(sub, sizeof sub, "%u", a->year);
        else sub[0] = 0;
        lv_label_set_text(d->trail, sub);
        deco_layout(d, r, true, false, true);
        break;
    }
    case V_ARTISTS: {
        if (i >= ui.lib->nartists) break;
        deco_t *d = deco_for(r, v);
        if (!d) break;
        d->index = i;
        const lib_artist_t *a = &ui.lib->artists[i];
        lv_label_set_text(r->primary, a->name);
        title_color(r->primary, false);
        char al[32], so[32];
        count_text(al, sizeof al, a->nalbums, "album", "albums");
        count_text(so, sizeof so, a->n, "song", "songs");
        snprintf(sub, sizeof sub, "%s, %s", al, so);
        lv_label_set_text(r->secondary, sub);
        deco_tile(d, tile_color(a->name), true, a->name, NULL);   /* people are round, records square */
        lv_obj_set_hidden(d->eq, true);
        deco_layout(d, r, false, false, true);
        break;
    }
    case V_PLAYLISTS: {
        if (i >= ui.lib->npl) break;
        deco_t *d = deco_for(r, v);
        if (!d) break;
        d->index = i;
        lv_label_set_text(r->primary, ui.lib->pl[i].name);
        title_color(r->primary, false);
        count_text(sub, sizeof sub, ui.lib->pl[i].n, "song", "songs");
        lv_label_set_text(r->secondary, sub);
        deco_tile(d, tile_color(ui.lib->pl[i].name), false, NULL, MI_QUEUE_MUSIC);
        lv_obj_set_hidden(d->eq, true);
        deco_layout(d, r, false, false, true);
        break;
    }
    }
}

/* ---- menus ------------------------------------------------------------------------------ */
static void playlist_add_to(const char *path, int ti);
static void open_name_page(const char *title, const char *initial_text, const char *pl_path, int add_track);
static void with_write(void (*fn)(void *), void *arg);
static void remove_from_playlist(void *arg);

static void add_menu_picked(int item, void *ud)
{
    (void)ud;
    if (item < 0) return;
    if (item == ui.menu_npl) {                          /* New playlist... */
        open_name_page("New playlist", "", NULL, ui.menu_track);
        return;
    }
    if (item < ui.menu_npl) playlist_add_to(ui.lib->pl[ui.menu_pl[item]].path, ui.menu_track);
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

static void track_menu_picked(int item, void *ud)
{
    lv_obj_t *anchor = ud;
    view_t *v = ui.menu_view;
    if (item < 0 || !v) return;
    if (v->kind == V_QUEUE) {
        if (item == 0) player_remove(ui.menu_index);
        return;
    }
    const lib_track_t *t = track(ui.menu_track);
    if (!t) return;
    switch (item) {
    case 0: player_add(t->path, true); toast("Plays next"); break;
    case 1: player_add(t->path, false); toast("Added to the queue"); break;
    case 2: show_add_menu(anchor); break;
    case 3: open_album(t->album_ix); break;
    case 4: open_artist(t->artist_ix); break;
    case 5:
        if (v->pl_path[0]) with_write(remove_from_playlist, NULL);
        break;
    }
}

/* A song's menu, from its ⋮ or a long press. */
static void row_menu(view_t *v, int index, lv_obj_t *anchor)
{
    int n;
    const int *tracks = view_tracks(v, &n);
    ui.menu_view = v;
    ui.menu_index = index;
    ui.menu_track = -1;
    if (v->kind == V_QUEUE) {
        static const char *q[] = { "Remove from the queue" };
        md_menu_show(anchor, q, 1, track_menu_picked, anchor);
        return;
    }
    if ((v->kind != V_SONGS && v->kind != V_TRACKS) || !tracks || index >= n) return;
    ui.menu_track = tracks[index];
    static const char *items[] = { "Play next", "Add to the queue", "Add to a playlist\xe2\x80\xa6", "Go to the album",
                                   "Go to the artist", "Remove from this playlist" };
    md_menu_show(anchor, items, v->pl_path[0] ? 6 : 5, track_menu_picked, anchor);
}

static void long_pressed(uint32_t index, void *user)
{
    row_menu(user, (int)index, lv_indev_get_active_obj());
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
        if (i < n) play_tracks(tracks, n, i, ui.ps.shuffle);
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

static lv_obj_t *make_list(lv_obj_t *parent, view_t *v, uint32_t count)
{
    v->list = md_list_create(parent, count, ROW_DP, bind, clicked, v);
    md_list_set_long_press(v->list, long_pressed);
    lv_obj_set_width(v->list, LV_PCT(100));
    lv_obj_set_flex_grow(v->list, 1);
    return v->list;
}

/* Forget the decorations of a list that is going away. */
static void list_deleted(lv_obj_t *list)
{
    for (int i = 0; i < DECOS; i++) {
        if (ui.decos[i].row && lv_obj_get_parent(ui.decos[i].row) == list) ui.decos[i] = (deco_t){ 0 };
    }
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
    if (v->list) list_deleted(v->list);
    if (ui.queue_view == v) ui.queue_view = NULL;
    if (ui.detail == v) ui.detail = NULL;
    if (v->head_img) lv_image_set_src(v->head_img, NULL);
    art_free(v->head_dsc);
    free(v->tracks);
    free(v);
}

static lv_obj_t *page_new(const char *title, lv_obj_t **bar)
{
    return md_page_create(t5_app_screen(ui.app), title, page_back, NULL, bar);
}

/* Browsing pages stop above the mini player, so play and pause stay at hand.
 * Over Now playing (the queue, an album from there) they cover it all. */
static void page_fit(lv_obj_t *page)
{
    if (!page) return;
    if (ui.np.page && lv_obj_get_index(page) > lv_obj_get_index(ui.np.page)) {
        lv_obj_set_height(page, LV_PCT(100));
        return;
    }
    lv_obj_t *scr = t5_app_screen(ui.app);
    lv_obj_update_layout(scr);
    lv_obj_set_height(page, lv_obj_get_content_height(scr) - T5_DP(MINI_DP));
}

static lv_obj_t *button_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, T5_DP(8), 0);
    lv_obj_set_scrollable(row, false);
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

static void head_art_done(uint32_t key, lv_image_dsc_t *img, void *user)
{
    (void)user;
    view_t *v = ui.detail;
    if (!v || key != v->head_key || !img) {
        art_free(img);
        return;
    }
    lv_image_set_src(v->head_img, img);
    lv_obj_set_hidden(v->head_img, false);
    art_free(v->head_dsc);
    v->head_dsc = img;
}

typedef enum { HEAD_ALBUM, HEAD_ARTIST, HEAD_PLAYLIST } head_t;

/* A page of songs: a header (cover or tile, name, who, what), Play and
 * Shuffle, then the list. Takes tracks. */
static view_t *track_page(head_t kind, const char *name, const char *who, const char *what, int *tracks, int n, int album,
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
    lv_obj_t *page = page_new("", &bar);
    if (bar_out) *bar_out = bar;
    v->page = page;
    lv_obj_add_event_cb(page, view_deleted, LV_EVENT_DELETE, v);
    lv_obj_t *body = md_page_body(page);
    lv_obj_set_scrollable(body, false);

    lv_obj_t *head = lv_obj_create(body);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_hor(head, T5_DP(16), 0);
    lv_obj_set_style_pad_top(head, T5_DP(8), 0);
    lv_obj_set_style_pad_bottom(head, T5_DP(12), 0);
    lv_obj_set_style_pad_column(head, T5_DP(16), 0);
    lv_obj_set_scrollable(head, false);

    /* the cover, or the same tile the lists show */
    lv_obj_t *tile = lv_obj_create(head);
    lv_obj_remove_style_all(tile);
    lv_obj_set_size(tile, T5_DP(HEAD_ART_DP), T5_DP(HEAD_ART_DP));
    lv_obj_set_style_radius(tile, kind == HEAD_ARTIST ? LV_RADIUS_CIRCLE : T5_DP(6), 0);
    lv_obj_set_style_clip_corner(tile, true, 0);
    lv_obj_set_style_bg_color(tile, md_color(tile_color(name)), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_t *gl;
    if (kind == HEAD_PLAYLIST) {
        gl = glyph_label(tile, MI_QUEUE_MUSIC, true, 40);
    } else {
        gl = md_label_create(tile, "", MD_TYPE_HEADLINE4);
        tile_glyph(gl, name, MD_TYPE_HEADLINE4, 40);
    }
    lv_obj_set_style_text_color(gl, lv_color_hex(0xffffff), 0);
    lv_obj_center(gl);
    if (kind == HEAD_ALBUM) {
        v->head_img = lv_image_create(tile);
        lv_obj_set_size(v->head_img, LV_PCT(100), LV_PCT(100));
        lv_obj_set_hidden(v->head_img, true);
        const lib_track_t *c = &ui.lib->t[ui.lib->albums[album].cover];
        v->head_key = album_key(album) ^ 0x40000000u;
        ui.detail = v;
        art_request(v->head_key, c->path, c->art_off, c->art_len, T5_DP(HEAD_ART_DP), head_art_done, NULL);
    }

    lv_obj_t *col = lv_obj_create(head);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, T5_DP(2), 0);
    lv_obj_set_scrollable(col, false);
    lv_obj_t *h = md_label_create(col, name, MD_TYPE_HEADLINE5);
    lv_obj_set_width(h, LV_PCT(100));
    one_line(h);
    if (who && who[0]) {
        lv_obj_t *w = md_label_create(col, who, MD_TYPE_SUBTITLE1);
        lv_obj_set_width(w, LV_PCT(100));
        one_line(w);
    }
    lv_obj_t *s = md_label_create(col, what, MD_TYPE_BODY2);
    lv_obj_set_width(s, LV_PCT(100));
    one_line(s);
    lv_obj_set_style_text_opa(s, sc()->on_surface.medium, 0);
    lv_obj_t *br = button_row(col);
    lv_obj_set_style_pad_top(br, T5_DP(8), 0);
    lv_obj_add_event_cb(md_button_create(br, "Play", MD_BUTTON_CONTAINED), play_view, LV_EVENT_CLICKED, v);
    lv_obj_add_event_cb(md_button_create(br, "Shuffle", MD_BUTTON_OUTLINED), shuffle_view, LV_EVENT_CLICKED, v);

    md_divider_create(body);
    make_list(body, v, (uint32_t)n);
    page_fit(page);
    ui.detail = v;
    return v;
}

static uint32_t total_ms(const int *t, int n)
{
    uint32_t ms = 0;
    for (int i = 0; i < n; i++) ms += ui.lib->t[t[i]].duration_ms;
    return ms;
}

/* "2024, 3 songs, 1:30" */
static void what_text(char *out, size_t cap, uint16_t year, const int *t, int n)
{
    char so[32], d[16];
    count_text(so, sizeof so, n, "song", "songs");
    lib_fmt_time(d, sizeof d, total_ms(t, n));
    if (year) snprintf(out, cap, "%u, %s, %s", year, so, d);
    else snprintf(out, cap, "%s, %s", so, d);
}

static void open_album(int a)
{
    if (!ui.lib || a < 0 || a >= ui.lib->nalbums) return;
    const lib_album_t *al = &ui.lib->albums[a];
    int *t = malloc((size_t)al->n * sizeof *t);
    if (!t) return;
    memcpy(t, al->tracks, (size_t)al->n * sizeof *t);
    char what[96];
    what_text(what, sizeof what, al->year, t, al->n);
    track_page(HEAD_ALBUM, al->name, al->artist, what, t, al->n, a, NULL);
}

static void open_artist(int a)
{
    if (!ui.lib || a < 0 || a >= ui.lib->nartists) return;
    const lib_artist_t *ar = &ui.lib->artists[a];
    int *t = malloc((size_t)ar->n * sizeof *t);
    if (!t) return;
    memcpy(t, ar->tracks, (size_t)ar->n * sizeof *t);
    char al[32], what[96];
    count_text(al, sizeof al, ar->nalbums, "album", "albums");
    what_text(what, sizeof what, 0, t, ar->n);
    char both[160];
    snprintf(both, sizeof both, "%s, %s", al, what);
    track_page(HEAD_ARTIST, ar->name, NULL, both, t, ar->n, -1, NULL);
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
    char what[96];
    what_text(what, sizeof what, 0, t, n);
    lv_obj_t *bar;
    view_t *v = track_page(HEAD_PLAYLIST, name, "Playlist", what, t, n, -1, &bar);
    if (!v) return;
    snprintf(v->pl_path, sizeof v->pl_path, "%s", path);
    md_appbar_add_action(bar, MD_ICON_MORE_VERT, playlist_menu, v);
    if (missing) {
        char m[96];
        snprintf(m, sizeof m, "%d song%s in this playlist %s not on the SD card", missing, missing == 1 ? "" : "s",
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

static bool write_playlist(const char *path, const int *t, int n, const char *ok_msg)
{
    if (lib_playlist_write(ui.lib, path, t, n)) {
        if (ok_msg) toast(ok_msg);
        return true;
    }
    toast("Can't save the playlist: the SD card may be full or read-only");
    return false;
}

/* After the person allows changing files in shared folders. */
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
    else toast("To change playlists, allow Music to change files in App Manager");
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
        if (write_playlist(j->path, t2, n + 1, "Added to the playlist")) playlists_changed();
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
    if (write_playlist(v->pl_path, v->tracks, v->n, "Removed from the playlist")) playlists_changed();
    md_list_set_count(v->list, (uint32_t)v->n);
    md_list_refresh(v->list);
}

/* The name page: a new playlist, or a rename. */
typedef struct {
    lv_obj_t *page, *field;
    char old[M3U_PATH_MAX];       /* rename: the playlist's path */
    int add_track;                /* new: a song to start it with, or -1 */
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
        md_textfield_set_helper(np->field, "Type a name for the playlist", true);
        return;
    }
    snprintf(path, sizeof path, PLAYLIST_DIR "/%s.m3u", name);
    struct stat st;
    if (stat(path, &st) == 0 && tag_casecmp(path, np->old)) {
        md_textfield_set_helper(np->field, "A playlist with that name already exists", true);
        return;
    }
    if (np->old[0]) {
        mkdir(PLAYLIST_DIR, 0775);
        if (rename(np->old, path) != 0) {
            toast("Can't rename the playlist");
            return;
        }
        if (ui.detail && !strcmp(ui.detail->pl_path, np->old)) lv_obj_delete(ui.detail->page);
        toast("Playlist renamed");
    } else {
        int t = np->add_track;
        write_playlist(path, &t, t >= 0 ? 1 : 0, t >= 0 ? "Playlist made, with the song in it" : "Playlist made");
    }
    playlists_changed();
    lv_obj_delete(np->page);
}

static void name_done(lv_event_t *e)
{
    with_write(do_name, lv_event_get_user_data(e));
}

static void open_name_page(const char *title, const char *initial_text, const char *pl_path, int add_track)
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
    md_textfield_set_text(np->field, initial_text);
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
        md_dialog_show("Delete this playlist?", "Its songs stay in your library.", "Delete", "Cancel", delete_answer, j);
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
    make_list(body, v, (uint32_t)ui.ps.qlen);
    page_fit(page);
    ui.queue_view = v;
}

/* ---- now playing --------------------------------------------------------------------------- */
/* The cover's average colour, darkened until white text reads on it. */
static md_rgb_t cover_tint(const lv_image_dsc_t *img, md_rgb_t fallback)
{
    uint32_t r = 0, g = 0, b = 0, n = 0;
    md_rgb_t base = fallback;
    if (img && img->data) {
        const uint16_t *px = (const uint16_t *)img->data;
        uint32_t w = img->header.w, h = img->header.h;
        for (uint32_t y = 0; y < h; y += h / 24 + 1) {
            for (uint32_t x = 0; x < w; x += w / 24 + 1) {
                uint16_t p = px[y * w + x];
                r += (p >> 11) << 3, g += ((p >> 5) & 0x3f) << 2, b += (p & 0x1f) << 3;
                n++;
            }
        }
        if (n) base = (r / n) << 16 | (g / n) << 8 | (b / n);
    }
    /* keep the hue, cap the brightest channel, then sink it into the background */
    uint32_t cr = base >> 16 & 0xff, cg = base >> 8 & 0xff, cb = base & 0xff;
    uint32_t mx = cr > cg ? (cr > cb ? cr : cb) : (cg > cb ? cg : cb);
    if (mx > 0x70) cr = cr * 0x70 / mx, cg = cg * 0x70 / mx, cb = cb * 0x70 / mx;
    return md_mix(cr << 16 | cg << 8 | cb, sc()->c.background, 150);
}

/* One even wash: a fade to the background would band in 16-bit colour. */
static void np_tint(md_rgb_t tint)
{
    if (ui.np.page) lv_obj_set_style_bg_color(ui.np.page, md_color(tint), 0);
}

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
    lv_obj_set_hidden(ui.np.art, img == NULL);
    art_free(ui.np.img);
    ui.np.img = img;
    np_tint(cover_tint(img, tile_color(ui.ps.tags.album[0] ? ui.ps.tags.album : "Unknown album")));
}

static void mini_art_done(uint32_t key, lv_image_dsc_t *img, void *user)
{
    (void)user;
    if (key != ui.mini_key) {
        art_free(img);
        return;
    }
    lv_image_set_src(ui.mini_art, img);
    lv_obj_set_hidden(ui.mini_art, img == NULL);
    art_free(ui.mini_img);
    ui.mini_img = img;
}

/* A tile for the playing track's album, until (or if no) cover comes. */
static void tile_for(lv_obj_t *tile, lv_obj_t *glyph, const char *album, bool big)
{
    const char *name = album && album[0] ? album : "Unknown album";
    tile_glyph(glyph, name, big ? MD_TYPE_HEADLINE4 : MD_TYPE_SUBTITLE1, big ? 40 : 24);
    if (!ui.ps.has_track) lv_label_set_text(glyph, "");
    lv_obj_set_style_bg_color(tile, md_color(ui.ps.has_track ? tile_color(name) : sc()->c.surface), 0);
}

static void art_for_track(void)
{
    const tags_t *t = &ui.ps.tags;
    uint32_t key = ui.ps.has_track ? hash(ui.ps.path) : 0;
    if (key != ui.mini_key) {
        ui.mini_key = key;
        lv_image_set_src(ui.mini_art, NULL);
        lv_obj_set_hidden(ui.mini_art, true);
        art_free(ui.mini_img);
        ui.mini_img = NULL;
        tile_for(ui.mini_tile, ui.mini_glyph, t->album, false);
        if (key) art_request(key, ui.ps.path, t->art_off, t->art_len, T5_DP(TILE_DP + 8), mini_art_done, NULL);
    }
    if (ui.np.page && key != ui.np.key) {
        ui.np.key = key;
        lv_image_set_src(ui.np.art, NULL);
        lv_obj_set_hidden(ui.np.art, true);
        art_free(ui.np.img);
        ui.np.img = NULL;
        tile_for(ui.np.frame, ui.np.glyph, t->album, true);
        np_tint(ui.ps.has_track ? cover_tint(NULL, tile_color(t->album[0] ? t->album : "Unknown album"))
                                : sc()->c.background);
        /* its own request key: the mini player's is the same track at another size */
        if (key) art_request(key ^ 0x80000000u, ui.ps.path, t->art_off, t->art_len, ui.np.art_px, np_art_done, NULL);
    }
}

static void ctl_prev(lv_event_t *e) { (void)e; player_prev(); }
static void ctl_next(lv_event_t *e) { (void)e; player_next(); }
/* Play/pause; with nothing queued, play the library (in the shuffle mode it's in). */
static void ctl_toggle(lv_event_t *e)
{
    (void)e;
    if (!ui.ps.qlen && ui.lib && ui.lib->n) {
        play_tracks(ui.lib->by_title, ui.lib->n, ui.ps.shuffle ? rand() % ui.lib->n : 0, ui.ps.shuffle);
        return;
    }
    player_toggle();
}
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
        toast("Play a song from the library to add it to a playlist");
        return;
    }
    ui.menu_track = ti;
    show_add_menu(lv_event_get_target_obj(e));
}

static void np_to_album(lv_event_t *e)
{
    (void)e;
    int ti = ui.ps.has_track ? lib_find(ui.lib, ui.ps.path) : -1;
    if (ti >= 0) open_album(ui.lib->t[ti].album_ix);
}

static void np_to_artist(lv_event_t *e)
{
    (void)e;
    int ti = ui.ps.has_track ? lib_find(ui.lib, ui.ps.path) : -1;
    if (ti >= 0) open_artist(ui.lib->t[ti].artist_ix);
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

static lv_obj_t *np_text(lv_obj_t *col, md_type_role_t role, lv_opa_t opa, lv_event_cb_t tap)
{
    lv_obj_t *l = md_label_create(col, "", role);
    lv_obj_set_width(l, LV_PCT(100));
    one_line(l);
    lv_obj_set_style_text_align(l, ui.np.land ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_opa(l, opa, 0);
    if (tap) {
        lv_obj_set_clickable(l, true);
        lv_obj_add_event_cb(l, tap, LV_EVENT_CLICKED, NULL);
    }
    return l;
}

static void np_build(void)
{
    int32_t hor = lv_display_get_horizontal_resolution(NULL), ver = lv_display_get_vertical_resolution(NULL);
    ui.np.land = hor > ver;
    /* the cover as big as the shape allows: under the bars in landscape,
     * across the width (up to 320 dp) in portrait */
    ui.np.art_px = ui.np.land ? ver - T5_DP(24 + 16 + 48 + 40) : hor - T5_DP(64);
    if (ui.np.art_px > T5_DP(320)) ui.np.art_px = T5_DP(320);

    lv_obj_t *page = lv_obj_create(t5_app_screen(ui.app));
    lv_obj_remove_style_all(page);
    ui.np.page = page;
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(page, false);
    lv_obj_add_event_cb(page, np_deleted, LV_EVENT_DELETE, NULL);
    np_tint(sc()->c.background);

    /* a bar that lets the colour through: down to the library, the queue, add to a playlist */
    lv_obj_t *bar = md_appbar_create(page, "", MD_ICON_KEYBOARD_ARROW_DOWN, page_back, NULL);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(bar, 0, 0);
    lv_obj_t *q = md_appbar_add_action(bar, MI_QUEUE_MUSIC, ctl_queue, NULL);
    lv_obj_set_style_text_font(lv_obj_get_child(q, 0), &music_icons_48, 0);
    lv_obj_t *a = md_appbar_add_action(bar, MI_PLAYLIST_ADD, ctl_add, NULL);
    lv_obj_set_style_text_font(lv_obj_get_child(a, 0), &music_icons_48, 0);

    lv_obj_t *body = lv_obj_create(page);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, ui.np.land ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, ui.np.land ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(body, T5_DP(ui.np.land ? 24 : 24), 0);
    lv_obj_set_style_pad_bottom(body, T5_DP(16), 0);
    lv_obj_set_style_pad_gap(body, T5_DP(ui.np.land ? 32 : 24), 0);
    lv_obj_set_scrollable(body, false);

    /* the cover; under it the album's tile, so there's always something */
    ui.np.frame = lv_obj_create(body);
    lv_obj_remove_style_all(ui.np.frame);
    lv_obj_set_size(ui.np.frame, ui.np.art_px, ui.np.art_px);
    lv_obj_set_style_radius(ui.np.frame, T5_DP(8), 0);
    lv_obj_set_style_clip_corner(ui.np.frame, true, 0);
    lv_obj_set_style_bg_opa(ui.np.frame, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ui.np.frame, md_color(sc()->c.surface), 0);
    ui.np.glyph = md_label_create(ui.np.frame, "", MD_TYPE_HEADLINE4);
    lv_obj_set_style_text_color(ui.np.glyph, lv_color_hex(0xffffff), 0);
    lv_obj_center(ui.np.glyph);
    ui.np.art = lv_image_create(ui.np.frame);
    lv_obj_set_size(ui.np.art, LV_PCT(100), LV_PCT(100));
    lv_obj_set_hidden(ui.np.art, true);

    lv_obj_t *col = lv_obj_create(body);
    lv_obj_remove_style_all(col);
    lv_obj_set_scrollable(col, false);
    if (ui.np.land) lv_obj_set_flex_grow(col, 1);
    lv_obj_set_width(col, ui.np.land ? LV_SIZE_CONTENT : LV_PCT(100));
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui.np.title = np_text(col, MD_TYPE_HEADLINE5, LV_OPA_COVER, NULL);
    ui.np.artist = np_text(col, MD_TYPE_SUBTITLE1, sc()->on_surface.high, np_to_artist);
    ui.np.album = np_text(col, MD_TYPE_BODY2, sc()->on_surface.medium, np_to_album);
    lv_obj_set_style_pad_top(ui.np.artist, T5_DP(4), 0);
    lv_obj_set_style_pad_top(ui.np.album, T5_DP(2), 0);

    ui.np.slider = md_slider_create(col, 0, 1000, 0);
    lv_obj_set_width(ui.np.slider, LV_PCT(100));
    lv_obj_set_style_margin_top(ui.np.slider, T5_DP(20), 0);
    lv_obj_add_event_cb(ui.np.slider, slider_released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(ui.np.slider, slider_moved, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *times = lv_obj_create(col);
    lv_obj_remove_style_all(times);
    lv_obj_set_size(times, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(times, T5_DP(6), 0);
    ui.np.pos = md_label_create(times, "0:00", MD_TYPE_CAPTION);
    lv_obj_set_style_text_opa(ui.np.pos, sc()->on_surface.medium, 0);
    lv_obj_align(ui.np.pos, LV_ALIGN_LEFT_MID, 0, 0);
    ui.np.dur = md_label_create(times, "0:00", MD_TYPE_CAPTION);
    lv_obj_set_style_text_opa(ui.np.dur, sc()->on_surface.medium, 0);
    lv_obj_align(ui.np.dur, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *ctl = lv_obj_create(col);
    lv_obj_remove_style_all(ctl);
    lv_obj_set_size(ctl, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(ctl, T5_DP(8), 0);
    lv_obj_set_flex_flow(ctl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctl, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui.np.shuffle = icon_btn(ctl, MI_SHUFFLE, true, ctl_shuffle, NULL);
    toggle_dot(ui.np.shuffle);
    icon_btn(ctl, MD_ICON_SKIP_PREVIOUS, false, ctl_prev, NULL);
    ui.np.play = md_fab_create(ctl, MD_ICON_PLAY_ARROW, false);
    lv_obj_add_event_cb(ui.np.play, ctl_toggle, LV_EVENT_CLICKED, NULL);
    icon_btn(ctl, MD_ICON_SKIP_NEXT, false, ctl_next, NULL);
    ui.np.repeat = icon_btn(ctl, MI_REPEAT, true, ctl_repeat, NULL);
    toggle_dot(ui.np.repeat);

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
        lv_label_set_text(ui.np.artist, p->has_track ? or_unknown(p->tags.artist, "Unknown artist") : "");
        lv_label_set_text(ui.np.album, p->has_track ? or_unknown(p->tags.album, "") : "Choose a song in the library");
        set_on(ui.np.shuffle, p->shuffle);
        set_icon(ui.np.repeat, p->repeat == REPEAT_ONE ? MI_REPEAT_ONE : MI_REPEAT);
        set_on(ui.np.repeat, p->repeat != REPEAT_OFF);
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
        lv_label_set_text(ui.mini_sub, p->has_track ? or_unknown(p->tags.artist, "Unknown artist") : "Choose a song");
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
    lv_bar_set_value(ui.mini_bar, p->dur_ms ? (int32_t)((uint64_t)p->pos_ms * 1000 / p->dur_ms) : 0, LV_ANIM_OFF);
    if (ui.np.page) np_update(changed);
    if (p->version != ui.saved_version && t5_time_ms() - ui.last_save > SAVE_EVERY_MS) save_state();
}

/* ---- the library: loading, scanning ----------------------------------------------------------- */
static void set_notice(view_t *v, int count, const char *empty)
{
    const char *msg = NULL;
    if (!ui.have_read) msg = "Music needs to read the music folder on the SD card. Tap here to allow it.";
    else if (!count && ui.scanning) msg = "Looking for music on the SD card\xe2\x80\xa6";
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
    const char *none = "No music yet. Copy MP3, FLAC or WAV files into the music folder on the SD card, then tap Rescan.";
    set_notice(&ui.songs, n, none);
    set_notice(&ui.albums, ui.lib ? ui.lib->nalbums : 0, none);
    set_notice(&ui.artists, ui.lib ? ui.lib->nartists : 0, none);
    set_notice(&ui.playlists, ui.lib ? ui.lib->npl : 0,
               "No playlists yet. Tap New playlist, or copy .m3u files into the music folder.");
    char c[96];
    if (ui.scanning) snprintf(c, sizeof c, "Looking for music\xe2\x80\xa6 %d files", ui.scan_files);
    else count_text(c, sizeof c, n, "song", "songs");
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

/* A queued file the library has (the scan just looked at the card). */
static bool in_library(const char *path)
{
    int i = lib_find(ui.lib, path);
    return i >= 0 && !tag_casecmp(ui.lib->t[i].path, path);
}

static void scan_finished(void *arg)
{
    scan_done_t *d = arg;
    ui.scanning = false;
    if (d->lib) {
        set_lib(d->lib);
        player_retain(in_library);                     /* a queue from before, with files since removed */
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
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);    /* no panel behind the head and list */
    lv_obj_set_style_radius(page, 0, 0);
    v->kind = kind;
    v->album = -1;
    return page;
}

/* The line over a tab's list: what's there on the left, actions on the right. */
static lv_obj_t *tab_head(lv_obj_t *page)
{
    lv_obj_t *row = button_row(page);
    lv_obj_set_style_pad_left(row, T5_DP(16), 0);
    lv_obj_set_style_pad_right(row, T5_DP(8), 0);
    lv_obj_set_height(row, T5_DP(48));
    return row;
}

static void add_notice(lv_obj_t *page, view_t *v)
{
    v->notice = md_label_create(page, "", MD_TYPE_BODY1);
    lv_obj_set_width(v->notice, LV_PCT(100));
    lv_label_set_long_mode(v->notice, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_pad_hor(v->notice, T5_DP(32), 0);
    lv_obj_set_style_text_align(v->notice, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_opa(v->notice, sc()->on_surface.medium, 0);
    lv_obj_set_floating(v->notice, true);
    lv_obj_align(v->notice, LV_ALIGN_TOP_MID, 0, T5_DP(96));
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
    lv_obj_set_style_pad_left(m, T5_DP(12), 0);
    lv_obj_set_style_pad_right(m, T5_DP(4), 0);
    lv_obj_set_style_pad_column(m, T5_DP(12), 0);
    lv_obj_set_clickable(m, true);
    lv_obj_add_event_cb(m, mini_clicked, LV_EVENT_CLICKED, NULL);

    /* how far into the song, along the top edge */
    ui.mini_bar = lv_bar_create(m);
    lv_obj_set_floating(ui.mini_bar, true);
    lv_obj_set_size(ui.mini_bar, LV_PCT(100), T5_DP(2));
    lv_obj_align(ui.mini_bar, LV_ALIGN_TOP_LEFT, -T5_DP(12), 0);
    lv_obj_set_style_radius(ui.mini_bar, 0, 0);
    lv_obj_set_style_radius(ui.mini_bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ui.mini_bar, md_color(sc()->c.on_surface), 0);
    lv_obj_set_style_bg_opa(ui.mini_bar, LV_OPA_10, 0);
    lv_obj_set_style_bg_color(ui.mini_bar, md_color(sc()->c.secondary), LV_PART_INDICATOR);
    lv_bar_set_range(ui.mini_bar, 0, 1000);
    lv_obj_set_clickable(ui.mini_bar, false);

    ui.mini_tile = lv_obj_create(m);
    lv_obj_remove_style_all(ui.mini_tile);
    lv_obj_set_size(ui.mini_tile, T5_DP(TILE_DP + 8), T5_DP(TILE_DP + 8));
    lv_obj_set_style_radius(ui.mini_tile, T5_DP(4), 0);
    lv_obj_set_style_clip_corner(ui.mini_tile, true, 0);
    lv_obj_set_style_bg_opa(ui.mini_tile, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ui.mini_tile, md_color(sc()->c.surface), 0);
    lv_obj_set_clickable(ui.mini_tile, false);
    ui.mini_glyph = md_label_create(ui.mini_tile, "", MD_TYPE_SUBTITLE1);
    lv_obj_set_style_text_color(ui.mini_glyph, lv_color_hex(0xffffff), 0);
    lv_obj_center(ui.mini_glyph);
    ui.mini_art = lv_image_create(ui.mini_tile);
    lv_obj_set_size(ui.mini_art, LV_PCT(100), LV_PCT(100));
    lv_obj_set_hidden(ui.mini_art, true);

    lv_obj_t *col = lv_obj_create(m);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_clickable(col, false);
    ui.mini_title = md_label_create(col, "Nothing playing", MD_TYPE_SUBTITLE1);
    lv_obj_set_width(ui.mini_title, LV_PCT(100));
    one_line(ui.mini_title);
    ui.mini_sub = md_label_create(col, "Choose a song", MD_TYPE_BODY2);
    lv_obj_set_width(ui.mini_sub, LV_PCT(100));
    one_line(ui.mini_sub);
    lv_obj_set_style_text_opa(ui.mini_sub, sc()->on_surface.medium, 0);

    ui.mini_play = icon_btn(m, MD_ICON_PLAY_ARROW, false, ctl_toggle, NULL);
    icon_btn(m, MD_ICON_SKIP_NEXT, false, ctl_next, NULL);
}

/* Headphones unplugged: pause, rather than play on through the speaker. */
static void route_changed(t5_event_id_t id, const void *data, size_t len, void *arg)
{
    (void)id;
    (void)arg;
    static bool had = true;
    bool hp = len >= sizeof(t5_ev_audio_t) ? ((const t5_ev_audio_t *)data)->headphones : t5_audio_headphones();
    if (had && !hp) player_pause();
    had = hp;
}

static void on_create(t5_app_t *app)
{
    ui.app = app;
    snprintf(ui.data, sizeof ui.data, "%s", t5_app_data_dir(app));
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
    lv_obj_t *head = tab_head(p);
    ui.songs_count = md_label_create(head, "", MD_TYPE_BODY2);
    lv_obj_set_style_text_opa(ui.songs_count, sc()->on_surface.medium, 0);
    lv_obj_set_flex_grow(ui.songs_count, 1);
    lv_obj_add_event_cb(md_button_create(head, "Shuffle", MD_BUTTON_TEXT), shuffle_all, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(md_button_create(head, "Rescan", MD_BUTTON_TEXT), rescan_clicked, LV_EVENT_CLICKED, NULL);
    make_list(p, &ui.songs, 0);
    add_notice(p, &ui.songs);

    p = tab_page(ui.tabs, "Albums", &ui.albums, V_ALBUMS);
    make_list(p, &ui.albums, 0);
    add_notice(p, &ui.albums);

    p = tab_page(ui.tabs, "Artists", &ui.artists, V_ARTISTS);
    make_list(p, &ui.artists, 0);
    add_notice(p, &ui.artists);

    p = tab_page(ui.tabs, "Playlists", &ui.playlists, V_PLAYLISTS);
    head = tab_head(p);
    lv_obj_t *spacer = lv_obj_create(head);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_add_event_cb(md_button_create(head, "New playlist", MD_BUTTON_TEXT), new_playlist, LV_EVENT_CLICKED, NULL);
    make_list(p, &ui.playlists, 0);
    add_notice(p, &ui.playlists);

    build_mini(ui.root);
    lists_from_lib();
    lv_timer_create(tick, 250, NULL);
    tick(NULL);
    T5_IGNORE(t5_event_subscribe(T5_EV_AUDIO_ROUTE, route_changed, NULL));   /* without it: no auto pause */
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
    if (ui.detail) page_fit(ui.detail->page);
    if (ui.queue_view) page_fit(ui.queue_view->page);
    if (ui.np.page) {                                   /* lay it out again for the new shape */
        lv_obj_delete(ui.np.page);
        np_build();
    }
}

T5_APP(.on_create = on_create, .on_suspend = on_suspend, .on_destroy = on_destroy, .on_rotate = on_rotate);
