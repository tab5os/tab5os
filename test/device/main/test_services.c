/* SPDX-License-Identifier: Apache-2.0
 * One group of tests per Phase 1 service, on real hardware.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"
#include "t5_bsp/xpin.h"
#include "t5_core/event.h"
#include "t5_core/os.h"
#include "t5_display/t5_display.h"
#include "t5_i2c/t5_i2c.h"
#include "t5_imu/t5_imu.h"
#include "t5_imu/t5_imu_test.h"
#include "t5_input/t5_touch.h"
#include "t5_input/t5_touch_test.h"
#include "t5_pins/pins.h"
#include "t5_storage/t5_storage.h"
#include "t5_time/rtc.h"
#include "t5_time/t5_time.h"
#include "t5_time/t5_time_test.h"

/* ---- core --------------------------------------------------------------- */
static volatile int s_ev_hits;
static void ev(t5_event_id_t id, const void *d, size_t l, void *a) { (void)id; (void)d; (void)l; (void)a; s_ev_hits++; }

TEST_CASE("events round-trip through the esp_event loop", "[core]")
{
    s_ev_hits = 0;
    TEST_ASSERT_EQUAL(T5_OK, t5_event_subscribe(T5_EV_TIME_SET, ev, NULL));
    TEST_ASSERT_EQUAL(T5_OK, t5_event_post(T5_EV_TIME_SET, NULL, 0));
    for (int i = 0; i < 50 && !s_ev_hits; i++) vTaskDelay(pdMS_TO_TICKS(10));
    TEST_ASSERT_EQUAL(1, s_ev_hits);
    t5_event_unsubscribe(T5_EV_TIME_SET, ev, NULL);
}

static void *volatile s_seen_ctx;
static void ctx_task(void *a) { (void)a; s_seen_ctx = t5_app_ctx_get(); }

TEST_CASE("tasks inherit the app context", "[core]")
{
    static int app;
    s_seen_ctx = NULL;
    t5_app_ctx_set(&app);
    TEST_ASSERT_EQUAL(T5_OK, t5_task_create("ctx", ctx_task, NULL, 2048, 5, T5_CORE_ANY, NULL));
    t5_app_ctx_set(NULL);
    for (int i = 0; i < 50 && !s_seen_ctx; i++) vTaskDelay(pdMS_TO_TICKS(10));
    TEST_ASSERT_EQUAL_PTR(&app, s_seen_ctx);
}

/* ---- i2c ---------------------------------------------------------------- */
TEST_CASE("every internal chip answers", "[i2c]")
{
    static const uint8_t want[] = { T5_I2C_ADDR_ES8388, T5_I2C_ADDR_RX8130, T5_I2C_ADDR_ES7210,
                                    T5_I2C_ADDR_INA226, T5_I2C_ADDR_EXP0, T5_I2C_ADDR_EXP1, T5_I2C_ADDR_BMI270 };
    for (size_t i = 0; i < sizeof want; i++) {
        char msg[16];
        snprintf(msg, sizeof msg, "0x%02x", want[i]);
        TEST_ASSERT_EQUAL_MESSAGE(T5_OK, t5_i2c_probe(want[i]), msg);
    }
}

TEST_CASE("bus recovery leaves devices working", "[i2c]")
{
    t5_i2c_dev_t *d;
    TEST_ASSERT_EQUAL(T5_OK, t5_i2c_open(T5_I2C_ADDR_INA226, T5_SYS_I2C_HZ, "test", &d));
    uint8_t id[2];
    for (int i = 0; i < 20; i++) {
        TEST_ASSERT_EQUAL(T5_OK, t5_i2c_recover());
        TEST_ASSERT_EQUAL(T5_OK, t5_i2c_read(d, 0xFE, id, 2));
        TEST_ASSERT_EQUAL_HEX8(0x54, id[0]);   /* "TI" */
        TEST_ASSERT_EQUAL_HEX8(0x49, id[1]);
    }
    t5_i2c_close(d);
}

typedef struct {
    uint8_t addr;
    volatile uint32_t ok, err;
    volatile bool run, done;
} hammer_t;

