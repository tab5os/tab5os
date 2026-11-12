/* SPDX-License-Identifier: Apache-2.0
 * Phase 9: the boot-control record convention shared by the bootloader,
 * the OS and recovery; which slot an install writes; which images each
 * source may install; the .t5os header as tools/ota/t5ota.py writes it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "unity.h"
#include "t5_bootfmt/bootctl.h"
#include "t5_bootfmt/espimg.h"
#include "t5_ota/ota.h"
#include "ota_rule.h"
#include "ota_slot.h"

void setUp(void) {}
void tearDown(void) {}

/* One boot as the bootloader runs it (t5_bootctl_boot, then the flash
 * write): returns true when it starts recovery. */
static bool boot(uint8_t rec[T5_BOOTCTL_REC_LEN], bool deep_sleep)
{
    t5_bootctl_boot_t b = t5_bootctl_boot(rec, deep_sleep);
    if (b.mark) {
        TEST_ASSERT_EQUAL(0, b.word_off % 4);
        uint8_t w[4];
        memcpy(w, &b.word, 4);
        for (int i = 0; i < 4; i++) {
            TEST_ASSERT_EQUAL(0, w[i] & ~rec[b.word_off + i]);     /* flash only clears bits */
            rec[b.word_off + i] = w[i];
        }
    }
    return b.recovery;
}

static void erase(uint8_t rec[T5_BOOTCTL_REC_LEN])
{
    memset(rec, 0xFF, T5_BOOTCTL_REC_LEN);
}

static void request(uint8_t rec[T5_BOOTCTL_REC_LEN], const char *req)
{
    memcpy(rec + T5_BOOTCTL_REQ_OFFSET, req, T5_BOOTCTL_REQ_LEN);
}

static void test_bootctl_failed_boots(void)
{
    uint8_t rec[T5_BOOTCTL_REC_LEN];
    erase(rec);
    TEST_ASSERT_EQUAL(T5_BOOT_REQ_NONE, t5_bootctl_request(rec));
    /* A healthy boot erases the record: the next one sees no failures. */
    TEST_ASSERT_FALSE(boot(rec, false));
    TEST_ASSERT_EQUAL(0, t5_bootctl_failed_before(rec, false));
    erase(rec);
    /* Boots that never get healthy. */
    for (int i = 0; i < T5_BOOTCTL_RECOVERY_AFTER; i++) {
        TEST_ASSERT_FALSE(boot(rec, false));
        TEST_ASSERT_EQUAL(i, t5_bootctl_failed_before(rec, false));
        /* Two failed boots start the OS in safe mode. */
        TEST_ASSERT_EQUAL(i >= 2, t5_bootctl_safe_mode(T5_BOOT_REQ_NONE, t5_bootctl_failed_before(rec, false)));
    }
    /* The next one starts recovery, which reports the failed OS boots. */
    TEST_ASSERT_TRUE(boot(rec, false));
    TEST_ASSERT_EQUAL(T5_BOOTCTL_RECOVERY_AFTER, t5_bootctl_failed_before(rec, false));
}

static void test_bootctl_requests(void)
{
    uint8_t rec[T5_BOOTCTL_REC_LEN];
    erase(rec);
    request(rec, T5_BOOTCTL_REQ_RECOVERY);
    TEST_ASSERT_EQUAL(T5_BOOT_REQ_RECOVERY, t5_bootctl_request(rec));
    TEST_ASSERT_TRUE(boot(rec, false));
    TEST_ASSERT_EQUAL(0, t5_bootctl_count(rec));                /* not counted */
    TEST_ASSERT_EQUAL(0, t5_bootctl_failed_before(rec, false));

    erase(rec);
    request(rec, T5_BOOTCTL_REQ_SAFEMODE);
    TEST_ASSERT_EQUAL(T5_BOOT_REQ_SAFEMODE, t5_bootctl_request(rec));
    TEST_ASSERT_FALSE(boot(rec, false));
    TEST_ASSERT_EQUAL(1, t5_bootctl_count(rec));
    TEST_ASSERT_EQUAL(0, t5_bootctl_failed_before(rec, false));
    TEST_ASSERT_TRUE(t5_bootctl_safe_mode(t5_bootctl_request(rec), t5_bootctl_failed_before(rec, false)));

    /* A deep-sleep wake is no attempt. */
    erase(rec);
    TEST_ASSERT_FALSE(boot(rec, false));
    TEST_ASSERT_FALSE(boot(rec, true));
    TEST_ASSERT_EQUAL(1, t5_bootctl_count(rec));
    TEST_ASSERT_EQUAL(1, t5_bootctl_failed_before(rec, true));
}

