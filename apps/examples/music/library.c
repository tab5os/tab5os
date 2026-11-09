/* The music library (library.h). */
#include "library.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "m3u.h"

#define MAX_DEPTH 8
#define CACHE_MAGIC "MLIB2\n"
#define PL_MAX_BYTES (512 * 1024)

/* ---- arena: the strings and per-group arrays, freed together ---------------------- */
typedef struct blk {
    struct blk *next;
    size_t used, cap;
    char data[];
} blk_t;

static void *ar_alloc(lib_t *l, size_t n)
{
    n = (n + 7) & ~(size_t)7;
    blk_t *b = l->arena;
    if (!b || b->used + n > b->cap) {
        size_t cap = n > 32768 ? n : 32768;
        blk_t *nb = malloc(sizeof *nb + cap);
        if (!nb) return NULL;
        nb->next = b;
        nb->used = 0;
        nb->cap = cap;
        l->arena = b = nb;
    }
    void *p = b->data + b->used;
    b->used += n;
    return p;
}

static const char *ar_str(lib_t *l, const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = ar_alloc(l, n);
    if (!p) return "";
    memcpy(p, s, n);
    return p;
}

static lib_t *lib_new(void)
{
    return calloc(1, sizeof(lib_t));
}

void lib_free(lib_t *l)
{
    if (!l) return;
    for (blk_t *b = l->arena; b;) {
        blk_t *nx = b->next;
        free(b);
        b = nx;
    }
    free(l->t);
    free(l->by_title);
    free(l->albums);
    free(l->artists);
    free(l->pl);
    free(l->hash);
    free(l);
}

/* ---- helpers ---------------------------------------------------------------------- */
static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static int ncasecmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char x = lower(a[i]), y = lower(b[i]);
        if (x != y || !x) return (unsigned char)x - (unsigned char)y;
    }
    return 0;
}

static bool ext_is(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e && !ncasecmp(name + n - e, ext, e);
}

static size_t dir_len(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? (size_t)(s - path) : 0;
}

