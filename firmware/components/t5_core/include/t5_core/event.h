/* SPDX-License-Identifier: Apache-2.0
 * System events: one loop, typed IDs, small payloads copied on post.
 * Device: an esp_event loop with its own task on core 0. Host: synchronous.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    T5_EV_ANY = -1,

    /* I2C (payload: t5_ev_i2c_t) */
    T5_EV_I2C_DEVICE_FAULT = 100,   /* a device crossed the failure threshold */
    T5_EV_I2C_DEVICE_OK,            /* a faulted device answered again */
    T5_EV_I2C_BUS_RECOVERED,        /* bus clear + controller reset done */

    /* Storage (payload: none) */
    T5_EV_SD_INSERTED = 200,
    T5_EV_SD_REMOVED,
    T5_EV_SD_MOUNT_FAILED,

    /* Time (payload: none) */
    T5_EV_TIME_SET = 300,           /* wall clock set by user, NTP or RTC */
    T5_EV_RTC_ALARM,

    /* Sensors */
    T5_EV_IMU_FAILED = 400,         /* init gave up after retries */
    T5_EV_IMU_MOTION,

    /* Display and touch */
    T5_EV_DISPLAY_READY = 500,
    T5_EV_TOUCH_FAILED,
} t5_event_id_t;

typedef struct {
    uint8_t addr;
    int32_t err;      /* t5_err_t of the last failure */
    uint32_t errors;  /* total failures on this device */
} t5_ev_i2c_t;

#define T5_EVENT_MAX_DATA 32

typedef void (*t5_event_handler_t)(t5_event_id_t id, const void *data, size_t len, void *arg);

t5_err_t t5_event_init(void);
/* Copies up to T5_EVENT_MAX_DATA bytes. Safe from any task, not from ISRs. */
t5_err_t t5_event_post(t5_event_id_t id, const void *data, size_t len);
t5_err_t t5_event_subscribe(t5_event_id_t id, t5_event_handler_t fn, void *arg);
t5_err_t t5_event_unsubscribe(t5_event_id_t id, t5_event_handler_t fn, void *arg);
const char *t5_event_name(t5_event_id_t id);

#ifdef __cplusplus
}
#endif
