/* SPDX-License-Identifier: Apache-2.0
 * Bluetooth LE for apps (needs ble; in the SDK through t5/ble.h): scan,
 * connect, a GATT client with notifications, advertising, and a GATT server
 * (the peripheral role).
 *
 * The radio: the controller runs on the ESP32-C6 (HCI over the esp_hosted
 * SDIO link) and NimBLE runs here. Bluetooth LE only: no Classic Bluetooth
 * (no audio profiles, no SPP). It works while Bluetooth is on in Settings,
 * with Wi-Fi on or off (the C6 runs while either is on): t5_ble_ready()
 * says so, and calls return T5_ERR_STATE otherwise. When the C6 goes down
 * (Bluetooth turned off, Sleep with no handle open, a C6 reset) links drop, a scan ends and
 * reads fail; an advertiser and a server stay open and come back on the air
 * by themselves when it is up again.
 *
 * Conventions are t5/net.h's: every handle closes itself when the app exits
 * or crashes; an app holds at most T5_BLE_APP_MAX handles; timeouts are in
 * ms (0 doesn't wait, T5_BLE_FOREVER waits); calls that move bytes return a
 * count or a negative t5_err_t. While an app has a handle open the OS holds
 * a T5_LOCK_NET wake lock, so Sleep keeps the C6 up. Events reach the app
 * through *_next() calls on its own task: no callbacks run behind its back.
 * Any task. At most T5_BLE_MAX_LINKS links at once, every app's and the
 * system's together, in either role.
 *
 * Addresses are 6 bytes, least significant first as on the air; as text,
 * "aa:bb:cc:dd:ee:ff" is the usual most-significant-first order
 * (t5_ble_parse_addr, t5_ble_format_addr). UUIDs are text: "0x180f" for
 * 16-bit ones, "6e400001-b5a3-f393-e0a9-e50e24dcca9e" for 128-bit ones.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_BLE_APP_MAX 6          /* handles per app: scanners, links, advertisers, servers */
#define T5_BLE_MAX_LINKS 3
#define T5_BLE_FOREVER UINT32_MAX
#define T5_BLE_NAME_LEN 30
#define T5_BLE_UUID_LEN 40        /* text, with its NUL */
#define T5_BLE_MAX_CHARS 48       /* characteristics kept per link by t5_ble_gatt_discover */
#define T5_BLE_VALUE_MAX 244      /* bytes of one notification or server value */
#define T5_BLE_SERVER_MAX_CHARS 8

/* Characteristic properties (the Bluetooth Core values). */
#define T5_BLE_PROP_READ 0x02
#define T5_BLE_PROP_WRITE_NR 0x04 /* write without response */
#define T5_BLE_PROP_WRITE 0x08
#define T5_BLE_PROP_NOTIFY 0x10
#define T5_BLE_PROP_INDICATE 0x20

/* The C6 is up and the stack is in sync (no permission needed). */
bool t5_ble_ready(void);

/* ---- scanning ------------------------------------------------------------- */
typedef struct t5_ble_scanner t5_ble_scanner_t;
typedef struct {
    uint32_t duration_ms;     /* 0: until stopped */
    bool active;              /* ask for scan responses, which often hold the name */
    bool duplicates;          /* every advertisement, not one per device */
} t5_ble_scan_cfg_t;
typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;        /* 0 public, 1 random: pass it to t5_ble_conn_open */
    int8_t rssi;              /* dBm */
    bool connectable;
    bool scan_rsp;            /* data is a scan response */
    char name[T5_BLE_NAME_LEN];   /* "" when this packet has none */
    uint8_t data_len;
    uint8_t data[31];         /* the advertising data as sent */
} t5_ble_report_t;
/* One scan at a time on the radio (T5_ERR_BUSY while another runs). */
t5_err_t t5_ble_scan_start(const t5_ble_scan_cfg_t *cfg, t5_ble_scanner_t **out);
/* The next report: T5_OK, T5_ERR_TIMEOUT, or T5_ERR_STATE once the scan has
 * ended and every report has been taken. Reports that came while the queue
 * (32) was full are dropped. */
t5_err_t t5_ble_scan_next(t5_ble_scanner_t *s, t5_ble_report_t *out, uint32_t timeout_ms);
t5_err_t t5_ble_scan_stop(t5_ble_scanner_t *s);

/* ---- connections and the GATT client -------------------------------------- */
typedef struct t5_ble_conn t5_ble_conn_t;
typedef struct {
    char svc_uuid[T5_BLE_UUID_LEN];
    char uuid[T5_BLE_UUID_LEN];
    uint16_t val_handle;      /* what read, write and subscribe take */
    uint16_t def_handle;
    uint16_t cccd_handle;     /* the descriptor subscribe writes; 0 if it has none */
    uint8_t props;            /* T5_BLE_PROP_* */
} t5_ble_chr_t;