static uint32_t hash_str(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

/* Merge sort with a context (qsort has none, and two of these can run at once). */
typedef int (*cmp_fn)(const lib_t *l, int a, int b);

static void msort(int *a, int *tmp, int n, cmp_fn cmp, const lib_t *l)
{
    if (n < 2) return;
    int h = n / 2;
    msort(a, tmp, h, cmp, l);
    msort(a + h, tmp, n - h, cmp, l);
    int i = 0, j = h, k = 0;
    while (i < h && j < n) tmp[k++] = cmp(l, a[j], a[i]) < 0 ? a[j++] : a[i++];
    while (i < h) tmp[k++] = a[i++];
    while (j < n) tmp[k++] = a[j++];
    memcpy(a, tmp, (size_t)n * sizeof *a);
}

static bool sort_ix(int *a, int n, cmp_fn cmp, const lib_t *l)
{
    int *tmp = malloc((size_t)(n ? n : 1) * sizeof *tmp);
    if (!tmp) return false;
    msort(a, tmp, n, cmp, l);
    free(tmp);
    return true;
}

static const char *or_unknown(const char *s, const char *dflt)
{
    return s && s[0] ? s : dflt;
}

/* ---- index building ------------------------------------------------------------- */
static int cmp_title(const lib_t *l, int a, int b)
{
    int c = tag_casecmp(l->t[a].title, l->t[b].title);
    return c ? c : tag_casecmp(l->t[a].artist, l->t[b].artist);
}

static int cmp_album_key(const lib_t *l, int a, int b)
{
    const lib_track_t *x = &l->t[a], *y = &l->t[b];
    int c = tag_casecmp(or_unknown(x->album, ""), or_unknown(y->album, ""));
    if (c) return c;
    size_t dx = dir_len(x->path), dy = dir_len(y->path);
    c = ncasecmp(x->path, y->path, dx < dy ? dx : dy);
    return c ? c : (int)dx - (int)dy;
}

static int cmp_in_album(const lib_t *l, int a, int b)
{
    const lib_track_t *x = &l->t[a], *y = &l->t[b];
    if (x->disc != y->disc) return (int)x->disc - (int)y->disc;
    if (x->track != y->track) return (int)x->track - (int)y->track;
    return tag_casecmp(x->title, y->title);
}

static int cmp_album_name(const lib_t *l, int a, int b)
{
    int c = tag_casecmp(l->albums[a].name, l->albums[b].name);
    return c ? c : tag_casecmp(l->albums[a].artist, l->albums[b].artist);
}

static int cmp_artist(const lib_t *l, int a, int b)
{
    const lib_track_t *x = &l->t[a], *y = &l->t[b];
    int c = tag_casecmp(or_unknown(x->artist, "Unknown artist"), or_unknown(y->artist, "Unknown artist"));
    if (c) return c;
    if (x->album_ix != y->album_ix) return tag_casecmp(l->albums[x->album_ix].name, l->albums[y->album_ix].name);
    return cmp_in_album(l, a, b);
}

static void build_hash(lib_t *l)
{
    l->hcap = 16;
    while (l->hcap < l->n * 2) l->hcap *= 2;
    l->hash = malloc((size_t)l->hcap * sizeof *l->hash);
    if (!l->hash) {
        l->hcap = 0;
        return;
    }
    for (int i = 0; i < l->hcap; i++) l->hash[i] = -1;
    for (int i = 0; i < l->n; i++) {
        uint32_t h = hash_str(l->t[i].path) & (uint32_t)(l->hcap - 1);
        while (l->hash[h] >= 0) h = (h + 1) & (uint32_t)(l->hcap - 1);
        l->hash[h] = i;
    }
}

static bool build_index(lib_t *l)
{
    int n = l->n;
    build_hash(l);
    l->by_title = malloc((size_t)(n ? n : 1) * sizeof *l->by_title);
    int *ix = malloc((size_t)(n ? n : 1) * sizeof *ix);
    if (!l->by_title || !ix) {
        free(ix);
        return false;
    }
    for (int i = 0; i < n; i++) l->by_title[i] = ix[i] = i;
    sort_ix(l->by_title, n, cmp_title, l);

    /* albums: runs of the same album name in the same folder */
    sort_ix(ix, n, cmp_album_key, l);
    l->albums = calloc((size_t)(n ? n : 1), sizeof *l->albums);
    if (!l->albums) {
        free(ix);
        return false;
    }
    l->nalbums = 0;
    for (int i = 0; i < n;) {
        int j = i + 1;
        while (j < n && !cmp_album_key(l, ix[i], ix[j])) j++;
        lib_album_t *a = &l->albums[l->nalbums];
        a->n = j - i;
        a->tracks = ar_alloc(l, (size_t)a->n * sizeof(int));
        if (!a->tracks) break;
        memcpy(a->tracks, ix + i, (size_t)a->n * sizeof(int));
        sort_ix(a->tracks, a->n, cmp_in_album, l);
        const lib_track_t *f = &l->t[a->tracks[0]];
        a->name = or_unknown(f->album, "Unknown album");
        a->artist = NULL;
        bool same = true;
        a->cover = a->tracks[0];
        bool cover_found = false;
        for (int k = 0; k < a->n; k++) {
            const lib_track_t *t = &l->t[a->tracks[k]];
            if (!a->artist && t->album_artist[0]) a->artist = t->album_artist;
            if (tag_casecmp(t->artist, f->artist)) same = false;
            if (t->year > a->year) a->year = t->year;
            if (!cover_found && t->art_len) a->cover = a->tracks[k], cover_found = true;
        }
        if (!a->artist) a->artist = same ? or_unknown(f->artist, "Unknown artist") : "Various artists";
        for (int k = 0; k < a->n; k++) l->t[a->tracks[k]].album_ix = l->nalbums;
        l->nalbums++;
        i = j;
    }
    /* albums by name: renumber the tracks' album_ix */
    int *order = malloc((size_t)(l->nalbums ? l->nalbums : 1) * sizeof *order);
    lib_album_t *sorted = malloc((size_t)(l->nalbums ? l->nalbums : 1) * sizeof *sorted);
    if (order && sorted) {
        for (int i = 0; i < l->nalbums; i++) order[i] = i;
        sort_ix(order, l->nalbums, cmp_album_name, l);
        for (int i = 0; i < l->nalbums; i++) {
            sorted[i] = l->albums[order[i]];
            for (int k = 0; k < sorted[i].n; k++) l->t[sorted[i].tracks[k]].album_ix = i;
        }
        memcpy(l->albums, sorted, (size_t)l->nalbums * sizeof *sorted);
    }
    free(order);
    free(sorted);

    /* artists: runs of the same artist */
    for (int i = 0; i < n; i++) ix[i] = i;
    sort_ix(ix, n, cmp_artist, l);
    l->artists = calloc((size_t)(n ? n : 1), sizeof *l->artists);
    l->nartists = 0;
    for (int i = 0; l->artists && i < n;) {
        const char *name = or_unknown(l->t[ix[i]].artist, "Unknown artist");
        int j = i + 1;
        while (j < n && !tag_casecmp(or_unknown(l->t[ix[j]].artist, "Unknown artist"), name)) j++;
        lib_artist_t *a = &l->artists[l->nartists];
        a->name = name;
        a->n = j - i;
        a->tracks = ar_alloc(l, (size_t)a->n * sizeof(int));
        if (!a->tracks) break;
        memcpy(a->tracks, ix + i, (size_t)a->n * sizeof(int));
        a->nalbums = 0;
        for (int k = 0; k < a->n; k++) {
            if (k == 0 || l->t[a->tracks[k]].album_ix != l->t[a->tracks[k - 1]].album_ix) a->nalbums++;
        }
        for (int k = 0; k < a->n; k++) l->t[a->tracks[k]].artist_ix = l->nartists;
        l->nartists++;
        i = j;
    }
    free(ix);
    return true;
}

/* ---- lookup ------------------------------------------------------------------------ */
int lib_find(const lib_t *l, const char *path)
{
    if (!l || !path) return -1;
    if (l->hcap) {
        uint32_t h = hash_str(path) & (uint32_t)(l->hcap - 1);
        while (l->hash[h] >= 0) {
            if (!strcmp(l->t[l->hash[h]].path, path)) return l->hash[h];
            h = (h + 1) & (uint32_t)(l->hcap - 1);
        }
    }
    for (int i = 0; i < l->n; i++) {
        if (!tag_casecmp(l->t[i].path, path)) return i;
    }
    /* by the last three parts: "Artist/Album/01 x.mp3" from a computer's playlist */
    const char *tail = path + strlen(path);
    for (int parts = 0; tail > path && parts < 3; tail--) {
        if (tail[-1] == '/' && ++parts == 3) break;
    }
    size_t tl = strlen(tail);
    for (int i = 0; tl && i < l->n; i++) {
        size_t pl = strlen(l->t[i].path);
        if (pl >= tl && !ncasecmp(l->t[i].path + pl - tl, tail, tl) && (pl == tl || l->t[i].path[pl - tl - 1] == '/')) {
            return i;
        }
    }
    /* the same song in another format: a playlist made for the .m4a or .mp3
     * files a library was converted from */
    const char *dot = strrchr(tail, '.');
    size_t sl = dot && !strchr(dot, '/') ? (size_t)(dot - tail) : tl;
    for (int i = 0; sl && i < l->n; i++) {
        const char *p = l->t[i].path, *pd = strrchr(p, '.');
        size_t ps = pd && !strchr(pd, '/') ? (size_t)(pd - p) : strlen(p);
        if (ps >= sl && !ncasecmp(p + ps - sl, tail, sl) && (ps == sl || p[ps - sl - 1] == '/')) return i;
    }
    return -1;
}

/* ---- scanning ---------------------------------------------------------------------- */
typedef struct {
    lib_t *l;
    const lib_t *prev;
    lib_progress_fn progress;
    void *user;
    int files, cap, plcap;
} scan_t;

static size_t f_read(void *ctx, uint32_t off, void *buf, size_t n)
{
    FILE *f = ctx;
    if (fseek(f, (long)off, SEEK_SET)) return 0;
    return fread(buf, 1, n, f);
}

static void add_track(scan_t *sc, const char *path, const struct stat *st)
{
    lib_t *l = sc->l;
    if (l->n >= LIB_MAX_TRACKS) return;
    if (l->n == sc->cap) {
        int cap = sc->cap ? sc->cap * 2 : 256;
        lib_track_t *t = realloc(l->t, (size_t)cap * sizeof *t);
        if (!t) return;
        l->t = t;
        sc->cap = cap;
    }
    lib_track_t *t = &l->t[l->n];
    memset(t, 0, sizeof *t);
    uint32_t size = (uint32_t)st->st_size, mtime = (uint32_t)st->st_mtime;
    int p = sc->prev ? lib_find(sc->prev, path) : -1;
    if (p >= 0 && !strcmp(sc->prev->t[p].path, path) && sc->prev->t[p].size == size && sc->prev->t[p].mtime == mtime) {
        const lib_track_t *o = &sc->prev->t[p];
        *t = *o;
        t->path = ar_str(l, o->path);
        t->title = ar_str(l, o->title);
        t->artist = ar_str(l, o->artist);
        t->album = ar_str(l, o->album);
        t->album_artist = ar_str(l, o->album_artist);
        t->genre = ar_str(l, o->genre);
    } else {
        FILE *f = fopen(path, "rb");
        if (!f) return;
        tag_src_t src = { f_read, f, size };
        tags_t tg;
        bool ok = tags_read(&src, &tg);
        fclose(f);
        if (!ok) return;
        if (!tg.title[0]) {
            const char *b = strrchr(path, '/');
            b = b ? b + 1 : path;
            const char *dot = strrchr(b, '.');
            tag_copy(tg.title, sizeof tg.title, b, dot ? (size_t)(dot - b) : strlen(b));
        }
        t->path = ar_str(l, path);
        t->title = ar_str(l, tg.title);
        t->artist = ar_str(l, tg.artist);
        t->album = ar_str(l, tg.album);
        t->album_artist = ar_str(l, tg.album_artist);
        t->genre = ar_str(l, tg.genre);
        t->duration_ms = tg.duration_ms;
        t->art_off = tg.art_off;
        t->art_len = tg.art_len;
        t->track = tg.track;
        t->disc = tg.disc;
        t->year = tg.year;
        t->fmt = (uint8_t)tg.fmt;
    }
    t->size = size;
    t->mtime = mtime;
    l->n++;
}

/* Entries in a playlist: its lines that aren't blank or comments. */
static int count_entries(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char line[M3U_PATH_MAX];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t' || (unsigned char)*p == 0xef || (unsigned char)*p == 0xbb || (unsigned char)*p == 0xbf) p++;
        if (*p && *p != '#' && *p != '\r' && *p != '\n') n++;
    }
    fclose(f);
    return n;
}

