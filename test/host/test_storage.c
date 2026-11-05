/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "unity.h"
#include "t5_mocks.h"
#include "t5_storage/t5_storage.h"

static char s_dir[256];

void setUp(void)
{
    mock_reset_all();
    t5_storage_reset_for_test();
    mock_sd = (mock_sd_t){ .mount_result = T5_ERR_NOT_FOUND };
    snprintf(s_dir, sizeof s_dir, "/tmp/t5_storage_test_%d", (int)getpid());
    mkdir(s_dir, 0755);
}
void tearDown(void) {}

static void test_atomic_write_replaces_whole_file(void)
{
    char p[300];
    snprintf(p, sizeof p, "%s/settings.bin", s_dir);
    TEST_ASSERT_EQUAL(T5_OK, t5_storage_write_atomic(p, "old contents", 12));
    TEST_ASSERT_EQUAL(T5_OK, t5_storage_write_atomic(p, "new", 3));
    char buf[32] = {0};
    FILE *f = fopen(p, "rb");
    TEST_ASSERT_NOT_NULL(f);
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_EQUAL_STRING("new", buf);
    char tmp[310];
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    TEST_ASSERT_EQUAL(-1, access(tmp, F_OK));   /* no temp file left behind */
}

static void test_atomic_write_bad_dir(void)
{
    TEST_ASSERT_EQUAL(T5_ERR_NOT_FOUND, t5_storage_write_atomic("/nonexistent_t5/x", "a", 1));
}

static void test_sd_insert_remove_cycle(void)
{
    t5_storage_sd_poll(0);
    TEST_ASSERT_EQUAL(T5_SD_ABSENT, t5_storage_sd_state());
    mock_sd.mount_result = T5_OK;
    t5_storage_sd_poll(1000);                    /* still inside the retry interval */
    TEST_ASSERT_EQUAL(T5_SD_ABSENT, t5_storage_sd_state());
    t5_storage_sd_poll(T5_SD_RETRY_MS);
    TEST_ASSERT_EQUAL(T5_SD_MOUNTED, t5_storage_sd_state());
    TEST_ASSERT_EQUAL(1, mock_events_count(T5_EV_SD_INSERTED));
    mock_sd.alive = false;                        /* card pulled */
    t5_storage_sd_poll(T5_SD_RETRY_MS + 1000);
    TEST_ASSERT_EQUAL(T5_SD_MOUNTED, t5_storage_sd_state());   /* one miss is tolerated */
    t5_storage_sd_poll(T5_SD_RETRY_MS + 2000);
    TEST_ASSERT_EQUAL(T5_SD_ABSENT, t5_storage_sd_state());
    TEST_ASSERT_EQUAL(1, mock_sd.unmounts);
    TEST_ASSERT_EQUAL(1, mock_events_count(T5_EV_SD_REMOVED));
}

static void test_sd_bad_card_backs_off(void)
{
    mock_sd.mount_result = T5_ERR_IO;
    t5_storage_sd_poll(0);
    TEST_ASSERT_EQUAL(T5_SD_FAILED, t5_storage_sd_state());
    TEST_ASSERT_EQUAL(1, mock_events_count(T5_EV_SD_MOUNT_FAILED));
    t5_storage_sd_poll(T5_SD_RETRY_MS);           /* longer back-off for a bad card */
    TEST_ASSERT_EQUAL(1, mock_events_count(T5_EV_SD_MOUNT_FAILED));
    mock_sd.mount_result = T5_OK;
    t5_storage_sd_poll(T5_SD_FAILED_RETRY_MS);
    TEST_ASSERT_EQUAL(T5_SD_MOUNTED, t5_storage_sd_state());
}

static int s_synced;
static t5_err_t sync_hook(void *arg) { (void)arg; s_synced++; return T5_OK; }

static void test_eject_syncs_and_stays_ejected(void)
{
    s_synced = 0;
    t5_storage_register_sync(sync_hook, NULL);
    mock_sd.mount_result = T5_OK;
    t5_storage_sd_poll(0);
    TEST_ASSERT_EQUAL(T5_OK, t5_storage_sd_eject());
    TEST_ASSERT_EQUAL(1, s_synced);
    TEST_ASSERT_EQUAL(T5_SD_EJECTED, t5_storage_sd_state());
    t5_storage_sd_poll(100000);                   /* poller leaves an ejected card alone */
    TEST_ASSERT_EQUAL(T5_SD_EJECTED, t5_storage_sd_state());
    TEST_ASSERT_EQUAL(1, mock_sd.mounts);
    TEST_ASSERT_EQUAL(T5_OK, t5_storage_sd_rescan());
    TEST_ASSERT_EQUAL(T5_SD_MOUNTED, t5_storage_sd_state());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_atomic_write_replaces_whole_file);
    RUN_TEST(test_atomic_write_bad_dir);
    RUN_TEST(test_sd_insert_remove_cycle);
    RUN_TEST(test_sd_bad_card_backs_off);
    RUN_TEST(test_eject_syncs_and_stays_ejected);
    return UNITY_END();
}