static void test_bootctl_full(void)
{
    uint8_t rec[T5_BOOTCTL_REC_LEN];
    erase(rec);
    memset(rec, 0, T5_BOOTCTL_ATTEMPT_BYTES);
    uint32_t off, word;
    TEST_ASSERT_FALSE(t5_bootctl_mark(rec, &off, &word));
    TEST_ASSERT_TRUE(boot(rec, false));
}

static void test_target_slot(void)
{
    TEST_ASSERT_EQUAL(1, t5_ota_target_slot(0, 0, 2));
    TEST_ASSERT_EQUAL(0, t5_ota_target_slot(1, 1, 2));
    TEST_ASSERT_EQUAL(0, t5_ota_target_slot(1, 0, 2));        /* an earlier install pointed boot at 0 */
    /* From recovery: the slot that fails to boot, never the other one. */
    TEST_ASSERT_EQUAL(0, t5_ota_target_slot(-1, 0, 2));
    TEST_ASSERT_EQUAL(1, t5_ota_target_slot(-1, 1, 2));
    TEST_ASSERT_EQUAL(0, t5_ota_target_slot(-1, -1, 2));
    TEST_ASSERT_EQUAL(-1, t5_ota_target_slot(0, 0, 1));
    TEST_ASSERT_EQUAL(-1, t5_ota_target_slot(-1, 0, 0));
}

static void test_source_rule(void)
{
    t5_ota_hdr_t unsig = { .sig_len = 0 }, other = { .sig_len = T5_OTA_SIG_LEN };
    const t5_ota_source_t all[] = { T5_OTA_FROM_SETTINGS, T5_OTA_FROM_CONSOLE, T5_OTA_FROM_DEV,
                                    T5_OTA_FROM_RECOVERY };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        TEST_ASSERT_NULL(t5_ota_refusal(&other, true, all[i], false));     /* a built-in key: always */
    }
    /* Settings: developer mode; the console: `unsigned`. Either kind. */
    TEST_ASSERT_NOT_NULL(t5_ota_refusal(&unsig, false, T5_OTA_FROM_SETTINGS, false));
    TEST_ASSERT_NOT_NULL(t5_ota_refusal(&other, false, T5_OTA_FROM_SETTINGS, false));
    TEST_ASSERT_NULL(t5_ota_refusal(&unsig, false, T5_OTA_FROM_SETTINGS, true));
    TEST_ASSERT_NULL(t5_ota_refusal(&other, false, T5_OTA_FROM_SETTINGS, true));
    TEST_ASSERT_NOT_NULL(t5_ota_refusal(&unsig, false, T5_OTA_FROM_CONSOLE, false));
    TEST_ASSERT_NULL(t5_ota_refusal(&other, false, T5_OTA_FROM_CONSOLE, true));
    /* The developer server takes anything. */
    TEST_ASSERT_NULL(t5_ota_refusal(&unsig, false, T5_OTA_FROM_DEV, false));
    TEST_ASSERT_NULL(t5_ota_refusal(&other, false, T5_OTA_FROM_DEV, false));
    /* Recovery: unsigned yes, an unknown key never. */
    TEST_ASSERT_NULL(t5_ota_refusal(&unsig, false, T5_OTA_FROM_RECOVERY, false));
    TEST_ASSERT_NOT_NULL(t5_ota_refusal(&other, false, T5_OTA_FROM_RECOVERY, true));
}