static void add_playlist(lib_t *l, int *cap, const char *path, const char *name)
{
    if (l->npl == *cap) {
        int c = *cap ? *cap * 2 : 16;
        lib_playlist_t *p = realloc(l->pl, (size_t)c * sizeof *p);
        if (!p) return;
        l->pl = p;
        *cap = c;
    }
    char n[128];
    const char *dot = strrchr(name, '.');
    tag_copy(n, sizeof n, name, dot ? (size_t)(dot - name) : strlen(name));
    l->pl[l->npl].path = ar_str(l, path);
    l->pl[l->npl].name = ar_str(l, n);
    l->pl[l->npl].n = count_entries(path);
    l->npl++;
}

static void walk(scan_t *sc, char *path, size_t len, int depth)
{
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *name = e->d_name;
        if (name[0] == '.') continue;                       /* hidden, and macOS "._" files */
        size_t nl = strlen(name);
        if (len + 1 + nl >= M3U_PATH_MAX) continue;
        path[len] = '/';
        memcpy(path + len + 1, name, nl + 1);
        struct stat st;
        bool is_dir;
#ifdef DT_DIR
        if (e->d_type == DT_DIR) is_dir = true;
        else if (e->d_type == DT_REG) is_dir = false;
        else
#endif
            is_dir = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
        if (is_dir) {
            if (depth < MAX_DEPTH) walk(sc, path, len + 1 + nl, depth + 1);
        } else if (ext_is(name, ".mp3") || ext_is(name, ".flac") || ext_is(name, ".wav")) {
            if (stat(path, &st) == 0) add_track(sc, path, &st);
            if (++sc->files % 8 == 0 && sc->progress) sc->progress(sc->files, sc->user);
        } else if (ext_is(name, ".m3u") || ext_is(name, ".m3u8")) {
            add_playlist(sc->l, &sc->plcap, path, name);
        }
        path[len] = 0;
    }
    closedir(d);
}

