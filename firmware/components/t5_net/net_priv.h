/* SPDX-License-Identifier: Apache-2.0
 * Shared inside t5_net: net.c, ble.c and sock.c.
 *
 * t5_net owns NVS: the Wi-Fi driver and the Bluetooth stack keep their own
 * data there too, so it must be up before either starts, and no other
 * service uses it. Wi-Fi and Bluetooth settings share one namespace.
 */
#pragma once
#include <stdbool.h>
#include "nvs.h"
#include "t5_core/err.h"

#define NET_NVS_NS "t5net"

/* NVS up (once; a full or newer-format partition is erased first), then
 * nvs_open of NET_NVS_NS. */
esp_err_t net_nvs_open(nvs_open_mode_t mode, nvs_handle_t *h);
/* Pause the station (disconnect, no auto-reconnect) while an app's Wi-Fi
 * monitor changes channels, and bring it back after (wifimon.c). */
t5_err_t net_station_hold(bool on);
/* The Wi-Fi driver is running (the C6 is up with Wi-Fi on). */
bool net_wifi_up(void);

/* wifimon.c: an open monitor ends (exclusive Wi-Fi is being claimed or given back). */
void wifimon_end_for_excl(void);
/* wifi_excl.c: registers the exclusive Wi-Fi provider (t5_net_init). */
void wifi_excl_init(void);

/* Ask the net task to start or stop BLE to match its setting. */
void net_ble_changed(void);
/* ble.c, on the net task: the C6 link came up or is going down, or the
 * enable setting changed (up = link state). */
void t5_ble_on_link(bool up);
/* sock.c: how many handles are open. While any is, the stack holds a NET
 * wake lock ("net.io"), so Sleep keeps Wi-Fi connected. */
