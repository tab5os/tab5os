/* SPDX-License-Identifier: Apache-2.0
 * Network service (plan Phase 3): Wi-Fi through the ESP32-C6 over SDIO
 * (esp_hosted), saved networks, SNTP into the RTC, C6 health and updates,
 * and the radio's place in the power states.
 *
 * Everything runs on one service task, so calls here only queue work and
 * return; results arrive as T5_EV_NET_* events.
 *
 * Sleep: with a T5_LOCK_NET wake lock held, Wi-Fi stays associated in modem
 * power save and the SDIO link stays up (the P4 never stops in Sleep, see
 * t5_power). Without one, the C6 is powered off (WLAN_PWR_EN) and comes
 * back, reconnecting, on resume.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    T5_NET_OFF,           /* radio off (user, or Sleep without a net lock) */
    T5_NET_STARTING,      /* C6 powering up, link coming up */
    T5_NET_IDLE,          /* link up, not associated */
    T5_NET_CONNECTING,
    T5_NET_CONNECTED,     /* associated and has an address */
    T5_NET_FAILED,        /* the C6 didn't answer; retried with backoff */
} t5_net_state_t;

const char *t5_net_state_name(t5_net_state_t s);

#define T5_NET_SSID_LEN 33
#define T5_NET_PASS_LEN 65
#define T5_NET_MAX_SCAN 24

typedef struct {
    char ssid[T5_NET_SSID_LEN];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
    uint8_t auth;         /* wifi_auth_mode_t */
    bool saved;
} t5_net_ap_t;

typedef struct {
    t5_net_state_t state;
    bool enabled;
    char ssid[T5_NET_SSID_LEN];
    int8_t rssi;
    uint8_t channel;
    uint8_t ip[4], gw[4], mask[4], dns[4];
    bool power_save;
    bool sntp_synced;
    char c6_version[32];      /* firmware running on the C6 */
    char c6_image[32];        /* firmware in the c6fw partition, "" if none */
    uint32_t link_up_ms;      /* last C6 power-on to link up */
    uint32_t connect_ms;      /* last connect request to IP */
    uint32_t link_ups, c6_resets, health_failures;
    uint32_t disconnects, connects;
    uint32_t wakes;           /* C6 wake-line edges (G14) */
} t5_net_status_t;

/* Load saved networks, register the suspend hook, start the service task,
 * and (if the radio is enabled) power the C6 and auto-connect. */
t5_err_t t5_net_init(void);
void t5_net_status(t5_net_status_t *out);

/* Radio on or off ("airplane mode"); saved. */
t5_err_t t5_net_enable(bool on);

t5_err_t t5_net_scan(void);                       /* -> T5_EV_NET_SCAN_DONE */
int t5_net_scan_results(t5_net_ap_t *out, int max);
/* pass may be NULL or "" for open networks. save: remember on success. */
t5_err_t t5_net_connect(const char *ssid, const char *pass, bool save);
t5_err_t t5_net_disconnect(void);                 /* and stop auto-connect until the next connect */
t5_err_t t5_net_forget(const char *ssid);
int t5_net_saved_list(char (*ssids)[T5_NET_SSID_LEN], int max);

/* Power-cycle the C6 and bring everything back (health recovery uses it too). */
t5_err_t t5_net_c6_reset(void);
/* Ask the network for the time now (SNTP); T5_ERR_STATE without Wi-Fi.
 * The answer arrives asynchronously, as a time.set event. */
t5_err_t t5_net_sync_time(void);
/* Update the C6 from the c6fw partition if its version differs (or force).
 * Runs on the service task; T5_EV_NET_C6_UPDATE reports the result. */
t5_err_t t5_net_c6_update(bool force);

/* Internal: ask the net task to start or stop BLE to match its setting. */
void t5_net_ble_changed(void);

/* Antenna (plan 3.7): RF_ANT_SEL on EXP0 P0, saved. */
typedef enum { T5_ANT_INTERNAL, T5_ANT_EXTERNAL } t5_antenna_t;
t5_err_t t5_radio_antenna_set(t5_antenna_t ant);
t5_antenna_t t5_radio_antenna(void);

#ifdef __cplusplus
}
#endif
