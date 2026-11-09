/* Track metadata (tags.h). */
#include "tags.h"
#include <stdlib.h>
#include <string.h>

#define TEXT_MAX 512            /* a text frame's body we look at */

/* ---- helpers ----------------------------------------------------------------- */
static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t be24(const uint8_t *p)
{
    return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t syncsafe(const uint8_t *p)
{
    return (uint32_t)(p[0] & 0x7f) << 21 | (uint32_t)(p[1] & 0x7f) << 14 | (uint32_t)(p[2] & 0x7f) << 7 | (p[3] & 0x7f);
}

static size_t rd(const tag_src_t *s, uint32_t off, void *buf, size_t n)
{
    if (off >= s->size) return 0;
    if (n > s->size - off) n = s->size - off;
    return s->read(s->ctx, off, buf, n);
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

int tag_casecmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = lower(*a), y = lower(*b);
        if (x != y || !x) return (unsigned char)x - (unsigned char)y;
    }
}

static bool casepfx(const char *s, const char *pfx, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (lower(s[i]) != lower(pfx[i])) return false;
    }
    return true;
}

void tag_copy(char *dst, size_t cap, const char *src, size_t len)
{
    if (!cap) return;
    size_t n = strnlen(src, len);
    /* trim spaces at both ends */
    while (n && (*src == ' ' || *src == '\t')) src++, n--;
    while (n && (src[n - 1] == ' ' || src[n - 1] == '\t' || src[n - 1] == '\r' || src[n - 1] == '\n')) n--;
    if (n >= cap) {
        n = cap - 1;
        while (n && ((unsigned char)src[n] & 0xc0) == 0x80) n--;   /* not inside a character */
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

static size_t put_utf8(char *o, size_t cap, size_t at, uint32_t cp)
{
    char b[4];
    size_t n;
    if (cp < 0x80) b[0] = (char)cp, n = 1;
    else if (cp < 0x800) b[0] = (char)(0xc0 | cp >> 6), b[1] = (char)(0x80 | (cp & 0x3f)), n = 2;
    else if (cp < 0x10000) {
        b[0] = (char)(0xe0 | cp >> 12), b[1] = (char)(0x80 | ((cp >> 6) & 0x3f)), b[2] = (char)(0x80 | (cp & 0x3f));
        n = 3;
    } else {
        b[0] = (char)(0xf0 | cp >> 18), b[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3f)), b[3] = (char)(0x80 | (cp & 0x3f));
        n = 4;
    }
    if (at + n >= cap) return at;
    memcpy(o + at, b, n);
    return at + n;
}

/* An ID3 text body (encoding byte first) to UTF-8; stops at the first NUL
 * (ID3v2.4 lists several values that way: we keep the first). */
static void id3_text(const uint8_t *p, size_t n, char *out, size_t cap)
{
    char tmp[TAG_TEXT * 2];
    size_t o = 0;
    if (!n) {
        out[0] = 0;
        return;
    }
    uint8_t enc = p[0];
    p++, n--;
    if (enc == 0) {                                     /* Latin-1 */
        for (size_t i = 0; i < n && p[i]; i++) o = put_utf8(tmp, sizeof tmp, o, p[i]);
    } else if (enc == 3) {                              /* UTF-8 */
        for (size_t i = 0; i < n && p[i] && o + 1 < sizeof tmp; i++) tmp[o++] = (char)p[i];
    } else {                                            /* UTF-16, with BOM (1) or BE (2) */
        bool be = enc == 2;
        size_t i = 0;
        if (enc == 1 && n >= 2) {
            if (p[0] == 0xff && p[1] == 0xfe) be = false, i = 2;
            else if (p[0] == 0xfe && p[1] == 0xff) be = true, i = 2;
        }
        for (; i + 1 < n; i += 2) {
            uint32_t u = be ? (uint32_t)(p[i] << 8 | p[i + 1]) : (uint32_t)(p[i] | p[i + 1] << 8);
            if (!u) break;
            if (u >= 0xd800 && u < 0xdc00 && i + 3 < n) {   /* surrogate pair */
                uint32_t l = be ? (uint32_t)(p[i + 2] << 8 | p[i + 3]) : (uint32_t)(p[i + 2] | p[i + 3] << 8);
                if (l >= 0xdc00 && l < 0xe000) {
                    u = 0x10000 + ((u - 0xd800) << 10) + (l - 0xdc00);
                    i += 2;
                }
            }
            o = put_utf8(tmp, sizeof tmp, o, u);
        }
    }
    tmp[o] = 0;
    tag_copy(out, cap, tmp, o);
}

/* "3/12" -> 3 */
static uint16_t num(const char *s)
{
    unsigned v = 0;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9' && v < 10000) v = v * 10 + (unsigned)(*s++ - '0');
    return (uint16_t)v;
}

/* "(17)" or "(17)Rock" from ID3: the text after, else the number as it is. */
static void genre_clean(char *g)
{
    if (g[0] != '(') return;
    char *e = strchr(g, ')');
    if (e && e[1]) memmove(g, e + 1, strlen(e + 1) + 1);
}

/* Undo ID3 unsynchronisation (FF 00 -> FF) in place; returns the new length. */
static size_t unsync(uint8_t *p, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        p[o++] = p[i];
        if (p[i] == 0xff && i + 1 < n && p[i + 1] == 0x00) i++;
    }
    return o;
}

