/* SPDX-License-Identifier: Apache-2.0 */
#include "unity.h"
#include <string.h>
#include "t5_imu/t5_imu.h"
#include "t5_imu/t5_imu_test.h"
#include "t5_mocks.h"
#include "t5_pins/pins.h"

/* Simulated BMI270: the config upload, soft reset, and the feature
 * registers 0x30-0x3F, which show the page selected in 0x2F. */
#define FEAT_PAGES 8
static mock_i2c_dev_t *imu;
static size_t s_uploaded;
static bool s_fail_load;
static uint8_t s_page;
static uint8_t s_feat[FEAT_PAGES][16];

static bool in_features(uint32_t reg) { return reg >= 0x30 && reg < 0x40; }

static bool imu_write(mock_i2c_dev_t *d, uint32_t reg, const uint8_t *data, size_t len)
{
    if (reg == 0x5E) { s_uploaded += len; return true; }            /* INIT_DATA is a port */
    if (reg == 0x59 && data[0] == 1 && !s_fail_load && s_uploaded >= 8192) d->regs[0x21] = 0x01;
    if (reg == 0x7E && data[0] == 0xB6) { d->regs[0x21] = 0; s_uploaded = 0; return true; }
    if (reg == 0x2F) { s_page = data[0] % FEAT_PAGES; return true; }
    if (in_features(reg)) {
        for (size_t i = 0; i < len && in_features(reg + i); i++) s_feat[s_page][reg - 0x30 + i] = data[i];
        return true;
    }
    for (size_t i = 0; i < len; i++) d->regs[reg + i] = data[i];
    return true;
}

static bool imu_read(mock_i2c_dev_t *d, uint32_t reg, uint8_t *out, size_t len)
{
    (void)d;
    if (!in_features(reg)) return false;
    for (size_t i = 0; i < len; i++) out[i] = in_features(reg + i) ? s_feat[s_page][reg - 0x30 + i] : 0;
    return true;
}

void setUp(void)
{
    mock_reset_all();
    t5_imu_reset_for_test();
    imu = mock_i2c_add(T5_I2C_ADDR_BMI270, false);
    imu->regs[0x00] = 0x24;
    imu->on_write = imu_write;
    imu->on_read = imu_read;
    s_uploaded = 0;
    s_fail_load = false;
    s_page = 0;
    memset(s_feat, 0xA5, sizeof s_feat);
}
void tearDown(void) {}

static void test_init_uploads_config_and_reads_accel(void)
{
    TEST_ASSERT_EQUAL(T5_OK, t5_imu_init_blocking());
    TEST_ASSERT_EQUAL(T5_IMU_READY, t5_imu_status());
    TEST_ASSERT_EQUAL(1, t5_imu_attempts());
    TEST_ASSERT_EQUAL(8192, s_uploaded);
    TEST_ASSERT_EQUAL_HEX8(0x0C, imu->regs[0x7D]);   /* accel + temp */
    imu->regs[0x0C] = 0x00; imu->regs[0x0D] = 0x20;  /* x = 8192 = 1 g */
    imu->regs[0x10] = 0x00; imu->regs[0x11] = 0xE0;  /* z = -8192 */
    t5_imu_accel_t a;
    TEST_ASSERT_EQUAL(T5_OK, t5_imu_read_accel(&a));
    TEST_ASSERT_EQUAL(1000, a.x);
    TEST_ASSERT_EQUAL(-1000, a.z);
}

static void test_temp_invalid_sample(void)
{
    t5_imu_init_blocking();
    imu->regs[0x22] = 0x00; imu->regs[0x23] = 0x80;
    int32_t c;
    TEST_ASSERT_EQUAL(T5_ERR_STATE, t5_imu_read_temp(&c));
    imu->regs[0x22] = 0x00; imu->regs[0x23] = 0x02;  /* 512 LSB = +1 K */
    TEST_ASSERT_EQUAL(T5_OK, t5_imu_read_temp(&c));
    TEST_ASSERT_EQUAL(2400, c);
}

static void test_init_fails_after_retries_and_reports(void)
{
    s_fail_load = true;
    TEST_ASSERT_NOT_EQUAL(T5_OK, t5_imu_init_blocking());
    TEST_ASSERT_EQUAL(T5_IMU_FAILED, t5_imu_status());
    TEST_ASSERT_EQUAL(T5_IMU_INIT_ATTEMPTS, t5_imu_attempts());
    TEST_ASSERT_EQUAL(1, mock_events_count(T5_EV_IMU_FAILED));
    t5_imu_accel_t a;
    TEST_ASSERT_EQUAL(T5_ERR_STATE, t5_imu_read_accel(&a));
}