static void test_version_rule(void)
{
    TEST_ASSERT_TRUE(t5_ota_version_cmp("0.1.10", "0.1.9") > 0);
    TEST_ASSERT_TRUE(t5_ota_version_cmp("0.2.0", "0.1.9") > 0);
    TEST_ASSERT_TRUE(t5_ota_version_cmp("1.0", "0.9.9") > 0);
    TEST_ASSERT_TRUE(t5_ota_version_cmp("0.1.2", "0.1.3") < 0);
    TEST_ASSERT_EQUAL(0, t5_ota_version_cmp("0.1.3", "0.1.3-dirty"));
    TEST_ASSERT_EQUAL(0, t5_ota_version_cmp("0.1", "0.1.0"));
    /* Settings without developer mode: the same or newer only. */
    TEST_ASSERT_NOT_NULL(t5_ota_downgrade_refusal("0.1.2", "0.1.3", T5_OTA_FROM_SETTINGS, false));
    TEST_ASSERT_NULL(t5_ota_downgrade_refusal("0.1.3", "0.1.3", T5_OTA_FROM_SETTINGS, false));
    TEST_ASSERT_NULL(t5_ota_downgrade_refusal("0.1.4", "0.1.3", T5_OTA_FROM_SETTINGS, false));
    TEST_ASSERT_NULL(t5_ota_downgrade_refusal("0.1.2", "0.1.3", T5_OTA_FROM_SETTINGS, true));
    TEST_ASSERT_NULL(t5_ota_downgrade_refusal("0.1.2", "0.1.3", T5_OTA_FROM_CONSOLE, false));
    TEST_ASSERT_NULL(t5_ota_downgrade_refusal("0.1.2", "0.1.3", T5_OTA_FROM_DEV, false));
    TEST_ASSERT_NULL(t5_ota_downgrade_refusal("0.1.2", "0.1.3", T5_OTA_FROM_RECOVERY, false));
}

#define REL(v) "{\"tag_name\":\"v" v "\",\"assets\":[" \
    "{\"browser_download_url\":\"https://github.com/tab5os/tab5os/releases/download/v" v "/tab5os-sdk-" v ".tar.gz\"}," \
    "{\"browser_download_url\":\"https://github.com/tab5os/tab5os/releases/download/v" v "/tab5os-" v ".t5os\"}]}"

static void test_pick_release(void)
{
    char url[192], ver[32];
    /* Newest first as GitHub lists them, but the order doesn't matter. */
    const char *list = "[" REL("0.1.10") "," REL("0.1.3") "," REL("0.1.9") "]";
    TEST_ASSERT_TRUE(t5_ota_pick_release(list, "0.1.3", url, sizeof url, ver, sizeof ver));
    TEST_ASSERT_EQUAL_STRING("0.1.10", ver);
    TEST_ASSERT_EQUAL_STRING("https://github.com/tab5os/tab5os/releases/download/v0.1.10/tab5os-0.1.10.t5os", url);
    TEST_ASSERT_FALSE(t5_ota_pick_release(list, "0.1.10", url, sizeof url, ver, sizeof ver));
    TEST_ASSERT_FALSE(t5_ota_pick_release("[]", "0.1.3", url, sizeof url, ver, sizeof ver));
    /* GitHub's API pretty-prints: a space after the colon. */
    TEST_ASSERT_TRUE(t5_ota_pick_release(
        "[{\n  \"browser_download_url\": \"https://github.com/tab5os/tab5os/releases/download/v0.1.4/tab5os-0.1.4.t5os\"\n}]",
        "0.1.3", url, sizeof url, ver, sizeof ver));
    TEST_ASSERT_EQUAL_STRING("0.1.4", ver);
    /* Another repo, a file that isn't the update, or a mismatched name: not an update. */
    TEST_ASSERT_FALSE(t5_ota_pick_release(
        "[{\"browser_download_url\":\"https://github.com/evil/tab5os/releases/download/v9.0.0/tab5os-9.0.0.t5os\"},"
        "{\"browser_download_url\":\"https://github.com/tab5os/tab5os/releases/download/v9.0.0/other.t5os\"},"
        "{\"browser_download_url\":\"https://github.com/tab5os/tab5os/releases/download/v9.0.0/tab5os-8.0.0.t5os\"}]",
        "0.1.3", url, sizeof url, ver, sizeof ver));
    /* A URL quoted in a release's notes is escaped, so it doesn't count. */
    TEST_ASSERT_FALSE(t5_ota_pick_release(
        "[{\"body\":\"see \\\"browser_download_url\\\":\\\"https://github.com/tab5os/tab5os/releases/download/"
        "v9.0.0/tab5os-9.0.0.t5os\\\"\",\"assets\":[]}]",
        "0.1.3", url, sizeof url, ver, sizeof ver));
}

