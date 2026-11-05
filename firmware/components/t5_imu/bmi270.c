/* SPDX-License-Identifier: Apache-2.0
 * BMI270 driver. Register map and feature offsets from Bosch's BMI270_SensorAPI.
 */
#include <string.h>
#include "t5_core/event.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_i2c/t5_i2c.h"
#include "t5_imu/t5_imu.h"
#include "t5_pins/pins.h"

static const char *TAG = "t5.imu";

enum {
    REG_CHIP_ID = 0x00,
    REG_ERR = 0x02,
    REG_ACC_X = 0x0C,
    REG_INT_STATUS_0 = 0x1C,    /* bit6: any-motion */
    REG_INTERNAL_STATUS = 0x21, /* [3:0] 1 = init ok */
    REG_TEMP = 0x22,
    REG_FEAT_PAGE = 0x2F,
    REG_FEATURES = 0x30,        /* 16 bytes of the selected page */
    REG_ACC_CONF = 0x40,
    REG_ACC_RANGE = 0x41,
    REG_INT1_IO_CTRL = 0x53,
    REG_INT2_IO_CTRL = 0x54,
    REG_INT_LATCH = 0x55,
    REG_INT1_MAP_FEAT = 0x56,
    REG_INT2_MAP_FEAT = 0x57,
    REG_INIT_CTRL = 0x59,
    REG_INIT_ADDR_0 = 0x5B,
    REG_INIT_DATA = 0x5E,
    REG_PWR_CONF = 0x7C,
    REG_PWR_CTRL = 0x7D,
    REG_CMD = 0x7E,
};

#define CHIP_ID          0x24
#define CMD_SOFT_RESET   0xB6
#define ANY_MOT_OFFSET   0x0C   /* in feature page 1 */
#define ANY_MOT_INT_BIT  0x40
#define ACC_CONF_50HZ    0xA7   /* perf mode, normal filter (avg4), 50 Hz */
#define ACC_RANGE_4G     0x01
#define UPLOAD_CHUNK     128

extern const uint8_t t5_bmi270_config[];
extern const size_t t5_bmi270_config_len;

static t5_i2c_dev_t *s_dev;
static volatile t5_imu_status_t s_status = T5_IMU_OFF;
static int s_attempts;
static t5_mutex_t s_lock;

static t5_imu_cb_t s_cb;
static void *s_cb_arg;
static volatile uint32_t s_stream_hz;
static bool s_stream_task;

int16_t t5_imu_raw_to_mg(int16_t raw)
{
    return (int16_t)(((int32_t)raw * 1000) / 8192);
}

void t5_imu_pack_any_motion(uint16_t threshold_mg, uint16_t duration_ms, bool enable, uint8_t out[4])
{
    /* duration: 13 bits in 20 ms steps; threshold: 11 bits, 1 LSB = 1/2048 g. */
    uint32_t dur = duration_ms / 20;
    if (dur < 1) dur = 1;
    if (dur > 0x1FFF) dur = 0x1FFF;
    uint32_t thr = ((uint32_t)threshold_mg * 2048 + 500) / 1000;
    if (thr < 1) thr = 1;
    if (thr > 0x7FF) thr = 0x7FF;
    uint16_t w0 = (uint16_t)(dur | 0xE000);              /* x, y, z selected */
    uint16_t w1 = (uint16_t)(thr | (enable ? 0x8000 : 0)); /* bit15 = enable */
    out[0] = (uint8_t)w0;
    out[1] = (uint8_t)(w0 >> 8);
    out[2] = (uint8_t)w1;
    out[3] = (uint8_t)(w1 >> 8);
}

