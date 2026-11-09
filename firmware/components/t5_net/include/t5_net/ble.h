/* SPDX-License-Identifier: Apache-2.0
 * BLE central basics (plan 3.6) on NimBLE, with the controller on the C6
 * (HCI over the esp_hosted SDIO link). One connection at a time.
 *
 * BLE lives and dies with the C6: when t5_net powers the C6 down (Sleep with
 * no net lock, radio off, C6 reset) the stack stops and any connection is
 * lost (T5_EV_BLE_DISCONNECTED); it starts again when the C6 comes back.
 * While connected, BLE holds a T5_LOCK_NET wake lock so Sleep keeps the C6 up.
 * HID keyboards over BLE come later.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_BLE_MAX_SCAN 32
#define T5_BLE_MAX_CHARS 48
#define T5_BLE_NAME_LEN 30

typedef struct {
    uint8_t addr[6];          /* little-endian, as NimBLE stores it */
    uint8_t addr_type;
    int8_t rssi;
    char name[T5_BLE_NAME_LEN];
} t5_ble_dev_t;

typedef struct {
    char svc_uuid[40];
    char uuid[40];
    uint16_t val_handle;
    uint16_t def_handle;
    uint8_t props;            /* BLE_GATT_CHR_PROP_* */
} t5_ble_chr_t;

/* Turn BLE on or off (saved with the network settings). On needs the C6. */
t5_err_t t5_ble_enable(bool on);
bool t5_ble_ready(void);

t5_err_t t5_ble_scan(uint32_t ms);                 /* -> T5_EV_BLE_SCAN_DONE */
int t5_ble_scan_results(t5_ble_dev_t *out, int max);

t5_err_t t5_ble_connect(const uint8_t addr[6], uint8_t addr_type);   /* -> T5_EV_BLE_CONNECTED */
t5_err_t t5_ble_disconnect(void);
bool t5_ble_connected(void);

/* Discover every service and characteristic of the peer (blocks, up to 10 s). */
t5_err_t t5_ble_discover(void);
int t5_ble_chars(t5_ble_chr_t *out, int max);

t5_err_t t5_ble_read(uint16_t handle, uint8_t *buf, size_t cap, size_t *len);
t5_err_t t5_ble_write(uint16_t handle, const uint8_t *data, size_t len, bool response);
/* Enable notifications by writing the CCCD at val_handle + 1. */
t5_err_t t5_ble_subscribe(uint16_t val_handle, bool on);
typedef void (*t5_ble_notify_cb_t)(uint16_t handle, const uint8_t *data, size_t len, void *arg);
void t5_ble_set_notify_cb(t5_ble_notify_cb_t cb, void *arg);

/* Pure helpers (host-tested). */
bool t5_ble_parse_addr(const char *s, uint8_t out[6]);           /* "aa:bb:cc:dd:ee:ff" -> LE bytes */
void t5_ble_format_addr(const uint8_t addr[6], char out[18]);
/* Pull the device name out of advertising data (0x09 complete / 0x08 short). */
bool t5_ble_adv_name(const uint8_t *adv, size_t len, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