/* ---- ID3v2 --------------------------------------------------------------------- */
typedef struct {
    bool front;                 /* the art we have is a front cover */
} art_pick_t;

/* An APIC/PIC body at file offset off (n bytes): the picture data's place if
 * it's a JPEG we'd rather have than what we have. */
static void id3_picture(const tag_src_t *s, uint32_t off, uint32_t n, int ver, tags_t *t, art_pick_t *pick)
{
    uint8_t h[160];
    size_t got = rd(s, off, h, n < sizeof h ? n : sizeof h);
    if (got < 4) return;
    uint8_t enc = h[0];
    size_t i = 1;
    bool jpeg_mime;
    if (ver == 2) {                                     /* 3-char format */
        jpeg_mime = casepfx((const char *)h + 1, "JPG", 3);
        i = 4;
    } else {
        size_t m = i;
        while (m < got && h[m]) m++;
        jpeg_mime = (m - i >= 4 && (casepfx((const char *)h + i, "image/jp", 8) || casepfx((const char *)h + i, "jpg", 3) ||
                                   casepfx((const char *)h + i, "jpeg", 4)));
        i = m + 1;
    }
    if (i >= got) return;
    uint8_t type = h[i++];
    /* description, terminated by one NUL (Latin-1, UTF-8) or two (UTF-16) */
    if (enc == 1 || enc == 2) {
        while (i + 1 < got && (h[i] || h[i + 1])) i += 2;
        i += 2;
    } else {
        while (i < got && h[i]) i++;
        i++;
    }
    if (i + 2 > got) return;                            /* description longer than we read: skip */
    bool jpeg = h[i] == 0xff && h[i + 1] == 0xd8;
    if (!jpeg && !jpeg_mime) return;
    if (!jpeg) return;                                  /* says JPEG, isn't */
    bool front = type == 3;
    if (t->art_len && (pick->front || !front)) return;   /* keep the one we have */
    t->art_off = off + (uint32_t)i;
    t->art_len = n - (uint32_t)i;
    pick->front = front;
}

