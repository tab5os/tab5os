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

    /* Power (payload: t5_ev_power_t) */
    T5_EV_POWER_STATE = 600,        /* state changed */
    T5_EV_POWER_SUSPEND_OVERRUN,    /* a suspend/resume hook ran over budget (payload: none) */
    T5_EV_POWER_SHUTDOWN,           /* soft off is about to cut power (payload: none) */

    /* Battery (payload: t5_ev_battery_t) */
    T5_EV_BATTERY_UPDATE = 650,     /* every gauge update that changed the whole percent */
    T5_EV_BATTERY_LOW,              /* 15%: warn once */
    T5_EV_BATTERY_LOW_POWER,        /* 10%: low-power mode on */
    T5_EV_BATTERY_LOW_POWER_OFF,    /* low-power mode off again (charging or recovered) */
    T5_EV_BATTERY_CRITICAL,         /* 5% / 6.5 V / 6.3 V: soft off follows */
    T5_EV_CHARGE_STATE,             /* charging, full, paused... changed */
    T5_EV_USB_POWER,                /* USB power plugged or unplugged */
    T5_EV_BATTERY_SWAPPED,          /* pack swap detected at boot */

    /* Network (payload: t5_ev_net_t) */
    T5_EV_NET_C6_UP = 700,          /* host link to the C6 is up */
    T5_EV_NET_C6_DOWN,              /* link lost or C6 powered off */
    T5_EV_NET_SCAN_DONE,
    T5_EV_NET_CONNECTED,            /* associated with an AP */
    T5_EV_NET_GOT_IP,
    T5_EV_NET_DISCONNECTED,
    T5_EV_NET_WAKE,                 /* the C6 raised its wake line (G14) */
    T5_EV_NET_C6_UPDATE,            /* C6 firmware update finished (err in payload) */

    /* Bluetooth (payload: t5_ev_net_t) */
    T5_EV_BLE_READY = 800,
    T5_EV_BLE_SCAN_DONE,
    T5_EV_BLE_CONNECTED,
    T5_EV_BLE_DISCONNECTED,
} t5_event_id_t;

typedef struct {
    uint8_t addr;
    int32_t err;      /* t5_err_t of the last failure */
    uint32_t errors;  /* total failures on this device */
} t5_ev_i2c_t;

typedef struct {
    uint8_t from;     /* t5_power_state_t */
    uint8_t to;
    uint8_t cause;    /* t5_power_cause_t */
} t5_ev_power_t;

typedef struct {
    int16_t percent;      /* 0..100, -1 unknown */
    uint16_t mv;          /* pack voltage */
    int16_t ma;           /* + discharging, - charging */
    uint8_t charge;       /* t5_charge_state_t */
    uint8_t usb;          /* USB power present */
} t5_ev_battery_t;

typedef struct {
    int32_t err;          /* t5_err_t, or a reason code for disconnects */
    int8_t rssi;
    uint8_t count;        /* scan results */
    uint8_t ip[4];
} t5_ev_net_t;

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