static t5_err_t init_once(void)
{
    uint8_t id = 0;
    t5_err_t err = t5_i2c_read_u8(s_dev, REG_CHIP_ID, &id);
    if (err != T5_OK) {
        return err;
    }
    if (id != CHIP_ID) {
        T5_LOGE(TAG, "chip id 0x%02x, want 0x%02x", id, CHIP_ID);
        return T5_ERR_NOT_FOUND;
    }
    T5_RETURN_ON_ERR(t5_i2c_write_u8(s_dev, REG_CMD, CMD_SOFT_RESET));
    t5_delay_ms(5);
    T5_RETURN_ON_ERR(t5_i2c_write_u8(s_dev, REG_PWR_CONF, 0x00));   /* advanced power save off */
    t5_delay_ms(2);
    T5_RETURN_ON_ERR(t5_i2c_write_u8(s_dev, REG_INIT_CTRL, 0x00));
    for (size_t i = 0; i < t5_bmi270_config_len; i += UPLOAD_CHUNK) {
        size_t n = t5_bmi270_config_len - i < UPLOAD_CHUNK ? t5_bmi270_config_len - i : UPLOAD_CHUNK;
        const uint8_t addr[2] = { (uint8_t)((i / 2) & 0x0F), (uint8_t)((i / 2) >> 4) };
        T5_RETURN_ON_ERR(t5_i2c_write(s_dev, REG_INIT_ADDR_0, addr, 2));
        T5_RETURN_ON_ERR(t5_i2c_write(s_dev, REG_INIT_DATA, &t5_bmi270_config[i], n));
    }
    T5_RETURN_ON_ERR(t5_i2c_write_u8(s_dev, REG_INIT_CTRL, 0x01));
    uint8_t st = 0;
    for (int i = 0; i < 30; i++) {
        t5_delay_ms(5);
        if (t5_i2c_read_u8(s_dev, REG_INTERNAL_STATUS, &st) == T5_OK && (st & 0x0F) == 0x01) {
            break;
        }
    }
    if ((st & 0x0F) != 0x01) {
        T5_LOGE(TAG, "config load failed, internal status 0x%02x", st);
        return T5_ERR_IO;
    }
    T5_RETURN_ON_ERR(t5_i2c_write_u8(s_dev, REG_ACC_CONF, ACC_CONF_50HZ));
    T5_RETURN_ON_ERR(t5_i2c_write_u8(s_dev, REG_ACC_RANGE, ACC_RANGE_4G));
    T5_RETURN_ON_ERR(t5_i2c_write_u8(s_dev, REG_PWR_CTRL, 0x0C));   /* accel + temperature, gyro off */
    return T5_OK;
}

t5_err_t t5_imu_init_blocking(void)
{
    if (!s_lock) {
        s_lock = t5_mutex_create();
    }
    if (!s_dev) {
        T5_RETURN_ON_ERR(t5_i2c_open(T5_I2C_ADDR_BMI270, 400000, "imu", &s_dev));
    }
    s_status = T5_IMU_STARTING;
    t5_err_t err = T5_ERR_FAIL;
    for (s_attempts = 1; s_attempts <= T5_IMU_INIT_ATTEMPTS; s_attempts++) {
        err = init_once();
        if (err == T5_OK) {
            break;
        }
        T5_LOGW(TAG, "init attempt %d failed: %s", s_attempts, t5_err_str(err));
        t5_delay_ms(50);
    }
    if (err == T5_OK) {
        s_status = T5_IMU_READY;
        T5_LOGI(TAG, "BMI270 ready (attempt %d)", s_attempts);
    } else {
        s_attempts = T5_IMU_INIT_ATTEMPTS;
        s_status = T5_IMU_FAILED;
        T5_LOGE(TAG, "BMI270 failed after %d attempts", T5_IMU_INIT_ATTEMPTS);
        int32_t e = err;
        t5_event_post(T5_EV_IMU_FAILED, &e, sizeof e);
    }
    return err;
}

static void init_task(void *arg)
{
    (void)arg;
    t5_imu_init_blocking();
}

t5_err_t t5_imu_start(void)
{
    if (s_status == T5_IMU_STARTING || s_status == T5_IMU_READY) {
        return T5_OK;
    }
    s_status = T5_IMU_STARTING;
    return t5_task_create("t5_imu_init", init_task, NULL, 4096, 5, 0, NULL);
}

t5_imu_status_t t5_imu_status(void)
{
    return s_status;
}

int t5_imu_attempts(void)
{
    return s_attempts;
}

t5_imu_status_t t5_imu_wait(uint32_t ms)
{
    uint32_t start = t5_time_ms();
    while (s_status == T5_IMU_STARTING && t5_time_ms() - start < ms) {
        t5_delay_ms(10);
    }
    return s_status;
}

t5_err_t t5_imu_read_accel(t5_imu_accel_t *out)
{
    if (s_status != T5_IMU_READY) {
        return T5_ERR_STATE;
    }
    uint8_t b[6];
    T5_RETURN_ON_ERR(t5_i2c_read(s_dev, REG_ACC_X, b, sizeof b));
    out->x = t5_imu_raw_to_mg((int16_t)(b[0] | (b[1] << 8)));
    out->y = t5_imu_raw_to_mg((int16_t)(b[2] | (b[3] << 8)));
    out->z = t5_imu_raw_to_mg((int16_t)(b[4] | (b[5] << 8)));
    return T5_OK;
}

t5_err_t t5_imu_read_temp(int32_t *centi_c)
{
    if (s_status != T5_IMU_READY) {
        return T5_ERR_STATE;
    }
    uint8_t b[2];
    T5_RETURN_ON_ERR(t5_i2c_read(s_dev, REG_TEMP, b, 2));
    int16_t raw = (int16_t)(b[0] | (b[1] << 8));
    if (raw == INT16_MIN) {
        return T5_ERR_STATE;   /* 0x8000: no valid sample (sensor off or not ready) */
    }
    /* 0x0000 = 23 C, 1/512 K per LSB */
    *centi_c = 2300 + ((int32_t)raw * 100) / 512;
    return T5_OK;
}