/* Parse the ID3v2 tag at base. Returns its total length (0 if none). */
static uint32_t id3v2(const tag_src_t *s, uint32_t base, tags_t *t)
{
    uint8_t h[10];
    if (rd(s, base, h, 10) < 10 || memcmp(h, "ID3", 3) || h[3] < 2 || h[3] > 4) return 0;
    int ver = h[3];
    uint8_t flags = h[5];
    uint32_t size = syncsafe(h + 6);
    uint32_t total = 10 + size + ((flags & 0x10) ? 10 : 0);
    bool tag_unsync = (flags & 0x80) && ver < 4;
    uint32_t at = base + 10, end = base + 10 + size;
    if (end > s->size) end = s->size;
    if ((flags & 0x40) && ver >= 3) {                   /* extended header */
        uint8_t e[4];
        if (rd(s, at, e, 4) < 4) return total;
        at += ver == 4 ? syncsafe(e) : 4 + be32(e);
    }
    art_pick_t pick = { 0 };
    uint8_t *body = malloc(TEXT_MAX);
    if (!body) return total;
    const int hl = ver == 2 ? 6 : 10;
    while (at + (uint32_t)hl <= end) {
        uint8_t f[10];
        if (rd(s, at, f, (size_t)hl) < (size_t)hl || !f[0]) break;     /* padding */
        char id[5] = { 0 };
        uint32_t n;
        uint16_t ff = 0;
        if (ver == 2) {
            memcpy(id, f, 3);
            n = be24(f + 3);
        } else {
            memcpy(id, f, 4);
            n = ver == 4 ? syncsafe(f + 4) : be32(f + 4);
            ff = (uint16_t)(f[8] << 8 | f[9]);
        }
        uint32_t data = at + (uint32_t)hl;
        if (n > end - data) break;
        at = data + n;
        if (ver == 4 && (ff & 0x0001)) data += 4, n = n >= 4 ? n - 4 : 0;   /* data length indicator */
        bool fr_unsync = tag_unsync || (ver == 4 && (ff & 0x0002));
        if (ver >= 3 && (ff & (ver == 4 ? 0x000c : 0x00c0))) continue;     /* compressed or encrypted */
        if (!strcmp(id, "APIC") || !strcmp(id, "PIC")) {
            if (!fr_unsync) id3_picture(s, data, n, ver, t, &pick);       /* unsynchronised art: skip */
            continue;
        }
        if (id[0] != 'T') continue;
        size_t len = rd(s, data, body, n < TEXT_MAX ? n : TEXT_MAX);
        if (fr_unsync) len = unsync(body, len);
        char v[TAG_TEXT];
        id3_text(body, len, v, sizeof v);
        if (!v[0]) continue;
        if (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) tag_copy(t->title, sizeof t->title, v, sizeof v);
        else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) tag_copy(t->artist, sizeof t->artist, v, sizeof v);
        else if (!strcmp(id, "TALB") || !strcmp(id, "TAL")) tag_copy(t->album, sizeof t->album, v, sizeof v);
        else if (!strcmp(id, "TPE2") || !strcmp(id, "TP2")) tag_copy(t->album_artist, sizeof t->album_artist, v, sizeof v);
        else if (!strcmp(id, "TCON") || !strcmp(id, "TCO")) {
            tag_copy(t->genre, sizeof t->genre, v, sizeof v);
            genre_clean(t->genre);
        } else if (!strcmp(id, "TRCK") || !strcmp(id, "TRK")) t->track = num(v);
        else if (!strcmp(id, "TPOS") || !strcmp(id, "TPA")) t->disc = num(v);
        else if (!strcmp(id, "TYER") || !strcmp(id, "TYE") || !strcmp(id, "TDRC") || !strcmp(id, "TDOR")) {
            if (!t->year) t->year = num(v);
        }
    }
    free(body);
    return total;
}

/* ID3v1 at the end: fills what ID3v2 left empty. Returns true if present. */
static bool id3v1(const tag_src_t *s, tags_t *t)
{
    uint8_t b[128];
    if (s->size < 128 || rd(s, s->size - 128, b, 128) < 128 || memcmp(b, "TAG", 3)) return false;
    char v[31 * 2];
    struct { char *dst; size_t cap; int at, len; } f[] = {
        { t->title, sizeof t->title, 3, 30 }, { t->artist, sizeof t->artist, 33, 30 }, { t->album, sizeof t->album, 63, 30 },
    };
    for (size_t i = 0; i < sizeof f / sizeof f[0]; i++) {
        if (f[i].dst[0]) continue;
        size_t o = 0;
        for (int k = 0; k < f[i].len && b[f[i].at + k]; k++) o = put_utf8(v, sizeof v, o, b[f[i].at + k]);
        v[o] = 0;
        tag_copy(f[i].dst, f[i].cap, v, o);
    }
    if (!t->year) {
        char y[5] = { (char)b[93], (char)b[94], (char)b[95], (char)b[96], 0 };
        t->year = num(y);
    }
    if (!t->track && b[125] == 0 && b[126]) t->track = b[126];       /* ID3v1.1 */
    return true;
}