static void hammer(void *arg)
{
    hammer_t *h = arg;
    t5_i2c_dev_t *d;
    if (t5_i2c_open(h->addr, T5_SYS_I2C_HZ, "hammer", &d) != T5_OK) {
        h->err++;   /* counted as a failure, and the test waits on done */
        h->done = true;
        vTaskDelete(NULL);
    }
    uint8_t v;
    while (h->run) {
        if (t5_i2c_read_u8(d, 0x01, &v) == T5_OK) h->ok++; else h->err++;
    }
    t5_i2c_close(d);
    h->done = true;
    vTaskDelete(NULL);
}

TEST_CASE("bus lock is fair under full contention", "[i2c]")
{
    /* Three tasks on both cores keep the bus 100% busy. Before the fair
     * hand-off, one task could wait over a second for the lock. */
    t5_i2c_bus_stats_t b0, b1;
    t5_i2c_bus_stats(&b0);
    static hammer_t h[3] = { { .addr = T5_I2C_ADDR_EXP0 }, { .addr = T5_I2C_ADDR_EXP1 }, { .addr = T5_I2C_ADDR_INA226 } };
    for (int i = 0; i < 3; i++) {
        h[i].ok = h[i].err = 0;
        h[i].run = true;
        h[i].done = false;
        xTaskCreatePinnedToCore(hammer, "hammer", 3072, &h[i], 3, NULL, i % 2);
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
    for (int i = 0; i < 3; i++) h[i].run = false;
    for (int i = 0; i < 3; i++) while (!h[i].done) vTaskDelay(pdMS_TO_TICKS(5));
    t5_i2c_bus_stats(&b1);
    uint32_t total = h[0].ok + h[1].ok + h[2].ok;
    printf("hammer: %u / %u / %u ok, lock timeouts %u, hand-offs %u\n", (unsigned)h[0].ok,
           (unsigned)h[1].ok, (unsigned)h[2].ok, (unsigned)(b1.lock_timeouts - b0.lock_timeouts),
           (unsigned)t5_i2c_handoffs());
    TEST_ASSERT_EQUAL(0, b1.lock_timeouts - b0.lock_timeouts);
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL(0, h[i].err);
        TEST_ASSERT_GREATER_THAN_UINT32(total / 6, h[i].ok);   /* each gets at least half its fair share */
    }
}

/* ---- expanders ---------------------------------------------------------- */
TEST_CASE("expander output reads back from the chip", "[xpin]")
{
    const t5_xpin_t nc = T5_XPIN(T5_I2C_ADDR_EXP0, 3);   /* not connected on Tab5 */
    bool v;
    TEST_ASSERT_EQUAL(T5_OK, t5_xpin_output(nc, true));
    TEST_ASSERT_EQUAL(T5_OK, t5_xpin_read(nc, &v));
    TEST_ASSERT_TRUE(v);
    TEST_ASSERT_EQUAL(T5_OK, t5_xpin_output(nc, false));
    TEST_ASSERT_EQUAL(T5_OK, t5_xpin_read(nc, &v));
    TEST_ASSERT_FALSE(v);
    TEST_ASSERT_EQUAL(T5_OK, t5_xpin_input(nc, T5_XPIN_PULL_DOWN));
}

TEST_CASE("charging stays enabled after boot defaults", "[xpin]")
{
    bool v;
    TEST_ASSERT_TRUE(t5_xpin_out_level(T5_XP_CHG_EN));
    TEST_ASSERT_EQUAL(T5_OK, t5_xpin_read(T5_XP_CHG_EN, &v));
    TEST_ASSERT_TRUE(v);
}

/* ---- rtc ---------------------------------------------------------------- */
TEST_CASE("rtc write, read back, restore", "[rtc]")
{
    TEST_ASSERT_EQUAL(T5_OK, t5_time_init());
    struct tm before;
    bool was_valid;
    TEST_ASSERT_EQUAL(T5_OK, t5_rtc_read(&before, &was_valid));
    time_t saved = was_valid ? t5_rtc_tm_to_utc(&before) : 0;

    TEST_ASSERT_EQUAL(T5_OK, t5_time_set(1800000000, T5_TIME_SRC_USER));
    vTaskDelay(pdMS_TO_TICKS(1100));
    struct tm t;
    bool ok;
    TEST_ASSERT_EQUAL(T5_OK, t5_rtc_read(&t, &ok));
    TEST_ASSERT_TRUE(ok);
    int64_t d = (int64_t)t5_rtc_tm_to_utc(&t) - 1800000000;
    TEST_ASSERT_TRUE_MESSAGE(d >= 1 && d <= 2, "RTC not counting at 1 Hz");
    if (saved) t5_time_set(saved + 2, T5_TIME_SRC_USER);
}