/* Connect as the central. Blocks until connected or timeout_ms. */
t5_err_t t5_ble_conn_open(const uint8_t addr[6], uint8_t addr_type, uint32_t timeout_ms, t5_ble_conn_t **out);
/* False once the link has dropped (the handle stays valid until closed). */
bool t5_ble_conn_up(t5_ble_conn_t *c);
/* The ATT MTU: a write or notification carries up to mtu - 3 bytes. */
uint16_t t5_ble_conn_mtu(t5_ble_conn_t *c);
t5_err_t t5_ble_conn_close(t5_ble_conn_t *c);

/* Find every service, characteristic and notification descriptor. */
t5_err_t t5_ble_gatt_discover(t5_ble_conn_t *c, uint32_t timeout_ms);
int t5_ble_gatt_chars(t5_ble_conn_t *c, t5_ble_chr_t *out, int max);
/* One characteristic by UUIDs (svc_uuid NULL: any service). T5_ERR_NOT_FOUND. */
t5_err_t t5_ble_gatt_find(t5_ble_conn_t *c, const char *svc_uuid, const char *chr_uuid, t5_ble_chr_t *out);
/* Returns the bytes read (cut to cap), or a negative t5_err_t. */
int t5_ble_gatt_read(t5_ble_conn_t *c, uint16_t handle, void *buf, size_t cap, uint32_t timeout_ms);
/* response false: write without response (returns once queued). */
t5_err_t t5_ble_gatt_write(t5_ble_conn_t *c, uint16_t handle, const void *data, size_t len, bool response,
                           uint32_t timeout_ms);
/* Notifications (or indications, if that is all the characteristic has)
 * for the characteristic whose value handle is val_handle, after
 * t5_ble_gatt_discover found its descriptor. */
t5_err_t t5_ble_gatt_subscribe(t5_ble_conn_t *c, uint16_t val_handle, bool on, uint32_t timeout_ms);
/* The next notification or indication: its bytes (cut to cap; may be 0)
 * and the value handle it came from, T5_ERR_TIMEOUT, or T5_ERR_STATE once
 * the link is down and nothing is queued (16 are queued). */
int t5_ble_gatt_notification(t5_ble_conn_t *c, uint16_t *handle, void *buf, size_t cap, uint32_t timeout_ms);

/* ---- advertising ---------------------------------------------------------- */
typedef struct t5_ble_advertiser t5_ble_advertiser_t;
typedef struct {
    const char *name;         /* the local name, NULL for none */
    const uint8_t *mfr_data;  /* manufacturer data: the company id (2 bytes, LSB first), then yours */
    uint8_t mfr_len;
    const char *svc_uuid;     /* one service UUID to list, NULL for none */
    uint32_t interval_ms;     /* 20..10240, 0 = 100 */
    bool connectable;         /* centrals may connect, to reach a t5_ble_server */
} t5_ble_adv_cfg_t;
/* One advertiser at a time on the radio (T5_ERR_BUSY). What doesn't fit the
 * 31-byte packet (the name first) goes in the scan response;
 * T5_ERR_INVALID_ARG if it doesn't fit there either. A connectable
 * advertiser goes back on the air after each link closes. */
t5_err_t t5_ble_adv_start(const t5_ble_adv_cfg_t *cfg, t5_ble_advertiser_t **out);
t5_err_t t5_ble_adv_stop(t5_ble_advertiser_t *a);

/* ---- GATT server ---------------------------------------------------------- */
typedef struct t5_ble_server t5_ble_server_t;
typedef struct {
    const char *uuid;
    uint8_t props;            /* T5_BLE_PROP_*: READ, WRITE, WRITE_NR, NOTIFY, INDICATE */
} t5_ble_server_chr_t;
typedef enum {
    T5_BLE_SERVER_CONNECTED,      /* a central connected (peer, addr) */
    T5_BLE_SERVER_DISCONNECTED,
    T5_BLE_SERVER_WRITE,          /* a central wrote chr: the bytes are in buf */
    T5_BLE_SERVER_SUBSCRIBED,     /* a central turned chr's notifications on (on) or off */
} t5_ble_server_ev_type_t;
typedef struct {
    t5_ble_server_ev_type_t type;
    int chr;                  /* index into the characteristics given to open */
    uint16_t peer;            /* which central: the same number for one link */
    uint8_t addr[6];
    bool on;
} t5_ble_server_ev_t;
/* One service with up to T5_BLE_SERVER_MAX_CHARS characteristics, added to
 * the device's GATT database (UUIDs copied). Reads get the value last set
 * (empty at first). Advertise with a connectable t5_ble_adv_start so
 * centrals can reach it. T5_ERR_BUSY if a service with that UUID is open. */
