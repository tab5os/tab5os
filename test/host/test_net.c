/* SPDX-License-Identifier: Apache-2.0
 * Phase 3: saved networks and connection policy, C6 image parsing, BLE helpers.
 */
#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "t5_net/ble.h"
#include "t5_bootfmt/espimg.h"
#include "t5_net/netcfg.h"
#include "t5_net/wifimon.h"
#include "c6img.h"
#include "netproto.h"

void setUp(void) {}
void tearDown(void) {}

static void test_add_update_remove(void)
{
    t5_netcfg_t c;
    t5_netcfg_init(&c);
    TEST_ASSERT_TRUE(t5_netcfg_valid(&c));
    TEST_ASSERT_EQUAL(0, t5_netcfg_add(&c, "home", "secret123"));
    TEST_ASSERT_EQUAL(1, t5_netcfg_add(&c, "cafe", NULL));
    TEST_ASSERT_EQUAL(0, t5_netcfg_add(&c, "home", "newpass99"));   /* update in place */
    TEST_ASSERT_EQUAL_STRING("newpass99", c.e[0].pass);
    TEST_ASSERT_EQUAL(2, c.n);
    TEST_ASSERT_EQUAL(-1, t5_netcfg_add(&c, "", "x"));
    char long_ssid[40];
    memset(long_ssid, 'a', sizeof long_ssid - 1);
    long_ssid[39] = 0;
    TEST_ASSERT_EQUAL(-1, t5_netcfg_add(&c, long_ssid, NULL));
    TEST_ASSERT_TRUE(t5_netcfg_remove(&c, "home"));
    TEST_ASSERT_FALSE(t5_netcfg_remove(&c, "home"));
    TEST_ASSERT_EQUAL(1, c.n);
    TEST_ASSERT_EQUAL_STRING("cafe", c.e[0].ssid);
    TEST_ASSERT_TRUE(t5_netcfg_valid(&c));
}

static void test_full_table_evicts_least_recent(void)
{
    t5_netcfg_t c;
    t5_netcfg_init(&c);
    char name[8];
    for (int i = 0; i < T5_NETCFG_MAX; i++) {
        snprintf(name, sizeof name, "n%d", i);
        t5_netcfg_add(&c, name, "password");
        t5_netcfg_mark_ok(&c, name);
    }
    t5_netcfg_mark_ok(&c, "n0");             /* n1 is now the oldest */
    int i = t5_netcfg_add(&c, "new", "password");
    TEST_ASSERT_EQUAL_STRING("new", c.e[i].ssid);
    TEST_ASSERT_EQUAL(-1, t5_netcfg_find(&c, "n1"));
    TEST_ASSERT_TRUE(t5_netcfg_find(&c, "n0") >= 0);
}

static void test_valid_rejects_garbage(void)
{
    t5_netcfg_t c;
    memset(&c, 0xFF, sizeof c);
    TEST_ASSERT_FALSE(t5_netcfg_valid(&c));
    t5_netcfg_init(&c);
    c.n = 1;   /* empty SSID */
    TEST_ASSERT_FALSE(t5_netcfg_valid(&c));
}

static void test_backoff(void)
{
    TEST_ASSERT_EQUAL(0, t5_netcfg_backoff_ms(0));
    TEST_ASSERT_EQUAL(T5_NETCFG_BACKOFF_MIN_MS, t5_netcfg_backoff_ms(1));
    for (uint8_t f = 1; f < 20; f++) {
        uint32_t a = t5_netcfg_backoff_ms(f), b = t5_netcfg_backoff_ms((uint8_t)(f + 1));
        TEST_ASSERT_TRUE(b > a || b == T5_NETCFG_BACKOFF_MAX_MS);
        TEST_ASSERT_TRUE(b <= T5_NETCFG_BACKOFF_MAX_MS);
    }
    TEST_ASSERT_EQUAL(T5_NETCFG_BACKOFF_MAX_MS, t5_netcfg_backoff_ms(20));
    t5_netcfg_backoff_t b = {0};
    TEST_ASSERT_EQUAL(100, t5_netcfg_next_retry(&b, 100));
    t5_netcfg_fail(&b, "home", 1000);
    t5_netcfg_fail(&b, "home", 1000);
    TEST_ASSERT_EQUAL(1000 + t5_netcfg_backoff_ms(2), t5_netcfg_next_retry(&b, 1000));
    t5_netcfg_clear_fail(&b, "home");
    TEST_ASSERT_EQUAL(1000, t5_netcfg_next_retry(&b, 1000));
}

