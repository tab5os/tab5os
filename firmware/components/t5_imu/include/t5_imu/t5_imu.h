/* SPDX-License-Identifier: Apache-2.0
 * BMI270 accelerometer service (0x68).
 *
 * The BMI270 needs an 8 KB config upload after every power loss, and it
 * sometimes fails to come up after a brown-out. t5_imu_start() runs init in
 * a background task with reset and retry; boot never waits on it. On final
 * failure it posts T5_EV_IMU_FAILED and t5_imu_status() says FAILED.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    T5_IMU_OFF,
    T5_IMU_STARTING,
    T5_IMU_READY,
    T5_IMU_FAILED,
} t5_imu_status_t;

typedef struct {
    int16_t x, y, z;   /* milli-g, chip axes */
} t5_imu_accel_t;

#define T5_IMU_INIT_ATTEMPTS 3

/* Start init in the background. */
t5_err_t t5_imu_start(void);
t5_imu_status_t t5_imu_status(void);
int t5_imu_attempts(void);

t5_err_t t5_imu_read_accel(t5_imu_accel_t *out);
/* Die temperature in centi-degrees C. */
t5_err_t t5_imu_read_temp(int32_t *centi_c);

/* Low-rate stream: calls cb from a service task at hz (1..100). hz 0 stops. */
typedef void (*t5_imu_cb_t)(const t5_imu_accel_t *a, void *arg);
t5_err_t t5_imu_stream(uint32_t hz, t5_imu_cb_t cb, void *arg);

/* Any-motion interrupt on the chip's INT1/INT2 pins. */
#define T5_IMU_INT1 0x01
#define T5_IMU_INT2 0x02
typedef struct {
    uint16_t threshold_mg;   /* 1..1000 */
    uint16_t duration_ms;    /* 20..10000, rounded to 20 ms */
    uint8_t pins;            /* T5_IMU_INT1 | T5_IMU_INT2 */
    bool active_high;
    bool open_drain;
    bool latched;            /* stays asserted until t5_imu_motion_status() reads it */
} t5_imu_motion_cfg_t;

t5_err_t t5_imu_motion_enable(const t5_imu_motion_cfg_t *cfg);
t5_err_t t5_imu_motion_disable(void);
/* Reads (and so clears, if latched) the feature interrupt status. */
t5_err_t t5_imu_motion_status(bool *moved);

#ifdef __cplusplus
}
#endif