/* ---- MP3 ------------------------------------------------------------------------- */
bool mp3_frame_parse(const uint8_t *p, mp3_frame_t *out)
{
    static const uint16_t br[2][3][15] = {
        { { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448 },     /* MPEG-1 L1 */
          { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 },        /* L2 */
          { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 } },       /* L3 */
        { { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256 },        /* MPEG-2/2.5 L1 */
          { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 },
          { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 } },
    };
    static const uint32_t sr[3] = { 44100, 48000, 32000 };
    if (p[0] != 0xff || (p[1] & 0xe0) != 0xe0) return false;
    int ver = (p[1] >> 3) & 3;                          /* 0: 2.5, 2: 2, 3: 1 */
    int layer = (p[1] >> 1) & 3;                        /* 1: III, 2: II, 3: I */
    int bri = p[2] >> 4, sri = (p[2] >> 2) & 3, pad = (p[2] >> 1) & 1;
    if (ver == 1 || layer == 0 || bri == 0 || bri == 15 || sri == 3) return false;
    int l = 3 - layer;                                  /* 0: I, 1: II, 2: III */
    uint32_t kbps = br[ver == 3 ? 0 : 1][l][bri];
    uint32_t rate = sr[sri] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
    uint16_t samples = l == 0 ? 384 : (l == 2 && ver != 3) ? 576 : 1152;
    uint32_t bytes = l == 0 ? (12 * kbps * 1000 / rate + (uint32_t)pad) * 4 : samples / 8 * kbps * 1000 / rate + (uint32_t)pad;
    if (bytes < 4) return false;
    out->rate = rate;
    out->samples = samples;
    out->bytes = (uint16_t)bytes;
    out->kbps = kbps;
    out->channels = ((p[3] >> 6) == 3) ? 1 : 2;
    out->mpeg = ver == 3 ? 1 : ver == 2 ? 2 : 25;
    return true;
}

/* The first frame at or after `from`, checked against the frame after it. */
static bool mp3_first_frame(const tag_src_t *s, uint32_t from, uint32_t *at, mp3_frame_t *fr)
{
    uint8_t *b = malloc(4096 + 4);
    if (!b) return false;
    bool found = false;
    for (uint32_t base = from; !found && base < from + 65536 && base < s->size; base += 4096) {
        size_t n = rd(s, base, b, 4096 + 4);
        for (size_t i = 0; i + 4 <= n && !found; i++) {
            if (b[i] != 0xff || !mp3_frame_parse(b + i, fr)) continue;
            uint8_t nx[4];
            uint32_t next = base + (uint32_t)i + fr->bytes;
            mp3_frame_t f2;
            if (next + 4 > s->size || (rd(s, next, nx, 4) == 4 && mp3_frame_parse(nx, &f2) && f2.rate == fr->rate)) {
                *at = base + (uint32_t)i;
                found = true;
            }
        }
    }
    free(b);
    return found;
}

static void mp3_info(const tag_src_t *s, tags_t *t)
{
    uint32_t at;
    mp3_frame_t fr;
    if (!mp3_first_frame(s, t->audio_off, &at, &fr)) return;
    t->rate = fr.rate;
    t->channels = fr.channels;
    uint8_t b[192];
    size_t n = rd(s, at, b, sizeof b);
    /* Xing/Info: after the side info */
    size_t side = fr.mpeg == 1 ? (fr.channels == 1 ? 17 : 32) : (fr.channels == 1 ? 9 : 17);
    size_t x = 4 + side;
    uint32_t frames = 0;
    if (x + 12 <= n && (!memcmp(b + x, "Xing", 4) || !memcmp(b + x, "Info", 4)) && (b[x + 7] & 1)) {
        frames = be32(b + x + 8);
    } else if (36 + 18 <= n && !memcmp(b + 36, "VBRI", 4)) {
        frames = be32(b + 36 + 14);
    }
    if (frames) {
        t->duration_ms = (uint32_t)((uint64_t)frames * fr.samples * 1000 / fr.rate);
    } else if (fr.kbps) {
        uint32_t bytes = t->audio_end > at ? t->audio_end - at : 0;
        t->duration_ms = (uint32_t)((uint64_t)bytes * 8 / fr.kbps);
    }
}

