/* SPDX-License-Identifier: Apache-2.0
 * Bluetooth LE for the OS (not in the SDK): the on/off setting, and the
 * scan and single connection Settings uses. Built on the same layer as the
 * app API (t5_net/ble.h), so they share the radio: a system scan while an
 * app scans gets T5_ERR_BUSY, and links count against T5_BLE_MAX_LINKS.
 *
 * BLE lives and dies with the C6. t5_net runs the C6 while Wi-Fi or
 * Bluetooth is on (Bluetooth alone starts it without the Wi-Fi driver).
 * When it powers the C6 down (Sleep with no net lock, both off, a C6
 * reset) the stack stops and links
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

/* Pairing (any link): the answer to T5_EV_BLE_PAIRING action 1, after
 * the person compared the codes. */
t5_err_t t5_ble_sys_pair_reply(uint16_t conn, bool accept);
/* Bonded devices (kept across reboots, 16 at most: the least recently
 * paired goes first) and forgetting one. The other device keeps its side:
 * remove the Tab5 there too. */
typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
} t5_ble_bond_t;
int t5_ble_sys_bonds(t5_ble_bond_t *out, int max);
t5_err_t t5_ble_sys_forget(const uint8_t addr[6], uint8_t addr_type);

#ifdef __cplusplus
}
#endif
