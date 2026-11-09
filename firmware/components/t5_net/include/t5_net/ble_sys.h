/* SPDX-License-Identifier: Apache-2.0
 * Bluetooth LE for the OS (not in the SDK): the on/off setting, and the
 * scan and single connection Settings uses. Built on the same layer as the
 * app API (t5_net/ble.h), so they share the radio: a system scan while an
 * app scans gets T5_ERR_BUSY, and links count against T5_BLE_MAX_LINKS.
 *
 * BLE lives and dies with the C6: when t5_net powers it down (Sleep with no
 * net lock, Wi-Fi or Bluetooth off, a C6 reset) the stack stops and links
 * drop (T5_EV_BLE_DISCONNECTED); it starts again when the C6 comes back.
 * While a link is up BLE holds a T5_LOCK_NET wake lock so Sleep keeps the
 * C6 up.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "t5_core/err.h"
#include "t5_net/ble.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_BLE_MAX_SCAN 32

/* Turn BLE on or off (saved with the network settings). On needs the C6. */
t5_err_t t5_ble_enable(bool on);
bool t5_ble_enabled(void);
/* Links up now, every app's and the system's (the status bar). */
int t5_ble_links(void);

/* Settings: a scan that keeps one entry per device (-> T5_EV_BLE_SCAN_DONE). */
typedef struct {
    uint8_t addr[6];          /* LSB first */
    uint8_t addr_type;
    int8_t rssi;
    char name[T5_BLE_NAME_LEN];
} t5_ble_dev_t;
t5_err_t t5_ble_sys_scan(uint32_t ms);
int t5_ble_sys_scan_results(t5_ble_dev_t *out, int max);
/* The system's one link (-> T5_EV_BLE_CONNECTED / _DISCONNECTED). */
t5_err_t t5_ble_sys_connect(const uint8_t addr[6], uint8_t addr_type);
t5_err_t t5_ble_sys_disconnect(void);
bool t5_ble_sys_connected(void);

#ifdef __cplusplus
}
#endif