/* ---- .t5os from t5ota.py ------------------------------------------------- */
#define IMG_LEN 4096

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void make_image(uint8_t *img, uint16_t chip)
{
    for (int i = 0; i < IMG_LEN; i++) img[i] = (uint8_t)(i * 7);
    img[0] = T5_IMG_MAGIC;
    put16(img + T5_IMG_CHIP_ID_OFF, chip);
    put16(img + T5_IMG_MIN_REV_OFF, 100);
    put16(img + T5_IMG_MAX_REV_OFF, 199);
    uint32_t m = T5_IMG_DESC_MAGIC;
    memcpy(img + T5_IMG_DESC_OFF, &m, 4);
    memset(img + T5_IMG_DESC_OFF + T5_IMG_DESC_VERSION_OFF, 0, T5_IMG_DESC_VERSION_LEN);
    strcpy((char *)img + T5_IMG_DESC_OFF + T5_IMG_DESC_VERSION_OFF, "9.8.7-test");
    memset(img + T5_IMG_DESC_OFF + T5_IMG_DESC_PROJECT_OFF, 0, T5_IMG_DESC_PROJECT_LEN);
    strcpy((char *)img + T5_IMG_DESC_OFF + T5_IMG_DESC_PROJECT_OFF, T5_OTA_PROJECT);
}

static int t5ota(const char *args)
{
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "python3 '%s/tools/ota/t5ota.py' %s > /dev/null 2>&1", T5_REPO_ROOT, args);
    return system(cmd);
}

static void test_ota_header_from_tool(void)
{
    char dir[] = "/tmp/t5ota_XXXXXX";
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    char img_path[64], out_path[64], args[256];
    snprintf(img_path, sizeof img_path, "%s/img.bin", dir);
    snprintf(out_path, sizeof out_path, "%s/out.t5os", dir);
    static uint8_t img[IMG_LEN];
    make_image(img, T5_IMG_CHIP_ESP32P4);
    FILE *f = fopen(img_path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL(IMG_LEN, fwrite(img, 1, IMG_LEN, f));
    fclose(f);

    snprintf(args, sizeof args, "pack '%s' -o '%s' --chip-rev 1.3", img_path, out_path);
    TEST_ASSERT_EQUAL(0, t5ota(args));
    static uint8_t file[T5_OTA_HDR_SIZE + IMG_LEN + 1];
    f = fopen(out_path, "rb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL(T5_OTA_HDR_SIZE + IMG_LEN, fread(file, 1, sizeof file, f));
    fclose(f);
    t5_ota_hdr_t h;
    memcpy(&h, file, sizeof h);
    TEST_ASSERT_EQUAL_MEMORY(T5_OTA_MAGIC, h.magic, 4);
    TEST_ASSERT_EQUAL(T5_OTA_HDR_VERSION, h.version);
    TEST_ASSERT_EQUAL(0, h.sig_len);
    TEST_ASSERT_EQUAL(IMG_LEN, h.image_len);
    TEST_ASSERT_EQUAL_STRING("9.8.7-test", h.os_version);
    TEST_ASSERT_EQUAL_MEMORY(img, file + T5_OTA_HDR_SIZE, IMG_LEN);
    snprintf(args, sizeof args, "inspect '%s'", out_path);
    TEST_ASSERT_EQUAL(0, t5ota(args));                           /* its hash matches */

    /* Refused like the device refuses: another chip revision or chip. */
    snprintf(args, sizeof args, "pack '%s' -o '%s' --chip-rev 3.0", img_path, out_path);
    TEST_ASSERT_NOT_EQUAL(0, t5ota(args));
    make_image(img, T5_IMG_CHIP_ESP32C6);
    f = fopen(img_path, "wb");
    TEST_ASSERT_EQUAL(IMG_LEN, fwrite(img, 1, IMG_LEN, f));
    fclose(f);
    snprintf(args, sizeof args, "pack '%s' -o '%s'", img_path, out_path);
    TEST_ASSERT_NOT_EQUAL(0, t5ota(args));
    remove(img_path);
    remove(out_path);
    rmdir(dir);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bootctl_failed_boots);
    RUN_TEST(test_bootctl_requests);
    RUN_TEST(test_bootctl_full);
    RUN_TEST(test_target_slot);
    RUN_TEST(test_source_rule);
    RUN_TEST(test_version_rule);
    RUN_TEST(test_pick_release);
    RUN_TEST(test_ota_header_from_tool);
    return UNITY_END();
}