/* ---- FLAC ------------------------------------------------------------------------ */
static void vorbis_comment(const uint8_t *p, uint32_t n, tags_t *t)
{
    if (n < 8) return;
    uint32_t i = 4 + le32(p);                           /* vendor */
    if (i + 4 > n) return;
    uint32_t count = le32(p + i);
    i += 4;
    for (uint32_t c = 0; c < count && i + 4 <= n; c++) {
        uint32_t len = le32(p + i);
        i += 4;
        if (len > n - i) break;
        const char *kv = (const char *)p + i;
        i += len;
        const char *eq = memchr(kv, '=', len);
        if (!eq) continue;
        size_t kl = (size_t)(eq - kv), vl = len - kl - 1;
        const char *v = eq + 1;
        char tmp[TAG_TEXT];
        tag_copy(tmp, sizeof tmp, v, vl);
#define KEY(k) (kl == sizeof(k) - 1 && casepfx(kv, k, kl))
        if (KEY("TITLE") && !t->title[0]) strcpy(t->title, tmp);
        else if (KEY("ARTIST") && !t->artist[0]) strcpy(t->artist, tmp);
        else if (KEY("ALBUM") && !t->album[0]) strcpy(t->album, tmp);
        else if ((KEY("ALBUMARTIST") || KEY("ALBUM ARTIST")) && !t->album_artist[0]) strcpy(t->album_artist, tmp);
        else if (KEY("GENRE") && !t->genre[0]) tag_copy(t->genre, sizeof t->genre, tmp, sizeof tmp);
        else if (KEY("TRACKNUMBER") && !t->track) t->track = num(tmp);
        else if (KEY("DISCNUMBER") && !t->disc) t->disc = num(tmp);
        else if ((KEY("DATE") || KEY("YEAR")) && !t->year) t->year = num(tmp);
#undef KEY
    }
}

static void flac_picture(const tag_src_t *s, uint32_t off, uint32_t n, tags_t *t, art_pick_t *pick)
{
    uint8_t h[8];
    if (n < 32 || rd(s, off, h, 8) < 8) return;
    uint32_t type = be32(h), ml = be32(h + 4);
    if (ml > 64) return;
    char mime[65] = { 0 };
    if (rd(s, off + 8, mime, ml) < ml) return;
    uint32_t i = 8 + ml;
    uint8_t d[4];
    if (rd(s, off + i, d, 4) < 4) return;
    i += 4 + be32(d) + 16;                              /* description; width, height, depth, colours */
    if (rd(s, off + i, d, 4) < 4) return;
    uint32_t len = be32(d);
    i += 4;
    if (len > n - i) return;
    uint8_t sig[2];
    if (rd(s, off + i, sig, 2) < 2 || sig[0] != 0xff || sig[1] != 0xd8) return;   /* JPEG only */
    bool front = type == 3;
    if (t->art_len && (pick->front || !front)) return;
    t->art_off = off + i;
    t->art_len = len;
    pick->front = front;
}

static bool flac(const tag_src_t *s, uint32_t at, tags_t *t)
{
    uint8_t h[4];
    if (rd(s, at, h, 4) < 4 || memcmp(h, "fLaC", 4)) return false;
    at += 4;
    art_pick_t pick = { 0 };
    for (int blocks = 0; blocks < 64; blocks++) {
        uint8_t b[4];
        if (rd(s, at, b, 4) < 4) break;
        bool last = b[0] & 0x80;
        int type = b[0] & 0x7f;
        uint32_t n = be24(b + 1), body = at + 4;
        if (type == 0 && n >= 18) {                     /* STREAMINFO */
            uint8_t si[18];
            if (rd(s, body, si, 18) == 18) {
                t->rate = (uint32_t)si[10] << 12 | (uint32_t)si[11] << 4 | si[12] >> 4;
                t->channels = (uint8_t)(((si[12] >> 1) & 7) + 1);
                uint64_t total = (uint64_t)(si[13] & 0x0f) << 32 | be32(si + 14);
                if (t->rate) t->duration_ms = (uint32_t)(total * 1000 / t->rate);
            }
        } else if (type == 4 && n < 1u << 20) {          /* VORBIS_COMMENT */
            uint8_t *p = malloc(n);
            if (p) {
                if (rd(s, body, p, n) == n) vorbis_comment(p, n, t);
                free(p);
            }
        } else if (type == 6) {
            flac_picture(s, body, n, t, &pick);
        }
        at = body + n;
        if (last) break;
    }
    return true;
}

