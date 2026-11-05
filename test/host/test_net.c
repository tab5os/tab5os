/* SPDX-License-Identifier: Apache-2.0
 * Phase 3: saved networks and connection policy, C6 image parsing, BLE helpers.
 */
#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "t5_mocks.h"
#include "t5_net/ble.h"
#include "t5_net/netcfg.h"
#include "c6img.h"

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
    TEST_ASSERT_EQUAL(5000, t5_netcfg_backoff_ms(1));
    TEST_ASSERT_EQUAL(10000, t5_netcfg_backoff_ms(2));
    TEST_ASSERT_EQUAL(T5_NETCFG_BACKOFF_MAX_MS, t5_netcfg_backoff_ms(20));
    t5_netcfg_backoff_t b = {0};
    TEST_ASSERT_EQUAL(100, t5_netcfg_next_retry(&b, 100));
    t5_netcfg_fail(&b, "home", 1000);
    t5_netcfg_fail(&b, "home", 1000);
    TEST_ASSERT_EQUAL(11000, t5_netcfg_next_retry(&b, 1000));
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

static void make_image(const char *ver, bool hash)
{
    memset(s_img, 0xFF, sizeof s_img);
    s_img[0] = 0xE9;
    s_img[1] = 2;                 /* segments */
    s_img[12] = 13;               /* chip id: C6 */
    s_img[13] = 0;
    s_img[23] = hash ? 1 : 0;
    /* segment 0: 300 bytes with the app descriptor first */
    put32(s_img + 24, 0x42000020);
    put32(s_img + 28, 300);
    put32(s_img + 32, 0xABCD5432);
    memset(s_img + 48, 0, 32);
    memcpy(s_img + 48, ver, strlen(ver));
    /* segment 1: 100 bytes */
    put32(s_img + 32 + 300, 0x40800000);
    put32(s_img + 36 + 300, 100);
}

static void test_c6img_parse(void)
{
    make_image("t5-c6 0.1.0", true);
    t5_c6img_info_t info;
    TEST_ASSERT_EQUAL(T5_OK, t5_c6img_parse(rd, NULL, sizeof s_img, &info));
    TEST_ASSERT_EQUAL(T5_C6_CHIP_ID, info.chip_id);
    TEST_ASSERT_EQUAL_STRING("t5-c6 0.1.0", info.version);
    /* 24 + 8 + 300 + 8 + 100 = 440; checksum pads to 448; + 32 hash */
    TEST_ASSERT_EQUAL(480, info.size);
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
    put32(s_img + 32, 0);
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_c6img_parse(rd, NULL, sizeof s_img, &info));  /* no descriptor */
    make_image("v", false);
    put32(s_img + 28, 100000);
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
    return UNITY_END();
}