static void test_wrong_chip(void)
{
    imu->regs[0x00] = 0x26;
    TEST_ASSERT_EQUAL(T5_ERR_NOT_FOUND, t5_imu_init_blocking());
    TEST_ASSERT_EQUAL(T5_IMU_FAILED, t5_imu_status());
}

static void test_start_is_async(void)
{
    TEST_ASSERT_EQUAL(T5_OK, t5_imu_start());
    TEST_ASSERT_EQUAL(T5_IMU_STARTING, t5_imu_status());
    TEST_ASSERT_EQUAL(1, mock_tasks_count());
}

static void test_start_without_a_task_fails_and_reports(void)
{
    mock_tasks_fail(true);
    TEST_ASSERT_NOT_EQUAL(T5_OK, t5_imu_start());
    TEST_ASSERT_EQUAL(T5_IMU_FAILED, t5_imu_status());
    TEST_ASSERT_EQUAL(1, mock_events_count(T5_EV_IMU_FAILED));
}

static void test_no_bus_handle_fails_and_reports(void)
{
    for (int i = 0; i < T5_I2C_MAX_DEVICES; i++) {
        t5_i2c_dev_t *d;
        TEST_ASSERT_EQUAL(T5_OK, t5_i2c_open(0x50, 0, "filler", &d));
    }
    TEST_ASSERT_NOT_EQUAL(T5_OK, t5_imu_init_blocking());
    TEST_ASSERT_EQUAL(T5_IMU_FAILED, t5_imu_status());
    TEST_ASSERT_EQUAL(1, mock_events_count(T5_EV_IMU_FAILED));
}

static uint16_t any_motion_word(int i)
{
    const uint8_t *b = &s_feat[1][0x0C + 2 * i];   /* any-motion: page 1, offset 0x0C */
    return (uint16_t)(b[0] | b[1] << 8);
}

static void test_motion_config(void)
{
    t5_imu_init_blocking();
    t5_imu_motion_cfg_t c = { .threshold_mg = 100, .duration_ms = 100, .pins = T5_IMU_INT1,
                              .active_high = true, .latched = true };
    TEST_ASSERT_EQUAL(T5_OK, t5_imu_motion_enable(&c));
    TEST_ASSERT_EQUAL_HEX8(0x40, imu->regs[0x56]);
    TEST_ASSERT_EQUAL_HEX8(0x0A, imu->regs[0x53]);   /* output enable, active high, push-pull */
    TEST_ASSERT_EQUAL_HEX8(0x00, imu->regs[0x57]);
    TEST_ASSERT_EQUAL(5, any_motion_word(0) & 0x1FFF);      /* 100 ms in 20 ms steps */
    TEST_ASSERT_EQUAL_HEX16(0xE000, any_motion_word(0) & 0xE000);   /* x, y and z */
    TEST_ASSERT_EQUAL(205, any_motion_word(1) & 0x7FF);     /* 100 mg in 1/2048 g */
    TEST_ASSERT_TRUE(any_motion_word(1) & 0x8000);          /* enabled */
    TEST_ASSERT_EQUAL_HEX8(0xA5, s_feat[1][0x0B]);          /* the rest of the page kept */
    TEST_ASSERT_EQUAL_HEX8(0xA5, s_feat[0][0x0C]);          /* other pages untouched */
    TEST_ASSERT_EQUAL(0, s_page);                           /* page 0 selected again */
    TEST_ASSERT_EQUAL(T5_OK, t5_imu_motion_disable());
    TEST_ASSERT_EQUAL_HEX8(0x00, imu->regs[0x56]);
    TEST_ASSERT_FALSE(any_motion_word(1) & 0x8000);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_uploads_config_and_reads_accel);
    RUN_TEST(test_temp_invalid_sample);
    RUN_TEST(test_init_fails_after_retries_and_reports);
    RUN_TEST(test_wrong_chip);
    RUN_TEST(test_start_is_async);
    RUN_TEST(test_start_without_a_task_fails_and_reports);
    RUN_TEST(test_no_bus_handle_fails_and_reports);
    RUN_TEST(test_motion_config);
    return UNITY_END();
}