TEST_CASE("rtc interrupts stay disabled while running", "[rtc]")
{
    /* An RTC /IRQ while running makes the power controller cut power (bench 0.7). */
    TEST_ASSERT_EQUAL(T5_OK, t5_time_init());
    bool on = true;
    TEST_ASSERT_EQUAL(T5_OK, t5_rtc_irqs_enabled_for_test(&on));
    TEST_ASSERT_FALSE(on);
}

/* ---- imu ---------------------------------------------------------------- */
TEST_CASE("imu reports about 1 g at rest", "[imu]")
{
    TEST_ASSERT_EQUAL(T5_OK, t5_imu_init_blocking());
    vTaskDelay(pdMS_TO_TICKS(100));
    t5_imu_accel_t a;
    TEST_ASSERT_EQUAL(T5_OK, t5_imu_read_accel(&a));
    int32_t m2 = a.x * a.x + a.y * a.y + a.z * a.z;
    TEST_ASSERT_INT32_WITHIN(250000, 1000000, m2);   /* 0.87 g .. 1.12 g */
    int32_t c;
    TEST_ASSERT_EQUAL(T5_OK, t5_imu_read_temp(&c));
    TEST_ASSERT_INT32_WITHIN(5000, 3000, c);         /* -20 .. 80 C */
}

/* ---- storage ------------------------------------------------------------ */
TEST_CASE("data and sd read back what was written", "[storage]")
{
    TEST_ASSERT_EQUAL(T5_OK, t5_storage_init());
    TEST_ASSERT_TRUE(t5_storage_data_mounted());
    const char msg[] = "tab5os device test";
    TEST_ASSERT_EQUAL(T5_OK, t5_storage_write_atomic(T5_DATA_PATH "/devtest.txt", msg, sizeof msg));
    char buf[64] = {0};
    FILE *f = fopen(T5_DATA_PATH "/devtest.txt", "rb");
    TEST_ASSERT_NOT_NULL(f);
    fread(buf, 1, sizeof buf, f);
    fclose(f);
    TEST_ASSERT_EQUAL_STRING(msg, buf);
    remove(T5_DATA_PATH "/devtest.txt");
    if (t5_storage_sd_state() == T5_SD_MOUNTED) {
        TEST_ASSERT_EQUAL(T5_OK, t5_storage_write_atomic(T5_SD_PATH "/devtest.txt", msg, sizeof msg));
        remove(T5_SD_PATH "/devtest.txt");
    } else {
        printf("(no SD card: SD part skipped)\n");
    }
}

/* ---- display and touch -------------------------------------------------- */
TEST_CASE("display answers, sleeps and wakes", "[display]")
{
    TEST_ASSERT_EQUAL(T5_OK, t5_display_init());
    uint8_t id[3], pm;
    TEST_ASSERT_EQUAL(T5_OK, t5_display_read_ids(id, &pm));
    TEST_ASSERT_EQUAL_HEX8(0x9C, pm);   /* booster, sleep out, display on */
    TEST_ASSERT_EQUAL(T5_OK, t5_display_sleep());
    TEST_ASSERT_EQUAL(T5_OK, t5_display_read_ids(id, &pm));
    TEST_ASSERT_EQUAL_HEX8(0, pm & 0x10);   /* sleep-in */
    TEST_ASSERT_EQUAL(T5_OK, t5_display_wake());
    TEST_ASSERT_EQUAL(T5_OK, t5_display_read_ids(id, &pm));
    TEST_ASSERT_EQUAL_HEX8(0x9C, pm);
}

TEST_CASE("touch controller samples with no finger", "[touch]")
{
    TEST_ASSERT_EQUAL(T5_OK, t5_touch_init());
    t5_touch_state_t s;
    TEST_ASSERT_EQUAL(T5_OK, t5_touch_sample(&s));
    TEST_ASSERT_EQUAL(0, s.count);
    t5_touch_info_t i;
    t5_touch_info(&i);
    TEST_ASSERT_EQUAL(T5_DISPLAY_W, i.max_x);
    TEST_ASSERT_EQUAL(T5_DISPLAY_H, i.max_y);
}