static int cmp_pl(const lib_t *l, int a, int b)
{
    return tag_casecmp(l->pl[a].name, l->pl[b].name);
}

static void sort_playlists(lib_t *l)
{
    int *ix = malloc((size_t)(l->npl ? l->npl : 1) * sizeof *ix);
    lib_playlist_t *s = malloc((size_t)(l->npl ? l->npl : 1) * sizeof *s);
    if (ix && s) {
        for (int i = 0; i < l->npl; i++) ix[i] = i;
        sort_ix(ix, l->npl, cmp_pl, l);
        for (int i = 0; i < l->npl; i++) s[i] = l->pl[ix[i]];
        memcpy(l->pl, s, (size_t)l->npl * sizeof *s);
    }
    free(ix);
    free(s);
}

lib_t *lib_scan(const lib_t *prev, lib_progress_fn progress, void *user)
{
    lib_t *l = lib_new();
    if (!l) return NULL;
    scan_t sc = { .l = l, .prev = prev, .progress = progress, .user = user };
    char path[M3U_PATH_MAX] = MUSIC_DIR;
    walk(&sc, path, strlen(path), 0);
    sort_playlists(l);
    if (!build_index(l)) {
        lib_free(l);
        return NULL;
    }
    return l;
}

void lib_rescan_playlists(lib_t *l)
{
    int keep = 0;
    for (int i = 0; i < l->npl; i++) {                    /* the ones elsewhere stay */
        if (ncasecmp(l->pl[i].path, PLAYLIST_DIR "/", strlen(PLAYLIST_DIR) + 1)) l->pl[keep++] = l->pl[i];
    }
    l->npl = keep;
    int cap = keep;
    DIR *d = opendir(PLAYLIST_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.' || !(ext_is(e->d_name, ".m3u") || ext_is(e->d_name, ".m3u8"))) continue;
            char p[M3U_PATH_MAX];
            size_t dl = sizeof PLAYLIST_DIR - 1, nl = strlen(e->d_name);
            if (dl + 1 + nl >= sizeof p) continue;          /* too long a name to be one of ours */
            memcpy(p, PLAYLIST_DIR "/", dl + 1);
            memcpy(p + dl + 1, e->d_name, nl + 1);
            add_playlist(l, &cap, p, e->d_name);
        }
        closedir(d);
    }
    sort_playlists(l);
}