static void test_pick(void)
{
    t5_netcfg_t c;
    t5_netcfg_init(&c);
    t5_netcfg_add(&c, "home", "password");
    t5_netcfg_add(&c, "office", "password");
    t5_net_ap_t aps[] = {
        { .ssid = "stranger", .rssi = -30 },
        { .ssid = "office", .rssi = -60 },
        { .ssid = "home", .rssi = -65 },
        { .ssid = "home", .rssi = -95 },   /* too weak */
    };
    int ap = -1;
    TEST_ASSERT_EQUAL(1, t5_netcfg_pick(&c, NULL, aps, 4, 0, &ap));   /* strongest saved */
    TEST_ASSERT_EQUAL(1, ap);
    t5_netcfg_mark_ok(&c, "home");                                    /* last good gets +10 */
    TEST_ASSERT_EQUAL(0, t5_netcfg_pick(&c, NULL, aps, 4, 0, &ap));
    TEST_ASSERT_EQUAL(2, ap);
    t5_netcfg_backoff_t b = {0};
    t5_netcfg_fail(&b, "home", 0);
    TEST_ASSERT_EQUAL(1, t5_netcfg_pick(&c, &b, aps, 4, 100, &ap));   /* home backing off */
    TEST_ASSERT_EQUAL(0, t5_netcfg_pick(&c, &b, aps, 4, 6000, &ap));  /* backoff over */
    t5_netcfg_fail(&b, "office", 6000);
    TEST_ASSERT_EQUAL(-1, t5_netcfg_pick(&c, &b, aps + 1, 1, 6000, &ap));
}

/* ---- C6 image ------------------------------------------------------------- */

static uint8_t s_img[4096];

static t5_err_t rd(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    (void)ctx;
    if (off + len > sizeof s_img) return T5_ERR_IO;
    memcpy(buf, s_img + off, len);
    return T5_OK;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

#define SEG0_LEN 300
#define SEG1_LEN 100
#define SEG1_OFF (T5_IMG_DESC_OFF + SEG0_LEN)

static void make_image(const char *ver, bool hash)
{
    memset(s_img, 0xFF, sizeof s_img);
    s_img[0] = T5_IMG_MAGIC;
    s_img[T5_IMG_SEGMENTS_OFF] = 2;
    s_img[T5_IMG_CHIP_ID_OFF] = T5_IMG_CHIP_ESP32C6;
    s_img[T5_IMG_CHIP_ID_OFF + 1] = 0;
    s_img[T5_IMG_HASH_APPENDED_OFF] = hash ? 1 : 0;
    /* segment 0 holds the app descriptor */
    put32(s_img + T5_IMG_HDR_LEN, 0x42000020);
    put32(s_img + T5_IMG_HDR_LEN + 4, SEG0_LEN);
    put32(s_img + T5_IMG_DESC_OFF, T5_IMG_DESC_MAGIC);
    memset(s_img + T5_IMG_DESC_OFF + T5_IMG_DESC_VERSION_OFF, 0, T5_IMG_DESC_VERSION_LEN);
    memcpy(s_img + T5_IMG_DESC_OFF + T5_IMG_DESC_VERSION_OFF, ver, strlen(ver));
    put32(s_img + SEG1_OFF, 0x40800000);
    put32(s_img + SEG1_OFF + 4, SEG1_LEN);
}

static void test_c6img_parse(void)
{
    make_image("t5-c6 0.1.0", true);
    t5_c6img_info_t info;
    TEST_ASSERT_EQUAL(T5_OK, t5_c6img_parse(rd, NULL, sizeof s_img, &info));
    TEST_ASSERT_EQUAL(T5_C6_CHIP_ID, info.chip_id);
    TEST_ASSERT_EQUAL_STRING("t5-c6 0.1.0", info.version);
    TEST_ASSERT_EQUAL(480, info.size);   /* segments end at 440, checksum block to 448, SHA-256 */
    make_image("x", false);
    TEST_ASSERT_EQUAL(T5_OK, t5_c6img_parse(rd, NULL, sizeof s_img, &info));
    TEST_ASSERT_EQUAL(448, info.size);
}

static void test_c6img_rejects(void)
{
    t5_c6img_info_t info;
    memset(s_img, 0xFF, sizeof s_img);
    TEST_ASSERT_EQUAL(T5_ERR_NOT_FOUND, t5_c6img_parse(rd, NULL, sizeof s_img, &info));   /* erased */
    make_image("v", false);
    put32(s_img + T5_IMG_DESC_OFF, 0);
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_c6img_parse(rd, NULL, sizeof s_img, &info));  /* no descriptor */
    make_image("v", false);
    put32(s_img + T5_IMG_HDR_LEN + 4, 100000);
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_c6img_parse(rd, NULL, sizeof s_img, &info));  /* runs off the end */
}

