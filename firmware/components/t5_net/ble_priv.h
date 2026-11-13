/* SPDX-License-Identifier: Apache-2.0
 * Inside t5_net's Bluetooth: ble.c (the stack, links, advertising) and
 * ble_hid.c (HID over GATT).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "host/ble_gap.h"
#include "t5_core/err.h"
#include "t5_core/os.h"

/* ble.c */
t5_mutex_t ble_mx(void);
bool ble_synced(void);
/* The system's advertiser (connectable, with an appearance and one 16-bit
 * service UUID): not an app's resource. T5_ERR_BUSY while an app
 * advertises; an app's t5_ble_adv_start is T5_ERR_BUSY meanwhile. */
t5_err_t ble_adv_sys_start(const char *name, uint16_t appearance, const char *uuid);
void ble_adv_sys_stop(void);

/* ble_hid.c */
/* At stack start, before sync: the HID, battery and device information
 * services, registered once so their handles never move under a host's
 * cache. NimBLE return code. */
int ble_hid_register(void);
/* ble.c's GAP callback for links centrals made to us, under ble_mx():
 * connect, disconnect, subscribe and encryption changes. */
void ble_hid_gap_locked(const struct ble_gap_event *e);
/* The stack went down: every host is gone. Under ble_mx(). */
void ble_hid_down_locked(void);