/* ---- the cache ------------------------------------------------------------------- */
static void put_u32(FILE *f, uint32_t v)
{
    fwrite(&v, 4, 1, f);
}

static void put_str(FILE *f, const char *s)
{
    fwrite(s, 1, strlen(s) + 1, f);
}

bool lib_save_cache(const lib_t *l, const char *file)
{
    char tmp[160];
    snprintf(tmp, sizeof tmp, "%s.new", file);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    fwrite(CACHE_MAGIC, 1, sizeof CACHE_MAGIC - 1, f);
    put_u32(f, (uint32_t)l->n);
    for (int i = 0; i < l->n; i++) {
        const lib_track_t *t = &l->t[i];
        uint32_t v[8] = { t->size, t->mtime, t->duration_ms, t->art_off, t->art_len,
                          (uint32_t)t->track << 16 | t->disc, t->year, t->fmt };
        fwrite(v, sizeof v, 1, f);
        put_str(f, t->path);
        put_str(f, t->title);
        put_str(f, t->artist);
        put_str(f, t->album);
        put_str(f, t->album_artist);
        put_str(f, t->genre);
    }
    for (int i = 0; i < l->npl; i++) {
        put_str(f, l->pl[i].path);
        put_str(f, l->pl[i].name);
        put_u32(f, (uint32_t)l->pl[i].n);
    }
    bool ok = !ferror(f);
    ok &= fclose(f) == 0;
    if (ok) {
        remove(file);
        ok = rename(tmp, file) == 0;
    }
    if (!ok) remove(tmp);
    return ok;
}

static const char *take_str(const char **p, const char *end)
{
    const char *s = *p;
    const char *z = memchr(s, 0, (size_t)(end - s));
    if (!z) return NULL;
    *p = z + 1;
    return s;
}

