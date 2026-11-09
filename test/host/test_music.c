/* Music example: tag and playlist parsing (apps/examples/music/tags.c, m3u.c). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "library.h"
#include "m3u.h"
#include "tags.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- an in-memory file ------------------------------------------------------- */
static uint8_t g_buf[64 * 1024];
static size_t g_len;

static void put(const void *p, size_t n)
{
    TEST_ASSERT_TRUE(g_len + n <= sizeof g_buf);
    memcpy(g_buf + g_len, p, n);
    g_len += n;
}
static void put8(uint8_t v) { put(&v, 1); }
static void put_be32(uint32_t v) { uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; put(b, 4); }
static void put_be24(uint32_t v) { uint8_t b[3] = { (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; put(b, 3); }
static void put_le32(uint32_t v) { uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; put(b, 4); }
static void put_le16(uint16_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; put(b, 2); }
static void put_syncsafe(uint32_t v)
{
    uint8_t b[4] = { (uint8_t)((v >> 21) & 0x7f), (uint8_t)((v >> 14) & 0x7f), (uint8_t)((v >> 7) & 0x7f), (uint8_t)(v & 0x7f) };
    put(b, 4);
}

static size_t mem_read(void *ctx, uint32_t off, void *buf, size_t n)
{
    (void)ctx;
    memcpy(buf, g_buf + off, n);
    return n;
}

static bool parse(tags_t *t)
{
    tag_src_t s = { mem_read, NULL, (uint32_t)g_len };
    return tags_read(&s, t);
}

static const uint8_t JPEG[] = { 0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0, 1, 2, 3, 0xff, 0xd9 };

/* MPEG-1 layer III, 128 kb/s, 44.1 kHz, stereo: 417-byte frames */
static void mp3_frames(int n, uint32_t xing_frames)
{
    for (int i = 0; i < n; i++) {
        uint8_t f[417] = { 0xff, 0xfb, 0x90, 0x04 };
        if (i == 0 && xing_frames) {
            memcpy(f + 4 + 32, "Xing", 4);
            f[4 + 32 + 7] = 1;
            f[4 + 32 + 8] = (uint8_t)(xing_frames >> 24);
            f[4 + 32 + 9] = (uint8_t)(xing_frames >> 16);
            f[4 + 32 + 10] = (uint8_t)(xing_frames >> 8);
            f[4 + 32 + 11] = (uint8_t)xing_frames;
        }
        put(f, sizeof f);
    }
}

/* ID3v2.3/2.4 frame: id, then body */
static void frame(int ver, const char *id, const void *body, uint32_t n)
{
    put(id, 4);
    if (ver == 4) put_syncsafe(n);
    else put_be32(n);
    put8(0);
    put8(0);
    put(body, n);
}

static void text_frame(int ver, const char *id, uint8_t enc, const void *text, uint32_t n)
{
    uint8_t b[256];
    b[0] = enc;
    memcpy(b + 1, text, n);
    frame(ver, id, b, n + 1);
}

/* the tag header, its size patched after the frames */
static size_t id3_begin(int ver)
{
    put("ID3", 3);
    put8((uint8_t)ver);
    put8(0);
    put8(0);
    size_t at = g_len;
    put_syncsafe(0);
    return at;
}

static void id3_end(size_t at)
{
    uint32_t size = (uint32_t)(g_len - at - 4);
    for (int i = 0; i < 64; i++) put8(0);                /* padding */
    size += 64;
    uint8_t b[4] = { (uint8_t)((size >> 21) & 0x7f), (uint8_t)((size >> 14) & 0x7f), (uint8_t)((size >> 7) & 0x7f),
                     (uint8_t)(size & 0x7f) };
    memcpy(g_buf + at, b, 4);
}

/* ---- MP3 ----------------------------------------------------------------------- */
static void test_mp3_frame_header(void)
{
    mp3_frame_t f;
    const uint8_t h[4] = { 0xff, 0xfb, 0x90, 0x04 };
    TEST_ASSERT_TRUE(mp3_frame_parse(h, &f));
    TEST_ASSERT_EQUAL(44100, f.rate);
    TEST_ASSERT_EQUAL(128, f.kbps);
    TEST_ASSERT_EQUAL(1152, f.samples);
    TEST_ASSERT_EQUAL(417, f.bytes);
    TEST_ASSERT_EQUAL(2, f.channels);
    const uint8_t bad[4] = { 0xff, 0xfb, 0xf0, 0x04 };   /* bitrate index 15 */
    TEST_ASSERT_FALSE(mp3_frame_parse(bad, &f));
    const uint8_t mono22[4] = { 0xff, 0xf3, 0x90, 0xc4 }; /* MPEG-2, 22.05 kHz, mono */
    TEST_ASSERT_TRUE(mp3_frame_parse(mono22, &f));
    TEST_ASSERT_EQUAL(22050, f.rate);
    TEST_ASSERT_EQUAL(576, f.samples);
    TEST_ASSERT_EQUAL(1, f.channels);
}

static void test_id3v23_utf16_cover(void)
{
    g_len = 0;
    size_t at = id3_begin(3);
    text_frame(3, "TIT2", 0, "Caf\xe9", 4);              /* Latin-1 é */
    const uint8_t bjork[] = { 0xff, 0xfe, 'B', 0, 'j', 0, 0xf6, 0, 'r', 0, 'k', 0 };
    text_frame(3, "TPE1", 1, bjork, sizeof bjork);
    text_frame(3, "TALB", 0, "Debut", 5);
    text_frame(3, "TRCK", 0, "3/12", 4);
    text_frame(3, "TPOS", 0, "1/2", 3);
    text_frame(3, "TYER", 0, "1993", 4);
    text_frame(3, "TCON", 0, "(17)Rock", 8);
    /* a back cover first, then the front: the front wins */
    uint8_t apic[64];
    size_t n = 0;
    apic[n++] = 0;
    memcpy(apic + n, "image/jpeg", 11), n += 11;
    apic[n++] = 4;                                      /* back cover */
    apic[n++] = 0;                                      /* no description */
    memcpy(apic + n, JPEG, sizeof JPEG), n += sizeof JPEG;
    frame(3, "APIC", apic, (uint32_t)n);
    apic[12] = 3;                                       /* front cover */
    frame(3, "APIC", apic, (uint32_t)n);
    uint32_t front_at = (uint32_t)(g_len - sizeof JPEG);
    id3_end(at);
    uint32_t audio = (uint32_t)g_len;
    mp3_frames(10, 0);
    tags_t t;
    TEST_ASSERT_TRUE(parse(&t));
    TEST_ASSERT_EQUAL(TRACK_MP3, t.fmt);
    TEST_ASSERT_EQUAL_STRING("Caf\xc3\xa9", t.title);
    TEST_ASSERT_EQUAL_STRING("Bj\xc3\xb6rk", t.artist);
    TEST_ASSERT_EQUAL_STRING("Debut", t.album);
    TEST_ASSERT_EQUAL_STRING("Rock", t.genre);
    TEST_ASSERT_EQUAL(3, t.track);
    TEST_ASSERT_EQUAL(1, t.disc);
    TEST_ASSERT_EQUAL(1993, t.year);
    TEST_ASSERT_EQUAL(front_at, t.art_off);
    TEST_ASSERT_EQUAL(sizeof JPEG, t.art_len);
    TEST_ASSERT_EQUAL(audio, t.audio_off);
    TEST_ASSERT_EQUAL(44100, t.rate);
    TEST_ASSERT_EQUAL(260, t.duration_ms);              /* CBR: 4170 bytes at 128 kb/s */
}

static void test_id3v24_utf8_xing(void)
{
    g_len = 0;
    size_t at = id3_begin(4);
    text_frame(4, "TIT2", 3, "One\0Two", 7);             /* two values: the first */
    text_frame(4, "TPE2", 3, "Various Artists", 15);
    text_frame(4, "TDRC", 3, "2004-05-06", 10);
    id3_end(at);
    mp3_frames(4, 100);
    tags_t t;
    TEST_ASSERT_TRUE(parse(&t));
    TEST_ASSERT_EQUAL_STRING("One", t.title);
    TEST_ASSERT_EQUAL_STRING("Various Artists", t.album_artist);
    TEST_ASSERT_EQUAL(2004, t.year);
    TEST_ASSERT_EQUAL(2612, t.duration_ms);             /* 100 frames of 1152 at 44.1 kHz */
}

static void test_id3v22_and_v1(void)
{
    g_len = 0;
    put("ID3", 3);
    put8(2);
    put8(0);
    put8(0);
    size_t at = g_len;
    put_syncsafe(0);
    put("TT2", 3);
    put_be24(6);
    put8(0);
    put("Old22", 5);
    put("PIC", 3);
    put_be24(6 + sizeof JPEG);
    put8(0);
    put("JPG", 3);
    put8(3);
    put8(0);                                            /* description */
    put(JPEG, sizeof JPEG);
    id3_end(at);
    mp3_frames(3, 0);
    /* ID3v1 fills the artist v2.2 lacked */
    uint8_t v1[128] = { 'T', 'A', 'G' };
    memcpy(v1 + 3, "Ignored title", 13);
    memcpy(v1 + 33, "V1 Artist", 9);
    memcpy(v1 + 93, "1987", 4);
    v1[126] = 7;
    put(v1, 128);
    tags_t t;
    TEST_ASSERT_TRUE(parse(&t));
    TEST_ASSERT_EQUAL_STRING("Old22", t.title);
    TEST_ASSERT_EQUAL_STRING("V1 Artist", t.artist);
    TEST_ASSERT_EQUAL(1987, t.year);
    TEST_ASSERT_EQUAL(7, t.track);
    TEST_ASSERT_EQUAL(sizeof JPEG, t.art_len);
    TEST_ASSERT_EQUAL(g_len - 128, t.audio_end);
}

static void test_not_audio(void)
{
    g_len = 0;
    put("hello, this is not music at all", 31);
    tags_t t;
    TEST_ASSERT_FALSE(parse(&t));
}

/* ---- FLAC ------------------------------------------------------------------------ */
static void test_flac(void)
{
    g_len = 0;
    put("fLaC", 4);
    put8(0);                                            /* STREAMINFO */
    put_be24(34);
    uint8_t si[34] = { 0 };
    /* 48000 Hz, 2 channels, 16 bits, 96000 samples */
    si[10] = 0x0b, si[11] = 0xb8, si[12] = 0x02, si[13] = 0xf0;
    si[14] = 0, si[15] = 0x01, si[16] = 0x77, si[17] = 0x00;
    put(si, sizeof si);
    const char *c[] = { "TITLE=Flac Song", "artist=Some Band", "ALBUM=Lossless", "TRACKNUMBER=05", "DATE=2011-02-03",
                        "ALBUMARTIST=The Band" };
    uint32_t vl = 4 + 6 + 4;
    for (size_t i = 0; i < 6; i++) vl += 4 + (uint32_t)strlen(c[i]);
    put8(4);
    put_be24(vl);
    put_le32(6);
    put("vendor", 6);
    put_le32(6);
    for (size_t i = 0; i < 6; i++) {
        put_le32((uint32_t)strlen(c[i]));
        put(c[i], strlen(c[i]));
    }
    uint32_t pl = 4 + 4 + 10 + 4 + 0 + 16 + 4 + sizeof JPEG;
    put8(0x80 | 6);                                     /* last block: PICTURE */
    put_be24(pl);
    put_be32(3);
    put_be32(10);
    put("image/jpeg", 10);
    put_be32(0);
    for (int i = 0; i < 16; i++) put8(0);
    put_be32(sizeof JPEG);
    uint32_t art = (uint32_t)g_len;
    put(JPEG, sizeof JPEG);
    tags_t t;
    TEST_ASSERT_TRUE(parse(&t));
    TEST_ASSERT_EQUAL(TRACK_FLAC, t.fmt);
    TEST_ASSERT_EQUAL(48000, t.rate);
    TEST_ASSERT_EQUAL(2, t.channels);
    TEST_ASSERT_EQUAL(2000, t.duration_ms);
    TEST_ASSERT_EQUAL_STRING("Flac Song", t.title);
    TEST_ASSERT_EQUAL_STRING("Some Band", t.artist);
    TEST_ASSERT_EQUAL_STRING("Lossless", t.album);
    TEST_ASSERT_EQUAL_STRING("The Band", t.album_artist);
    TEST_ASSERT_EQUAL(5, t.track);
    TEST_ASSERT_EQUAL(2011, t.year);
    TEST_ASSERT_EQUAL(art, t.art_off);
    TEST_ASSERT_EQUAL(sizeof JPEG, t.art_len);
}

/* ---- WAV ------------------------------------------------------------------------- */
static void test_wav_info(void)
{
    g_len = 0;
    put("RIFF", 4);
    put_le32(0);
    put("WAVE", 4);
    put("fmt ", 4);
    put_le32(16);
    put_le16(1);
    put_le16(2);
    put_le32(44100);
    put_le32(44100 * 4);
    put_le16(4);
    put_le16(16);
    put("LIST", 4);
    put_le32(4 + 8 + 10 + 8 + 4);
    put("INFO", 4);
    put("INAM", 4);
    put_le32(10);
    put("Wave Song", 10);
    put("IART", 4);
    put_le32(4);
    put("Wav", 4);
    put("data", 4);
    put_le32(44100 * 4);
    for (int i = 0; i < 44100 * 4 && g_len < sizeof g_buf; i++) put8(0);   /* short: clipped to the file */
    tags_t t;
    TEST_ASSERT_TRUE(parse(&t));
    TEST_ASSERT_EQUAL(TRACK_WAV, t.fmt);
    TEST_ASSERT_EQUAL_STRING("Wave Song", t.title);
    TEST_ASSERT_EQUAL_STRING("Wav", t.artist);
    TEST_ASSERT_EQUAL(44100, t.rate);
    TEST_ASSERT_TRUE(t.duration_ms > 300 && t.duration_ms < 400);   /* what's there */
}

static void test_tag_copy_utf8(void)
{
    char d[6];
    tag_copy(d, sizeof d, "  ab\xc3\xa9\xc3\xa9", 8);   /* "abéé": the cut can't split é */
    TEST_ASSERT_EQUAL_STRING("ab\xc3\xa9", d);
    TEST_ASSERT_EQUAL(0, tag_casecmp("The Band", "the BAND"));
}

/* ---- M3U --------------------------------------------------------------------------- */
static char g_paths[8][M3U_PATH_MAX], g_titles[8][128];
static int g_n;

static void entry(const char *path, const char *title, void *user)
{
    (void)user;
    if (g_n < 8) {
        snprintf(g_paths[g_n], sizeof g_paths[0], "%s", path);
        snprintf(g_titles[g_n], sizeof g_titles[0], "%s", title);
    }
    g_n++;
}

static void test_m3u_parse(void)
{
    const char *t = "\xef\xbb\xbf#EXTM3U\r\n"
                    "#EXTINF:215,Artist - Song One\r\n"
                    "../Artist/Album/01 Song One.mp3\r\n"
                    "\r\n"
                    "  ./local.flac  \n"
                    "/sd/music/abs.wav\n"
                    "E:\\Music\\Win\\track.mp3\n"
                    "file:///sd/music/With%20Space.mp3\n"
                    "http://radio.example/stream\n"
                    "# a comment\n";
    g_n = 0;
    int n = m3u_parse(t, strlen(t), "/sd/music/playlists", entry, NULL);
    TEST_ASSERT_EQUAL(5, n);
    TEST_ASSERT_EQUAL_STRING("/sd/music/Artist/Album/01 Song One.mp3", g_paths[0]);
    TEST_ASSERT_EQUAL_STRING("Artist - Song One", g_titles[0]);
    TEST_ASSERT_EQUAL_STRING("/sd/music/playlists/local.flac", g_paths[1]);
    TEST_ASSERT_EQUAL_STRING("", g_titles[1]);
    TEST_ASSERT_EQUAL_STRING("/sd/music/abs.wav", g_paths[2]);
    TEST_ASSERT_EQUAL_STRING("/Music/Win/track.mp3", g_paths[3]);
    TEST_ASSERT_EQUAL_STRING("/sd/music/With Space.mp3", g_paths[4]);
}

static void test_m3u_paths(void)
{
    char p[M3U_PATH_MAX] = "/sd//music/./a/b/../c.mp3";
    m3u_clean(p);
    TEST_ASSERT_EQUAL_STRING("/sd/music/a/c.mp3", p);
    char q[M3U_PATH_MAX] = "/../../x";
    m3u_clean(q);
    TEST_ASSERT_EQUAL_STRING("/x", q);
    char r[M3U_PATH_MAX];
    m3u_relative(r, sizeof r, "/sd/music/playlists", "/sd/music/Artist/Album/x.mp3");
    TEST_ASSERT_EQUAL_STRING("../Artist/Album/x.mp3", r);
    m3u_relative(r, sizeof r, "/sd/music/playlists", "/sd/Music/Playlists/here.mp3");   /* FAT: any case */
    TEST_ASSERT_EQUAL_STRING("here.mp3", r);
    m3u_relative(r, sizeof r, "/sd/music/playlists", "/sd/other/y.mp3");
    TEST_ASSERT_EQUAL_STRING("../../other/y.mp3", r);
}

/* ---- library lookup -------------------------------------------------------------- */
static void test_lib_find(void)
{
    lib_track_t t[3] = { { .path = "/sd/music/Charli xcx/BRAT/1-01. 360 - Charli xcx.flac" },
                         { .path = "/sd/music/Other/Album/01 Song.mp3" },
                         { .path = "/sd/music/Other/Album/02 Song.flac" } };
    lib_t l = { .t = t, .n = 3 };
    TEST_ASSERT_EQUAL(0, lib_find(&l, "/sd/music/Charli xcx/BRAT/1-01. 360 - Charli xcx.flac"));
    TEST_ASSERT_EQUAL(0, lib_find(&l, "/SD/Music/charli xcx/brat/1-01. 360 - Charli xcx.flac"));   /* FAT: any case */
    TEST_ASSERT_EQUAL(1, lib_find(&l, "/Music/Other/Album/01 Song.mp3"));                         /* a computer's path */
    /* converted: the playlist says .m4a, the library has .flac */
    TEST_ASSERT_EQUAL(0, lib_find(&l, "/sd/music/Charli xcx/BRAT/1-01. 360 - Charli xcx.m4a"));
    TEST_ASSERT_EQUAL(2, lib_find(&l, "/sd/music/Other/Album/02 Song.m4a"));
    TEST_ASSERT_EQUAL(-1, lib_find(&l, "/sd/music/Other/Album/03 Song.m4a"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_mp3_frame_header);
    RUN_TEST(test_id3v23_utf16_cover);
    RUN_TEST(test_id3v24_utf8_xing);
    RUN_TEST(test_id3v22_and_v1);
    RUN_TEST(test_not_audio);
    RUN_TEST(test_flac);
    RUN_TEST(test_wav_info);
    RUN_TEST(test_tag_copy_utf8);
    RUN_TEST(test_m3u_parse);
    RUN_TEST(test_m3u_paths);
    RUN_TEST(test_lib_find);
    return UNITY_END();
}