/* ---- BLE helpers ------------------------------------------------------------- */

static void test_ble_addr(void)
{
    uint8_t a[6];
    TEST_ASSERT_TRUE(t5_ble_parse_addr("48:69:b4:f9:4b:1c", a));
    TEST_ASSERT_EQUAL_HEX8(0x1c, a[0]);
    TEST_ASSERT_EQUAL_HEX8(0x48, a[5]);
    char s[18];
    t5_ble_format_addr(a, s);
    TEST_ASSERT_EQUAL_STRING("48:69:b4:f9:4b:1c", s);
    TEST_ASSERT_FALSE(t5_ble_parse_addr("48:69:b4:f9:4b", a));
    TEST_ASSERT_FALSE(t5_ble_parse_addr("48:69:b4:f9:4b:1c:00", a));
}

static void test_ble_adv_name(void)
{
    const uint8_t adv[] = { 2, 0x01, 0x06, 4, 0x08, 'A', 'B', 'C', 6, 0x09, 'T', 'a', 'b', '5', '!' };
    char n[8];
    TEST_ASSERT_TRUE(t5_ble_adv_name(adv, sizeof adv, n, sizeof n));
    TEST_ASSERT_EQUAL_STRING("Tab5!", n);              /* complete name wins over short */
    TEST_ASSERT_TRUE(t5_ble_adv_name(adv, 8, n, sizeof n));
    TEST_ASSERT_EQUAL_STRING("ABC", n);
    const uint8_t bad[] = { 9, 0x09, 'x' };            /* length runs past the end */
    TEST_ASSERT_FALSE(t5_ble_adv_name(bad, sizeof bad, n, sizeof n));
    char tiny[3];
    TEST_ASSERT_TRUE(t5_ble_adv_name(adv, sizeof adv, tiny, sizeof tiny));
    TEST_ASSERT_EQUAL_STRING("Ta", tiny);
}

/* ---- netproto: URLs, HTTP heads, chunked bodies, WebSocket frames ---- */
static void test_url_parse(void)
{
    t5np_url_t u;
    TEST_ASSERT_EQUAL(0, t5np_url_parse("https://api.example.com/v1/x?a=1#f", &u));
    TEST_ASSERT_TRUE(u.tls);
    TEST_ASSERT_EQUAL_STRING("api.example.com", u.host);
    TEST_ASSERT_EQUAL(443, u.port);
    TEST_ASSERT_FALSE(u.port_given);
    TEST_ASSERT_EQUAL_STRING("/v1/x?a=1#f", u.path);
    TEST_ASSERT_EQUAL(0, t5np_url_parse("http://user:pw@10.0.0.2:8080", &u));
    TEST_ASSERT_EQUAL(8080, u.port);
    TEST_ASSERT_EQUAL_STRING("10.0.0.2", u.host);
    TEST_ASSERT_EQUAL_STRING("", u.path);
    TEST_ASSERT_EQUAL(7, (int)u.userinfo_len);
    TEST_ASSERT_EQUAL(0, t5np_url_parse("ws://[fe80::1]:81/chat", &u));
    TEST_ASSERT_EQUAL_STRING("fe80::1", u.host);
    TEST_ASSERT_EQUAL(T5NP_WS, u.scheme);
    TEST_ASSERT_EQUAL(81, u.port);
    TEST_ASSERT_EQUAL(0, t5np_url_parse("WSS://h", &u));
    TEST_ASSERT_EQUAL(443, u.port);
    TEST_ASSERT_EQUAL(-1, t5np_url_parse("ftp://h/", &u));
    TEST_ASSERT_EQUAL(-1, t5np_url_parse("http://h:0/", &u));
    TEST_ASSERT_EQUAL(-1, t5np_url_parse("http://h:99999/", &u));
    TEST_ASSERT_EQUAL(-1, t5np_url_parse("http:///x", &u));
    TEST_ASSERT_EQUAL(-1, t5np_url_parse("http://h/a b", &u));
    TEST_ASSERT_EQUAL(-1, t5np_url_parse("http://h/a\r\nX: y", &u));
    TEST_ASSERT_EQUAL(-1, t5np_url_parse("http://[::1/", &u));
}

