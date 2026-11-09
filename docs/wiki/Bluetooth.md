# Bluetooth LE

Include `<t5/ble.h>` and declare `ble = true` in `[permissions]`. Everything here follows the [networking](Networking.md) conventions: handles close themselves when your app exits or crashes, timeouts are milliseconds (`T5_BLE_FOREVER` waits), byte-moving calls return a count or a negative `t5_err_t`, and events come to your own task through `*_next()` calls.

## What the hardware does

| | |
| --- | --- |
| Radio | The ESP32-C6 co-processor. Its controller talks HCI to NimBLE on the P4 over the esp_hosted SDIO link |
| Bluetooth LE | Central (scan, connect, GATT client) and peripheral (advertise, GATT server), both at once |
| Classic Bluetooth | No: the C6 has none. No A2DP or HFP audio, no SPP serial |
| LE Audio | No: not available on the C6 with this ESP-IDF |
| Links | 3 at once (`T5_BLE_MAX_LINKS`), every app's and the system's, in either role |
| Power | Bluetooth works while the C6 is up: **Wi-Fi and Bluetooth both on** in Settings (they share the chip). `t5_ble_ready()` says so; calls return `T5_ERR_STATE` otherwise. While your app has a handle open the OS holds a net wake lock, so Sleep keeps the radio up |

When the C6 goes down (turned off, a C6 reset) links drop and a scan ends. Advertisers and servers stay open and come back on the air by themselves.

## Scan

```c
t5_ble_scanner_t *sc;
if (t5_ble_scan_start(&(t5_ble_scan_cfg_t){ .duration_ms = 5000, .active = true }, &sc) == T5_OK) {
    t5_ble_report_t r;
    while (t5_ble_scan_next(sc, &r, 1000) != T5_ERR_STATE) {   /* T5_OK: a report; TIMEOUT: none yet */
        char a[18];
        t5_ble_format_addr(r.addr, a);
        printf("%s %d dBm %s\n", a, r.rssi, r.name);
    }
    t5_ble_scan_stop(sc);
}
```

One scan at a time on the radio (`T5_ERR_BUSY`). `active` asks for scan responses, which usually carry the name. `r.data` is the raw advertising data; `t5_ble_adv_name()` pulls the name out of any.

## Connect and use GATT

```c
t5_ble_conn_t *c;
t5_ble_conn_open(r.addr, r.addr_type, 10000, &c);          /* blocks until connected */
t5_ble_gatt_discover(c, 10000);                            /* services, characteristics, CCCDs */
t5_ble_chr_t level;
if (t5_ble_gatt_find(c, "0x180f", "0x2a19", &level) == T5_OK) {           /* battery level */
    uint8_t pct;
    t5_ble_gatt_read(c, level.val_handle, &pct, 1, 5000);
    t5_ble_gatt_subscribe(c, level.val_handle, true, 5000);
    uint16_t h;
    uint8_t buf[T5_BLE_VALUE_MAX];
    int n = t5_ble_gatt_notification(c, &h, buf, sizeof buf, 30000);   /* bytes, TIMEOUT, or STATE once down */
}
t5_ble_gatt_write(c, handle, data, len, true, 5000);       /* false: write without response */
t5_ble_conn_close(c);
```

UUIDs are text: `"0x2a19"` for 16-bit ones, the full form for 128-bit ones. A write or notification carries up to `t5_ble_conn_mtu(c) - 3` bytes. Subscribe turns on notifications, or indications if that's all the characteristic has. `t5_ble_conn_up()` goes false when the link drops; close the handle and connect again.

## Be a peripheral

```c
static const t5_ble_server_chr_t chrs[] = {
    { "0x2a19", T5_BLE_PROP_READ | T5_BLE_PROP_NOTIFY },                    /* 0: battery level */
    { "6e400002-b5a3-f393-e0a9-e50e24dcca9e", T5_BLE_PROP_WRITE },          /* 1: commands */
};
t5_ble_server_t *srv;
t5_ble_server_open("0x180f", chrs, 2, &srv);
t5_ble_server_set(srv, 0, &pct, 1);                        /* subscribers are notified */
t5_ble_advertiser_t *adv;
t5_ble_adv_start(&(t5_ble_adv_cfg_t){ .name = "My Tab5", .svc_uuid = "0x180f", .connectable = true }, &adv);

t5_ble_server_ev_t ev;
uint8_t buf[T5_BLE_VALUE_MAX];
int n = t5_ble_server_next(srv, &ev, buf, sizeof buf, 1000);
if (n >= 0 && ev.type == T5_BLE_SERVER_WRITE && ev.chr == 1) handle_command(buf, n);
```

A server is one service of up to 8 characteristics; reads get the value you last set. A connectable advertiser goes back on the air after each central leaves. A non-connectable one is a beacon: put your data in `mfr_data` (the 2-byte company id first). What doesn't fit the 31-byte packet (the name first) moves to the scan response. One advertiser at a time on the radio.

## Limits

| | |
| --- | --- |
| Handles per app | `T5_BLE_APP_MAX` (6): scanners, links, advertisers and servers together |
| Queues | 32 scan reports, 16 notifications per link, 16 server events; more are dropped |
| Value size | `T5_BLE_VALUE_MAX` (244) bytes per notification, read or server value |
| Characteristics kept per link | 48 |
| Pairing, bonding, encryption | Not in this API |

## Testing

The console's `ble` command uses the same API: `ble scan`, `ble connect <addr> [type]`, `ble gatt`, `ble read <h>`, `ble write <h> <text> [nr]`, `ble sub <h>`, `ble notes <ms>`, `ble close`, `ble adv [name [conn]]`, and `ble server on` / `ev <ms>` / `off` for a test GATT server advertised as "Tab5 test". The `ble_explorer` example app scans, connects and reads, and can act as a battery-level peripheral.