/* ---- WAV ------------------------------------------------------------------------- */
static bool wav(const tag_src_t *s, tags_t *t)
{
    uint8_t h[12];
    if (rd(s, 0, h, 12) < 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return false;
    uint32_t at = 12, block = 0, data_len = 0;
    for (int chunks = 0; chunks < 64 && at + 8 <= s->size; chunks++) {
        uint8_t c[8];
        if (rd(s, at, c, 8) < 8) break;
        uint32_t n = le32(c + 4), body = at + 8;
        if (!memcmp(c, "fmt ", 4) && n >= 16) {
            uint8_t f[16];
            if (rd(s, body, f, 16) == 16) {
                t->channels = (uint8_t)le16(f + 2);
                t->rate = le32(f + 4);
                block = le16(f + 12);
            }
        } else if (!memcmp(c, "data", 4)) {
            data_len = n > s->size - body ? s->size - body : n;
        } else if (!memcmp(c, "LIST", 4) && n >= 4 && n < 65536) {
            uint8_t *p = malloc(n);
            if (p && rd(s, body, p, n) == n && !memcmp(p, "INFO", 4)) {
                for (uint32_t i = 4; i + 8 <= n;) {
                    uint32_t l = le32(p + i + 4);
                    if (l > n - i - 8) break;
                    const char *v = (const char *)p + i + 8;
                    char tmp[TAG_TEXT];
                    tag_copy(tmp, sizeof tmp, v, l);
                    if (!memcmp(p + i, "INAM", 4) && !t->title[0]) strcpy(t->title, tmp);
                    else if (!memcmp(p + i, "IART", 4) && !t->artist[0]) strcpy(t->artist, tmp);
                    else if (!memcmp(p + i, "IPRD", 4) && !t->album[0]) strcpy(t->album, tmp);
                    else if (!memcmp(p + i, "IGNR", 4) && !t->genre[0]) tag_copy(t->genre, sizeof t->genre, tmp, sizeof tmp);
                    else if (!memcmp(p + i, "ICRD", 4) && !t->year) t->year = num(tmp);
                    else if ((!memcmp(p + i, "ITRK", 4) || !memcmp(p + i, "IPRT", 4)) && !t->track) t->track = num(tmp);
                    i += 8 + l + (l & 1);
                }
            }
            free(p);
        } else if (!memcmp(c, "id3 ", 4) || !memcmp(c, "ID3 ", 4)) {
            id3v2(s, body, t);
        }
        at = body + n + (n & 1);
    }
    if (block && t->rate) t->duration_ms = (uint32_t)((uint64_t)data_len / block * 1000 / t->rate);
    return true;
}

/* ---- entry ----------------------------------------------------------------------- */
bool tags_read(const tag_src_t *src, tags_t *out)
{
    memset(out, 0, sizeof *out);
    uint8_t h[4];
    if (rd(src, 0, h, 4) < 4) return false;
    if (!memcmp(h, "RIFF", 4)) {
        if (!wav(src, out)) return false;
        out->fmt = TRACK_WAV;
        return true;
    }
    uint32_t id3 = id3v2(src, 0, out);
    if (flac(src, id3, out)) {
        out->fmt = TRACK_FLAC;
        return true;
    }
    out->audio_off = id3;
    out->audio_end = src->size;
    if (id3v1(src, out)) out->audio_end = src->size - 128;
    mp3_info(src, out);
    if (!out->rate) return false;                       /* no MP3 frames */
    out->fmt = TRACK_MP3;
    return true;
}