static void test_url_join(void)
{
    char out[128];
    TEST_ASSERT_EQUAL(0, t5np_url_join("http://h/a/b?q", "c", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("http://h/a/c", out);
    TEST_ASSERT_EQUAL(0, t5np_url_join("http://h:81/a/b", "/x", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("http://h:81/x", out);
    TEST_ASSERT_EQUAL(0, t5np_url_join("https://h/a", "//o.org/p", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("https://o.org/p", out);
    TEST_ASSERT_EQUAL(0, t5np_url_join("http://h/a", "https://z/", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("https://z/", out);
    TEST_ASSERT_EQUAL(0, t5np_url_join("http://h/a?x", "?y", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("http://h/a?y", out);
    TEST_ASSERT_EQUAL(0, t5np_url_join("http://h", "x", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("http://h/x", out);
    TEST_ASSERT_EQUAL(-1, t5np_url_join("http://h/", "/very/long/path", out, 12));
}

static void test_head_parse(void)
{
    char buf[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain \r\nX-A:1\r\n folded\r\nTransfer-Encoding: gzip, Chunked\r\n\r\nbody";
    size_t end = t5np_head_end(buf, strlen(buf));
    TEST_ASSERT_EQUAL(strlen(buf) - 4, end);
    t5np_head_t h;
    TEST_ASSERT_EQUAL(0, t5np_head_parse(buf, end, &h));
    TEST_ASSERT_EQUAL(200, h.status);
    TEST_ASSERT_EQUAL(1, h.minor);
    TEST_ASSERT_EQUAL(3, h.n);
    TEST_ASSERT_EQUAL_STRING("text/plain", t5np_head_get(&h, "content-type"));
    TEST_ASSERT_EQUAL_STRING("1", t5np_head_get(&h, "X-A"));
    TEST_ASSERT_NULL(t5np_head_get(&h, "missing"));
    TEST_ASSERT_TRUE(t5np_has_token(t5np_head_get(&h, "Transfer-Encoding"), "chunked"));
    TEST_ASSERT_FALSE(t5np_has_token("chunkedx", "chunked"));
    char partial[] = "HTTP/1.1 200 OK\r\nA: b\r\n";
    TEST_ASSERT_EQUAL(0, t5np_head_end(partial, strlen(partial)));
    char bad[] = "HTTP/2 200 OK\r\n\r\n";
    TEST_ASSERT_EQUAL(-1, t5np_head_parse(bad, strlen(bad), &h));
    char bad2[] = "HTTP/1.1 200 OK\r\nNo colon\r\n\r\n";
    TEST_ASSERT_EQUAL(-1, t5np_head_parse(bad2, strlen(bad2), &h));
    char lf[] = "HTTP/1.0 404 Not Found\nServer: x\n\n";
    end = t5np_head_end(lf, strlen(lf));
    TEST_ASSERT_EQUAL(strlen(lf), end);
    TEST_ASSERT_EQUAL(0, t5np_head_parse(lf, end, &h));
    TEST_ASSERT_EQUAL(404, h.status);
    TEST_ASSERT_EQUAL_STRING("x", t5np_head_get(&h, "server"));
}

static void test_req_parse(void)
{
    char buf[] = "POST /api/x?a=1 HTTP/1.1\r\nHost: tab5\r\nContent-Length: 4\r\n\r\nbody";
    size_t end = t5np_head_end(buf, strlen(buf));
    t5np_head_t h;
    TEST_ASSERT_EQUAL(0, t5np_req_parse(buf, end, &h));
    TEST_ASSERT_EQUAL_STRING("POST", h.method);
    TEST_ASSERT_EQUAL_STRING("/api/x?a=1", h.target);
    TEST_ASSERT_EQUAL(1, h.minor);
    TEST_ASSERT_EQUAL_STRING("4", t5np_head_get(&h, "content-length"));
    char bad[] = "get / HTTP/1.1\r\n\r\n";                       /* lower-case method */
    TEST_ASSERT_EQUAL(-1, t5np_req_parse(bad, strlen(bad), &h));
    char bad2[] = "GET /a b HTTP/1.1\r\n\r\n";
    TEST_ASSERT_EQUAL(-1, t5np_req_parse(bad2, strlen(bad2), &h));
    char bad3[] = "GET / SPDY/3\r\n\r\n";
    TEST_ASSERT_EQUAL(-1, t5np_req_parse(bad3, strlen(bad3), &h));
}

static void test_chunked(void)
{
    const char *src = "4\r\nWiki\r\n5;ext=1\r\npedia\r\nE\r\n in\r\n\r\nchunks.\r\n0\r\nTrailer: x\r\n\r\nGARBAGE";
    const char *want = "Wikipedia in\r\n\r\nchunks.";
    /* Fed whole, and one byte at a time. */
    for (int step = 0; step < 2; step++) {
        t5np_chunk_t c;
        t5np_chunk_init(&c);
        char out[128] = { 0 };
        size_t outn = 0, in = 0, n = strlen(src);
        while (in < n && !c.done) {
            uint8_t buf[128];
            size_t take = step ? 1 : n - in;
            memcpy(buf, src + in, take);
            size_t used;
            size_t got = t5np_chunk_decode(&c, buf, take, &used);
            memcpy(out + outn, buf, got);
            outn += got;
            in += used;
            TEST_ASSERT_FALSE(c.bad);
        }
        TEST_ASSERT_TRUE(c.done);
        TEST_ASSERT_EQUAL_STRING(want, out);
        TEST_ASSERT_EQUAL_STRING("GARBAGE", src + in);
    }
    t5np_chunk_t c;
    t5np_chunk_init(&c);
    uint8_t bad[] = "zz\r\n";
    size_t used;
    t5np_chunk_decode(&c, bad, 4, &used);
    TEST_ASSERT_TRUE(c.bad);
    t5np_chunk_init(&c);
    uint8_t bad2[] = "2\r\nabX";
    t5np_chunk_decode(&c, bad2, 6, &used);
    TEST_ASSERT_TRUE(c.bad);
}

static void test_ws_frames(void)
{
    uint8_t h[14];
    const uint8_t mask[4] = { 1, 2, 3, 4 };
    TEST_ASSERT_EQUAL(6, t5np_ws_header(h, T5NP_WS_TEXT, true, 5, mask));
    TEST_ASSERT_EQUAL_HEX8(0x81, h[0]);
    TEST_ASSERT_EQUAL_HEX8(0x85, h[1]);
    TEST_ASSERT_EQUAL(4, t5np_ws_header(h, T5NP_WS_BINARY, true, 300, NULL));
    TEST_ASSERT_EQUAL_HEX8(126, h[1]);
    TEST_ASSERT_EQUAL(10, t5np_ws_header(h, T5NP_WS_BINARY, false, 70000, NULL));
    t5np_ws_frame_t f;
    TEST_ASSERT_EQUAL(1, t5np_ws_parse(h, 10, &f));
    TEST_ASSERT_EQUAL(70000, (int)f.len);
    TEST_ASSERT_FALSE(f.fin);
    TEST_ASSERT_EQUAL(10, (int)f.hdr_len);
    TEST_ASSERT_EQUAL(0, t5np_ws_parse(h, 5, &f));                 /* need more */
    uint8_t m[] = { 0x89, 0x82, 9, 9, 9, 9, 'h' ^ 9, 'i' ^ 9 };      /* masked ping "hi" */
    TEST_ASSERT_EQUAL(1, t5np_ws_parse(m, sizeof m, &f));
    TEST_ASSERT_TRUE(f.masked);
    TEST_ASSERT_EQUAL(T5NP_WS_PING, f.opcode);
    t5np_ws_mask(m + 6, 2, f.mask, 0);
    TEST_ASSERT_EQUAL_MEMORY("hi", m + 6, 2);
    uint8_t frag_ctl[] = { 0x09, 0x00 };                             /* fragmented ping */
    TEST_ASSERT_EQUAL(-1, t5np_ws_parse(frag_ctl, 2, &f));
    uint8_t big_ctl[] = { 0x88, 126, 0, 200 };
    TEST_ASSERT_EQUAL(-1, t5np_ws_parse(big_ctl, 4, &f));
    uint8_t bad_op[] = { 0x83, 0 };
    TEST_ASSERT_EQUAL(-1, t5np_ws_parse(bad_op, 2, &f));
    uint8_t data[] = "abcdef";
    t5np_ws_mask(data, 6, mask, 0);
    t5np_ws_mask(data, 3, mask, 0);
    t5np_ws_mask(data + 3, 3, mask, 3);                              /* chunked unmask */
    TEST_ASSERT_EQUAL_MEMORY("abcdef", data, 6);
}

static void test_sha1_base64_accept(void)
{
    uint8_t d[20];
    char hex[41];
    t5np_sha1("abc", 3, d);
    for (int i = 0; i < 20; i++) sprintf(hex + 2 * i, "%02x", d[i]);
    TEST_ASSERT_EQUAL_STRING("a9993e364706816aba3e25717850c26c9cd0d89d", hex);
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";   /* 56 bytes: two blocks */
    t5np_sha1(two, strlen(two), d);
    for (int i = 0; i < 20; i++) sprintf(hex + 2 * i, "%02x", d[i]);
    TEST_ASSERT_EQUAL_STRING("84983e441c3bd26ebaae4aa1f95129e5e54670f1", hex);
    char b[16];
    t5np_base64((const uint8_t *)"Ma", 2, b);
    TEST_ASSERT_EQUAL_STRING("TWE=", b);
    t5np_base64((const uint8_t *)"Man", 3, b);
    TEST_ASSERT_EQUAL_STRING("TWFu", b);
    char acc[29];
    t5np_ws_accept("dGhlIHNhbXBsZSBub25jZQ==", acc);                /* RFC 6455 section 1.3 */
    TEST_ASSERT_EQUAL_STRING("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", acc);
}

static void test_wifimon_pcap(void)
{
    uint8_t h[24];
    TEST_ASSERT_EQUAL(24, t5_wifimon_pcap_header(h));
    TEST_ASSERT_EQUAL_HEX8(0xd4, h[0]);              /* a1b2c3d4, little-endian */
    TEST_ASSERT_EQUAL_HEX8(127, h[20]);              /* radiotap */
    const uint8_t beacon[30] = { 0x80, 0x00 };
    t5_wifimon_frame_t m = { .time_us = 3500123, .rssi = -61, .noise_floor = -95, .channel = 6, .len = 200 };
    uint8_t rec[128];
    size_t n = t5_wifimon_pcap_record(&m, beacon, sizeof beacon, rec, sizeof rec);
    TEST_ASSERT_EQUAL(16 + 16 + sizeof beacon, n);
    TEST_ASSERT_EQUAL_HEX8(3, rec[0]);               /* seconds */
    TEST_ASSERT_EQUAL(500123, rec[4] | rec[5] << 8 | rec[6] << 16);
    TEST_ASSERT_EQUAL(16 + 30, rec[8]);              /* captured */
    TEST_ASSERT_EQUAL(16 + 200, rec[12]);            /* on the air */
    const uint8_t *rt = rec + 16;
    TEST_ASSERT_EQUAL(16, rt[2]);                    /* radiotap length */
    TEST_ASSERT_EQUAL_HEX8(0x6a, rt[4]);             /* flags, channel, signal, noise */
    TEST_ASSERT_EQUAL(2437, rt[10] | rt[11] << 8);   /* channel 6 */
    TEST_ASSERT_EQUAL((uint8_t)-61, rt[14]);
    TEST_ASSERT_EQUAL_HEX8(0x80, rec[32]);           /* the frame follows */
    TEST_ASSERT_EQUAL(0, t5_wifimon_pcap_record(&m, beacon, sizeof beacon, rec, n - 1));
    m.channel = 14;
    t5_wifimon_pcap_record(&m, beacon, sizeof beacon, rec, sizeof rec);
    TEST_ASSERT_EQUAL(2484, rec[26] | rec[27] << 8);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_add_update_remove);
    RUN_TEST(test_full_table_evicts_least_recent);
    RUN_TEST(test_valid_rejects_garbage);
    RUN_TEST(test_backoff);
    RUN_TEST(test_pick);
    RUN_TEST(test_c6img_parse);
    RUN_TEST(test_c6img_rejects);
    RUN_TEST(test_ble_addr);
    RUN_TEST(test_ble_adv_name);
    RUN_TEST(test_url_parse);
    RUN_TEST(test_url_join);
    RUN_TEST(test_head_parse);
    RUN_TEST(test_req_parse);
    RUN_TEST(test_chunked);
    RUN_TEST(test_ws_frames);
    RUN_TEST(test_sha1_base64_accept);
    RUN_TEST(test_wifimon_pcap);
    return UNITY_END();
}
