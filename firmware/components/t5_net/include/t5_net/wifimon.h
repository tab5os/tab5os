/* SPDX-License-Identifier: Apache-2.0
 * Wi-Fi monitor mode and raw 802.11 frames (API level 2, needs net; in the
 * SDK through t5/wifi.h).
 *
 * The radio is the ESP32-C6 (2.4 GHz only, so 5 GHz traffic can't be
 * seen). esp_hosted doesn't forward promiscuous mode, so Tab5 OS's own C6
 * firmware runs it and sends frames to the P4 in batches; on a busy channel
 * some are dropped, and the stats say how many (on the radio, on the way,
 * or in the queue here).
 *
 * Opening asks the person the first time ("see all Wi-Fi traffic nearby");
 * sending a raw frame asks again, separately. One monitor at a time
 * (T5_ERR_BUSY). Wi-Fi must be on in Settings (T5_ERR_STATE).
 *
 * Channels: while the Tab5 is connected to a network, the monitor shares
 * the radio with it and listens on its channel; networking keeps working.
 * A monitor opened with exclusive set may change channel or hop: the first
 * change pauses the connection for every app and the system (their network
 * calls fail) until the monitor closes, when it rejoins.
 *
 * Raw transmit uses ESP-IDF's esp_wifi_80211_tx as it is, including the
 * frame types it refuses, at most tx_per_s frames a second.
 *
 * Handles close with the app; the person can stop it in Quick Settings.
 * While open the C6 stays up through Sleep. Any task.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"
#include "t5_core/excl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_WIFIMON_MGMT 0x01
#define T5_WIFIMON_CTRL 0x02
#define T5_WIFIMON_DATA 0x04
#define T5_WIFIMON_MISC 0x08
#define T5_WIFIMON_FOREVER UINT32_MAX

typedef struct t5_wifimon t5_wifimon_t;

typedef struct {
    uint32_t size;            /* sizeof(t5_wifimon_cfg_t) */
    uint8_t channel;          /* 1..13, 0: the current one */
    uint8_t types;            /* T5_WIFIMON_*, 0: management only */
    uint16_t snaplen;         /* bytes kept per frame, 24..1350, 0: 256 */
    bool exclusive;           /* may pause the network connection to change channel */
    uint16_t tx_per_s;        /* raw transmit limit, 1..500, 0: 50 */
} t5_wifimon_cfg_t;

typedef struct {
    uint32_t time_us;         /* the radio's receive time (wraps every 71 min) */
    int8_t rssi;              /* dBm */
    int8_t noise_floor;
    uint8_t channel;
    uint8_t second;           /* HT40: 1 above, 2 below, 0 none */
    uint8_t rate;
    uint8_t format;           /* 0 11b/g, 1 11n, 3 11ac, 4+ 11ax */
    uint8_t type;             /* T5_WIFIMON_* */
    bool fcs_bad;
    uint16_t len;             /* the frame's length on the air (the copy may be shorter) */
} t5_wifimon_frame_t;

typedef struct {
    uint32_t frames;          /* handed to the app */
    uint32_t dropped_radio;   /* the C6's queue was full */
    uint32_t dropped_link;    /* batches lost between the C6 and the P4 */
    uint32_t dropped_queue;   /* this side's queue was full (the app didn't keep up) */
    uint32_t sent, refused;   /* raw frames */
    uint8_t channel;          /* listening on now */
} t5_wifimon_stats_t;

t5_err_t t5_wifimon_open(const t5_wifimon_cfg_t *cfg, t5_wifimon_t **out);
/* The next frame's bytes (cut to cap, FCS excluded): returns the count
 * copied, T5_ERR_TIMEOUT, or T5_ERR_STATE once stopped. */
int t5_wifimon_next(t5_wifimon_t *m, t5_wifimon_frame_t *meta, void *buf, size_t cap, uint32_t timeout_ms);
/* T5_ERR_STATE while connected to a network unless the monitor is exclusive. */
t5_err_t t5_wifimon_set_channel(t5_wifimon_t *m, uint8_t channel);
void t5_wifimon_stats(t5_wifimon_t *m, t5_wifimon_stats_t *out);
/* One raw 802.11 frame, from the 24-byte header on (no FCS). T5_ERR_DENIED
 * if the person said no or ESP-IDF refuses that frame type, T5_ERR_BUSY over
 * the per-second limit. */