t5_err_t t5_ble_server_open(const char *svc_uuid, const t5_ble_server_chr_t *chrs, int n, t5_ble_server_t **out);
/* Set chr's value (up to T5_BLE_VALUE_MAX bytes). If it notifies or
 * indicates, every subscribed central gets it. */
t5_err_t t5_ble_server_set(t5_ble_server_t *s, int chr, const void *data, size_t len);
/* The next event; for a write, its bytes go to buf (cut to cap) and the
 * count is returned. 0 for other events, T5_ERR_TIMEOUT, or a negative
 * error. 16 events are queued. */
int t5_ble_server_next(t5_ble_server_t *s, t5_ble_server_ev_t *ev, void *buf, size_t cap, uint32_t timeout_ms);
t5_err_t t5_ble_server_close(t5_ble_server_t *s);

/* ---- HID: the Tab5 as a Bluetooth keyboard, mouse or gamepad (API level 2) --
 * HID over GATT. Every app shares one report map: t5_hid's descriptor of
 * every part (T5_HID_PART_ALL, with the T5_HID_ID_* report IDs), so a
 * computer that paired once keeps working whichever app is in front. Send
 * reports built with t5/hid.h's pack functions.
 *
 * Opening asks the person the first time (as t5/usbdev.h does), then
 * advertises as a HID device until closed; one at a time, and not while an
 * app advertises (T5_ERR_BUSY). Computers pair from their Bluetooth
 * settings; the Tab5 shows the code to confirm (a system dialog). Reports
 * only go out while the app is in the foreground and resumed: when it's
 * suspended the OS sends all-released reports. The handle closes with the
 * app; the person can stop it in Quick Settings. */
#define T5_BLE_APPEARANCE_HID 0x03c0
#define T5_BLE_APPEARANCE_KEYBOARD 0x03c1
#define T5_BLE_APPEARANCE_MOUSE 0x03c2
#define T5_BLE_APPEARANCE_GAMEPAD 0x03c4

typedef struct t5_ble_hid t5_ble_hid_t;
typedef struct {
    uint32_t size;            /* sizeof(t5_ble_hid_cfg_t) */
    const char *name;         /* advertised, NULL: "Tab5" */
    uint16_t appearance;      /* T5_BLE_APPEARANCE_*, 0: HID */
} t5_ble_hid_cfg_t;
typedef enum {
    T5_BLE_HID_CONNECTED,     /* a computer connected, paired, and is listening (addr) */
    T5_BLE_HID_DISCONNECTED,
    T5_BLE_HID_OUTPUT,        /* an output report (keyboard LEDs): bytes in buf, report_id */
    T5_BLE_HID_SUSPEND,       /* the computer suspended (on) or woke (off) the device */
} t5_ble_hid_ev_type_t;
typedef struct {
    t5_ble_hid_ev_type_t type;
    uint8_t addr[6];
    uint8_t report_id;
    bool on;
} t5_ble_hid_ev_t;

t5_err_t t5_ble_hid_open(const t5_ble_hid_cfg_t *cfg, t5_ble_hid_t **out);
/* A paired computer is connected and listening for reports. */
bool t5_ble_hid_connected(t5_ble_hid_t *h);
/* One input report for part (T5_HID_PART_KEYBOARD, _MOUSE, _CONSUMER,
 * _GAMEPAD or _ABSOLUTE), its body without the report ID, to every
 * listening computer. T5_ERR_STATE when none is, or not foreground;
 * T5_ERR_TIMEOUT if the radio's buffers stayed full for timeout_ms. */
t5_err_t t5_ble_hid_send(t5_ble_hid_t *h, uint32_t part, const void *report, size_t len, uint32_t timeout_ms);
/* The next event; an output report's bytes go to buf (cut to cap) and the
 * count is returned. 0 for other events, T5_ERR_TIMEOUT. 16 are queued. */
int t5_ble_hid_next(t5_ble_hid_t *h, t5_ble_hid_ev_t *ev, void *buf, size_t cap, uint32_t timeout_ms);
t5_err_t t5_ble_hid_close(t5_ble_hid_t *h);

/* ---- helpers (no permission, pure) ---------------------------------------- */
bool t5_ble_parse_addr(const char *s, uint8_t out[6]);           /* "aa:bb:cc:dd:ee:ff" */
void t5_ble_format_addr(const uint8_t addr[6], char out[18]);
/* The name in advertising data (0x09 complete, else 0x08 shortened). */
bool t5_ble_adv_name(const uint8_t *adv, size_t len, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