lib_t *lib_load_cache(const char *file)
{
    FILE *f = fopen(file, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = len > 0 && len < 8 * 1024 * 1024 ? malloc((size_t)len) : NULL;
    bool ok = buf && fread(buf, 1, (size_t)len, f) == (size_t)len;
    fclose(f);
    lib_t *l = ok ? lib_new() : NULL;
    if (!l || memcmp(buf, CACHE_MAGIC, sizeof CACHE_MAGIC - 1)) {
        free(buf);
        lib_free(l);
        return NULL;
    }
    const char *p = buf + sizeof CACHE_MAGIC - 1, *end = buf + len;
    uint32_t n;
    memcpy(&n, p, 4);
    p += 4;
    if (n > LIB_MAX_TRACKS || !(l->t = calloc(n ? n : 1, sizeof *l->t))) goto bad;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v[8];
        if (end - p < (long)sizeof v) goto bad;
        memcpy(v, p, sizeof v);
        p += sizeof v;
        lib_track_t *t = &l->t[i];
        const char *s[6];
        for (int k = 0; k < 6; k++) {
            if (!(s[k] = take_str(&p, end))) goto bad;
        }
        t->path = ar_str(l, s[0]);
        t->title = ar_str(l, s[1]);
        t->artist = ar_str(l, s[2]);
        t->album = ar_str(l, s[3]);
        t->album_artist = ar_str(l, s[4]);
        t->genre = ar_str(l, s[5]);
        t->size = v[0];
        t->mtime = v[1];
        t->duration_ms = v[2];
        t->art_off = v[3];
        t->art_len = v[4];
        t->track = (uint16_t)(v[5] >> 16);
        t->disc = (uint16_t)v[5];
        t->year = (uint16_t)v[6];
        t->fmt = (uint8_t)v[7];
        l->n++;
    }
    int cap = 0;
    while (p < end) {
        const char *path = take_str(&p, end), *name = path ? take_str(&p, end) : NULL;
        if (!name || end - p < 4) break;
        uint32_t count;
        memcpy(&count, p, 4);
        p += 4;
        if (l->npl == cap) {
            int c = cap ? cap * 2 : 16;
            lib_playlist_t *q = realloc(l->pl, (size_t)c * sizeof *q);
            if (!q) break;
            l->pl = q;
            cap = c;
        }
        l->pl[l->npl].path = ar_str(l, path);
        l->pl[l->npl].name = ar_str(l, name);
        l->pl[l->npl].n = (int)count;
        l->npl++;
    }
    free(buf);
    if (!build_index(l)) {
        lib_free(l);
        return NULL;
    }
    return l;
bad:
    free(buf);
    lib_free(l);
    return NULL;
}

/* ---- playlists ----------------------------------------------------------------------- */
typedef struct {
    const lib_t *l;
    int *out;
    int n, cap, missing;
} pl_read_t;

static void pl_entry(const char *path, const char *title, void *user)
{
    (void)title;
    pl_read_t *r = user;
    int i = lib_find(r->l, path);
    if (i < 0) {
        r->missing++;
        return;
    }
    if (r->n == r->cap) {
        int c = r->cap ? r->cap * 2 : 64;
        int *o = realloc(r->out, (size_t)c * sizeof *o);
        if (!o) return;
        r->out = o;
        r->cap = c;
    }
    r->out[r->n++] = i;
}

int *lib_playlist_tracks(const lib_t *l, const char *path, int *n, int *missing)
{
    *n = 0;
    if (missing) *missing = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || len > PL_MAX_BYTES) len = len < 0 ? 0 : PL_MAX_BYTES;
    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    char dir[M3U_PATH_MAX];
    snprintf(dir, sizeof dir, "%.*s", (int)dir_len(path), path);
    pl_read_t r = { .l = l };
    m3u_parse(buf, got, dir, pl_entry, &r);
    free(buf);
    *n = r.n;
    if (missing) *missing = r.missing;
    return r.out ? r.out : calloc(1, sizeof(int));
}

bool lib_playlist_write(const lib_t *l, const char *path, const int *tracks, int n)
{
    mkdir(MUSIC_DIR, 0775);
    mkdir(PLAYLIST_DIR, 0775);
    char tmp[M3U_PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    char dir[M3U_PATH_MAX];
    snprintf(dir, sizeof dir, "%.*s", (int)dir_len(path), path);
    fputs("#EXTM3U\n", f);
    for (int i = 0; i < n; i++) {
        const lib_track_t *t = &l->t[tracks[i]];
        char rel[M3U_PATH_MAX];
        m3u_relative(rel, sizeof rel, dir, t->path);
        fprintf(f, "#EXTINF:%u,%s%s%s\n%s\n", (unsigned)(t->duration_ms / 1000), t->artist, t->artist[0] ? " - " : "",
                t->title, rel);
    }
    bool ok = !ferror(f);
    ok &= fclose(f) == 0;
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) remove(tmp);
    return ok;
}

void lib_fmt_time(char *out, size_t cap, uint32_t ms)
{
    uint32_t s = ms / 1000;
    if (s >= 3600) snprintf(out, cap, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    else snprintf(out, cap, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}