t5_err_t t5_wifimon_send(t5_wifimon_t *m, const void *frame, size_t len, bool use_sys_seq);
t5_err_t t5_wifimon_close(t5_wifimon_t *m);

/* ---- the radio: exclusive Wi-Fi ---------------------------------------------
 * With t5_excl_claim(T5_EXCL_WIFI) (t5/excl.h) the OS's network connection
 * pauses (other apps and the system lose the network; developer mode,
 * update checks and time sync stop) and the app runs the radio. A monitor
 * it opens may change channel freely. On release the OS undoes the app's
 * settings (soft AP off, radio settings back) and rejoins its network. */
typedef struct {
    uint8_t bssid[6];
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    uint8_t auth;             /* 0 open, 1 WEP, 2 WPA, 3 WPA2, 4 WPA/WPA2, 6 WPA3, 7 WPA2/WPA3... (wifi_auth_mode_t) */
} t5_wifi_ap_t;

typedef struct {
    uint32_t size;
    uint8_t channel;          /* 0: every channel */
    bool passive;             /* listen for beacons instead of asking */
    bool show_hidden;
    uint16_t dwell_ms;        /* per channel, 0: the radio's default */
} t5_wifi_scan_cfg_t;
/* Blocks until done: the networks found (up to max), or a negative t5_err_t. */
int t5_wifi_excl_scan(t5_excl_t *h, const t5_wifi_scan_cfg_t *cfg, t5_wifi_ap_t *out, int max);
/* Join a network as a station with the app's credentials (never saved).
 * Waits for an address; then sockets and t5/net.h work over it. */
t5_err_t t5_wifi_excl_connect(t5_excl_t *h, const char *ssid, const char *pass, uint32_t timeout_ms);
t5_err_t t5_wifi_excl_disconnect(t5_excl_t *h);

typedef struct {
    uint32_t size;
    const char *ssid;         /* 1..32 bytes */
    const char *pass;         /* NULL or "": open; else 8..63 (WPA2) */
    uint8_t channel;          /* 1..13, 0: 6 */
    uint8_t max_clients;      /* 1..8, 0: 4 */
    bool hidden;
} t5_wifi_ap_cfg_t;
/* A soft access point (with a DHCP server at 192.168.4.1) alongside the
 * station. T5_ERR_NOT_SUPPORTED if the radio firmware can't. */
t5_err_t t5_wifi_excl_ap_start(t5_excl_t *h, const t5_wifi_ap_cfg_t *cfg);
t5_err_t t5_wifi_excl_ap_stop(t5_excl_t *h);
/* MAC addresses of the stations on the soft AP: the count, or an error. */
int t5_wifi_excl_ap_clients(t5_excl_t *h, uint8_t (*mac)[6], int max);

#define T5_WIFI_PROTO_B 0x01
#define T5_WIFI_PROTO_G 0x02
#define T5_WIFI_PROTO_N 0x04
#define T5_WIFI_PROTO_AX 0x20
typedef struct {
    uint32_t size;
    char country[3];          /* "US", "DE"...; "" leaves it */
    int8_t tx_power_dbm;      /* 2..20, 0 leaves it */
    uint8_t protocols;        /* T5_WIFI_PROTO_*, 0 leaves it */
    uint8_t bandwidth;        /* 20 or 40 (MHz), 0 leaves it */
} t5_wifi_radio_t;
/* Change the station's radio settings (set may be NULL) and read them back
 * (get may be NULL). */
t5_err_t t5_wifi_excl_radio(t5_excl_t *h, const t5_wifi_radio_t *set, t5_wifi_radio_t *get);
/* The station's MAC address: read it, or set it (a locally administered
 * unicast address) while not connected. */
t5_err_t t5_wifi_excl_mac(t5_excl_t *h, uint8_t mac[6], bool set);

/* pcap files (LINKTYPE_IEEE802_11_RADIOTAP, 127) that open in Wireshark:
 * the 24-byte file header, then one record per frame. Pure. */
size_t t5_wifimon_pcap_header(uint8_t out[24]);
/* The record for one frame (record header, radiotap header, then the
 * bytes): its length, or 0 if cap is too small. */
size_t t5_wifimon_pcap_record(const t5_wifimon_frame_t *meta, const void *frame, size_t n, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