static void stream_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t hz = s_stream_hz;
        if (!hz) {
            t5_delay_ms(100);
            continue;
        }
        t5_imu_accel_t a;
        if (s_cb && t5_imu_read_accel(&a) == T5_OK) {
            s_cb(&a, s_cb_arg);
        }
        t5_delay_ms(1000 / hz);
    }
}

t5_err_t t5_imu_stream(uint32_t hz, t5_imu_cb_t cb, void *arg)
{
    if (hz > 100 || (hz && !cb)) {
        return T5_ERR_INVALID_ARG;
    }
    s_stream_hz = 0;
    s_cb = cb;
    s_cb_arg = arg;
    if (hz && !s_stream_task) {
        T5_RETURN_ON_ERR(t5_task_create("t5_imu", stream_task, NULL, 3072, 4, 0, NULL));
        s_stream_task = true;
    }
    s_stream_hz = hz;
    return T5_OK;
}

static t5_err_t write_any_motion(const uint8_t block[4])
{
    uint8_t page[16];
    T5_RETURN_ON_ERR(t5_i2c_write_u8(s_dev, REG_FEAT_PAGE, 1));
    T5_RETURN_ON_ERR(t5_i2c_read(s_dev, REG_FEATURES, page, sizeof page));
    memcpy(&page[ANY_MOT_OFFSET], block, 4);
    T5_RETURN_ON_ERR(t5_i2c_write(s_dev, REG_FEATURES, page, sizeof page));
    return t5_i2c_write_u8(s_dev, REG_FEAT_PAGE, 0);
}

t5_err_t t5_imu_motion_enable(const t5_imu_motion_cfg_t *cfg)
{
    if (s_status != T5_IMU_READY) {
        return T5_ERR_STATE;
    }
    if (!cfg || !cfg->pins || cfg->threshold_mg == 0 || cfg->threshold_mg > 1000) {
        return T5_ERR_INVALID_ARG;
    }
    uint8_t block[4];
    t5_imu_pack_any_motion(cfg->threshold_mg, cfg->duration_ms, true, block);
    uint8_t io = (uint8_t)(0x08 | (cfg->open_drain ? 0x04 : 0) | (cfg->active_high ? 0x02 : 0));
    T5_RETURN_ON_ERR(t5_i2c_lock(1000));
    t5_err_t err = write_any_motion(block);
    if (err == T5_OK) err = t5_i2c_write_u8(s_dev, REG_INT_LATCH, cfg->latched ? 1 : 0);
    if (err == T5_OK) err = t5_i2c_write_u8(s_dev, REG_INT1_IO_CTRL, (cfg->pins & T5_IMU_INT1) ? io : 0);
    if (err == T5_OK) err = t5_i2c_write_u8(s_dev, REG_INT2_IO_CTRL, (cfg->pins & T5_IMU_INT2) ? io : 0);
    if (err == T5_OK) err = t5_i2c_write_u8(s_dev, REG_INT1_MAP_FEAT, (cfg->pins & T5_IMU_INT1) ? ANY_MOT_INT_BIT : 0);
    if (err == T5_OK) err = t5_i2c_write_u8(s_dev, REG_INT2_MAP_FEAT, (cfg->pins & T5_IMU_INT2) ? ANY_MOT_INT_BIT : 0);
    t5_i2c_unlock();
    return err;
}

t5_err_t t5_imu_motion_disable(void)
{
    if (s_status != T5_IMU_READY) {
        return T5_ERR_STATE;
    }
    uint8_t block[4];
    t5_imu_pack_any_motion(100, 100, false, block);
    T5_RETURN_ON_ERR(t5_i2c_lock(1000));
    t5_err_t err = write_any_motion(block);
    if (err == T5_OK) err = t5_i2c_write_u8(s_dev, REG_INT1_MAP_FEAT, 0);
    if (err == T5_OK) err = t5_i2c_write_u8(s_dev, REG_INT2_MAP_FEAT, 0);
    if (err == T5_OK) err = t5_i2c_write_u8(s_dev, REG_INT1_IO_CTRL, 0);
    if (err == T5_OK) err = t5_i2c_write_u8(s_dev, REG_INT2_IO_CTRL, 0);
    t5_i2c_unlock();
    return err;
}

t5_err_t t5_imu_motion_status(bool *moved)
{
    if (s_status != T5_IMU_READY || !moved) {
        return T5_ERR_STATE;
    }
    uint8_t st;
    T5_RETURN_ON_ERR(t5_i2c_read_u8(s_dev, REG_INT_STATUS_0, &st));
    *moved = st & ANY_MOT_INT_BIT;
    return T5_OK;
}

void t5_imu_reset_for_test(void)
{
    s_dev = NULL;
    s_status = T5_IMU_OFF;
    s_attempts = 0;
}
