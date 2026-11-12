# Tab5 OS: device emulation and raw radio access for apps

Nov 12, 2026 · @Kanishka · built on branch io-access (not yet run on hardware) · API level 2

## Summary

Let apps make the Tab5 act as someone else's input or music device, and let them reach the radios and the USB host below the class drivers:

1. **USB-C device mode.** The Tab5 enumerates on a computer as a composite device that the app defines: up to three HID interfaces (keyboard, mouse, gamepad, consumer keys, or any report descriptor) and USB MIDI.
2. **USB-A raw host.** An app opens a device on the USB-A port (or a hub behind it), reads its descriptors, claims interfaces, and does its own control, bulk and interrupt transfers.
3. **Bluetooth LE HID and MIDI.** The Tab5 pairs with a computer, phone or tablet as a keyboard, mouse/trackpad or gamepad (HID over GATT), or as a BLE MIDI instrument.
4. **Wi-Fi monitor mode and raw frames.** An app receives 802.11 frames from the air with channel and RSSI metadata, can change or hop channels, and can send raw frames.

One HID helper layer (`t5/hid.h`) is shared by USB-C and Bluetooth, so an app that builds a keyboard report gets it to either transport by changing one call.

Every port and radio gets **two levels of access**:

- **Managed** (the default). Today's APIs keep working unchanged: the OS owns the hardware and shares it between apps and its own features. Sections 1 to 5 add the device emulation and raw access above to this level.
- **Exclusive** (opt-in). An app takes the whole USB-C port, USB-A bus, Wi-Fi radio or Bluetooth controller for itself, below the OS's drivers and stacks, until it closes (section 6). Meanwhile the OS's own use of that hardware stops, and other apps get `T5_ERR_BUSY`.

## Hardware limits that shape this

| Limit | Consequence |
| --- | --- |
| The USB-A socket is a host-only receptacle on the high-speed controller | Device emulation is only possible on USB-C. USB-A gets raw host access. |
| USB-C uses full-speed PHY 0, which belongs to USB-Serial/JTAG (the console). The OTG controller only gets it by swapping PHYs, and PHY 1's pads are the I2S pins (`t5_usb/usbc.c`) | While USB-C is in device mode, **the console is gone and audio is off**, exactly as in storage mode today. A MIDI app can't play sound on the Tab5 while it's a USB MIDI device. It can over BLE MIDI. Full speed only (12 Mbit/s): enough for HID and MIDI. |
| The C6 is Bluetooth LE only | HID means HID over GATT (HOGP). It works with macOS, Windows 8+, iOS/iPadOS, Android and Linux/BlueZ. Hosts that only speak Classic Bluetooth HID (old TVs, some consoles) won't see it. |
| esp_hosted 3.1.0 doesn't carry promiscuous mode or raw TX (its coverage manifest says "not implemented in hosted") | Monitor mode and raw TX live in our C6 firmware (`c6/`), with frames sent to the P4 over a side channel. It ships with the OS, because the C6 image comes from the `c6fw` partition. |
| Monitor mode and station mode share one radio and one channel | While Wi-Fi is connected, sniffing is locked to the access point's channel. Choosing a channel or hopping pauses the station connection for every app until the monitor closes. |

## Common rules

These follow the conventions `t5/net.h` and `t5/ble.h` already use, so nothing here is new to an app author:

- **Handles clean up after the app.** Every handle closes itself when the app exits or crashes, through the existing resource tracking. Closing a USB-C device handle gives the port back to the console. Closing a monitor brings the station connection back.
- **Events through `*_next()` calls** on the app's own task, with a timeout. No callbacks run behind the app's back.
- **Structs that can grow start with `uint32_t size`** (set to `sizeof`), as planned for notifications in level 2, so later fields don't need `_ex()` functions.
- **Wake locks.** An open handle holds what it needs: USB-C device mode keeps the system awake while a host has the device configured (in Sleep USB stops answering and the host drops the device, as found with storage mode). BLE HID and the Wi-Fi monitor hold `T5_LOCK_NET` so the C6 stays up. The screen may still turn off.
- **HID only from the foreground.** USB-C and Bluetooth HID reports are only accepted while the app is in the foreground. When it leaves the foreground, the OS sends an all-released report on every HID interface (so no key or button stays held on the host), and `*_hid_send` returns `T5_ERR_STATE` until it's back. The port, link and bond stay held, so the host doesn't see the device leave and come back on every app switch. Everything is released when the app closes. MIDI and the raw and exclusive APIs keep working in the background.
- **Visible while active.** The status bar shows an icon while any of these is active, and Quick Settings gets a tile naming the app with a "Stop" button. That is the user's way out if an app misbehaves, and for USB-C it's the way to get the console back.

### Permissions

`T5_PERM_ALL` and the permission bits are locked in level 1, so no new manifest groups are added. Each feature uses the group it belongs to, plus a **first-use prompt** like camera and mic. The answer is stored with the app's grants, can be changed in App Manager, and is only asked once:

| Feature | Manifest group | First-use prompt |
| --- | --- | --- |
| USB-C device mode | `usb` | "*App* wants to use the USB-C port as a keyboard/MIDI device. The console and sound are off while it does." |
| USB-A raw access to an unclaimed interface | `usb` | none |
| USB-A taking an interface from an OS driver (keyboard, mouse, MIDI, serial) | `usb` | "*App* wants to take over *device name*." This is how keystrokes on your own keyboard would reach the app, so it's asked. |
| BLE HID, BLE MIDI | `ble` | "*App* wants to act as a Bluetooth keyboard/mouse for other devices." |
| Wi-Fi monitor | `net` | "*App* wants to see all Wi-Fi traffic nearby." |
| Raw Wi-Fi transmit | `net` | A separate prompt: "*App* wants to send raw Wi-Fi frames. Misuse can disrupt networks and may be illegal where you are." |
| Exclusive USB-C, USB-A, Wi-Fi or Bluetooth (section 6) | `usb`, `usb`, `net`, `ble` | One prompt per subsystem, naming what stops meanwhile: "*App* wants full control of Wi-Fi. Other apps and the system lose Wi-Fi until you close it." |

The prompted grants are stored as internal grant bits above `T5_PERM_ALL` (`T5_CAP_USB_DEVICE`, `T5_CAP_USB_TAKEOVER`, `T5_CAP_BLE_HID`, `T5_CAP_WIFI_MONITOR`, `T5_CAP_WIFI_TX`, and one `T5_CAP_EXCL_*` per subsystem). They're part of the grants file, not the ABI. As everywhere else, for native apps this is consent and correctness, not a security boundary (`T5_NATIVE_APP_WARNING`). Python apps can't get around it.

## 1. Shared HID layer: `t5/hid.h`

Pure code with no hardware access, so it gets host tests (`test/host/test_hid.c`):

- **Ready-made report descriptors** with fixed report IDs: keyboard (8 modifier bits plus 6 keys, LED output report), mouse (5 buttons, 16-bit X/Y, vertical wheel, horizontal pan), consumer control (media keys, volume, brightness), gamepad (16 buttons, 2 sticks, 2 triggers, hat), and absolute digitizer (pen or single touch). Each is available alone or combined into one descriptor.
- **Report builders**: `t5_hid_kbd_report(mods, keys[6], out)`, `t5_hid_mouse_report(...)` and so on, so apps don't pack bits by hand.
- **Text to key strokes**: `t5_hid_text_to_keys(utf8, layout, cb)` for US, UK, German and French layouts. It returns the key/modifier pairs needed, and refuses characters the layout can't type. The host's layout setting decides what a key code means, so the app chooses the layout to match.
- **Trackpads are the app's job.** Hosts don't accept a third-party multi-touch trackpad generically (Windows "precision touchpad" needs a Microsoft certification blob, and macOS only does gestures for Apple trackpads), so a trackpad app recognises gestures itself and sends mouse reports: movement, clicks, wheel and pan. The OS only provides the raw touch points (below). The absolute pointer part is there for drawing apps and works on every host, macOS included.

### Raw touch points

LVGL gives apps one pointer. For multi-finger input, `t5_ui_touch_points(t5_ui_touch_t *out, int max)` returns every finger on the screen (up to 5, each with an id that stays the same while it's down) in the app's screen coordinates, with rotation applied. It only works for the foreground app (0 points otherwise), and the system edge gestures keep working.

## 2. USB-C device mode: `t5/usbdev.h`

### API

```c
typedef struct t5_usbdev t5_usbdev_t;

typedef struct {
    const uint8_t *report_desc;   /* e.g. T5_HID_DESC_KEYBOARD, or the app's own */
    uint16_t desc_len;
    uint8_t boot;                 /* T5_HID_BOOT_NONE / KEYBOARD / MOUSE: works in BIOS/UEFI */
    uint8_t poll_ms;              /* 1..255, 0 = 10 */
    uint16_t max_report;          /* largest input report, bytes (<= 64) */
} t5_usbdev_hid_t;

typedef struct {
    uint32_t size;                /* sizeof(t5_usbdev_cfg_t) */
    uint16_t vid, pid;            /* 0: Espressif's VID and the class-derived PID, as storage mode */
    const char *manufacturer, *product, *serial;   /* NULL: "Tab5 OS", the app's name, the chip's */
    uint8_t hid_count;            /* 0..3 */
    t5_usbdev_hid_t hid[3];
    bool midi;                    /* one USB MIDI port (cable 0) */
} t5_usbdev_cfg_t;

/* Asks the user the first time (blocks until answered: T5_ERR_DENIED), then
 * swaps USB-C to device mode. T5_ERR_BUSY while storage mode or another
 * app's device mode holds the port. */
t5_err_t t5_usbdev_open(const t5_usbdev_cfg_t *cfg, t5_usbdev_t **out);
bool t5_usbdev_mounted(t5_usbdev_t *d);            /* a host has configured us */
/* An input report. T5_ERR_STATE when not mounted, T5_ERR_TIMEOUT if the
 * previous one hasn't gone out yet. */
t5_err_t t5_usbdev_hid_send(t5_usbdev_t *d, int hid, uint8_t report_id, const void *data, size_t len,
                            uint32_t timeout_ms);
t5_err_t t5_usbdev_midi_send(t5_usbdev_t *d, uint8_t cable, const uint8_t *msg, size_t len);
/* Ask a suspended host to wake (if it allowed remote wake-up). */
t5_err_t t5_usbdev_wakeup(t5_usbdev_t *d);

typedef enum {
    T5_USBDEV_MOUNTED, T5_USBDEV_UNMOUNTED, T5_USBDEV_SUSPENDED, T5_USBDEV_RESUMED,
    T5_USBDEV_HID_OUTPUT,         /* the host set an output report (keyboard LEDs): bytes in buf */
    T5_USBDEV_HID_PROTOCOL,       /* boot or report protocol */
    T5_USBDEV_MIDI_IN,            /* a MIDI message from the host: bytes in buf */
} t5_usbdev_ev_type_t;
typedef struct {
    t5_usbdev_ev_type_t type;
    uint8_t hid, report_id, cable;
    bool boot_protocol;
} t5_usbdev_ev_t;
int t5_usbdev_next(t5_usbdev_t *d, t5_usbdev_ev_t *ev, void *buf, size_t cap, uint32_t timeout_ms);
t5_err_t t5_usbdev_close(t5_usbdev_t *d);          /* the console comes back */
```

### Implementation

- **One owner of the USB-C port.** The PHY swap, audio pin hold and wake lock in `usbc.c` become `usbc_take(owner)` / `usbc_give()`, shared by storage mode and device mode. `t5_usbc_mode_t` gets a `T5_USBC_DEVICE` value for the shell and Settings to show. The hand-back keeps its step trace (`t5_usbc_trace`) and the console write that has to follow it.
- **TinyUSB.** `CONFIG_TINYUSB_HID_COUNT=3` and `CONFIG_TINYUSB_MIDI_COUNT=1` are compiled in alongside MSC. The device, configuration and string descriptors are built at open from the app's config (`tud_descriptor_*_cb` read them), so one firmware serves any report descriptor. The configuration descriptor is checked against the endpoint budget of the full-speed controller before the PHY moves, and a bad config fails with `T5_ERR_INVALID_ARG` while the console is still there.
- **Getting the console back.** The port goes back to the console when the handle closes, when the app exits or crashes (resource cleanup), when the app is killed after not responding, and from the Quick Settings tile. Recovery mode never enters device mode. In the background the port stays held, but HID reports stop (see "HID only from the foreground").
- **Reports** go out on the TinyUSB task from a small queue per HID interface, so a slow host doesn't block the app beyond `timeout_ms`. Keyboard key-up reports are never dropped (a stuck key on the host is the worst failure here). If the queue overflows, the newest state is sent and the keyboard state is resent in full.

## 3. USB-A raw host: additions to `t5/usb.h`

### API

```c
typedef struct t5_usb_handle t5_usb_handle_t;

/* addr from t5_usb_devices(). */
t5_err_t t5_usb_open(uint8_t addr, t5_usb_handle_t **out);
/* The raw device descriptor, and the active configuration descriptor with
 * all its interfaces and endpoints (cut to cap; returns the full length). */
int t5_usb_device_desc(t5_usb_handle_t *h, void *buf, size_t cap);
int t5_usb_config_desc(t5_usb_handle_t *h, void *buf, size_t cap);
int t5_usb_string(t5_usb_handle_t *h, uint8_t index, char *out, size_t cap);   /* UTF-8 */

#define T5_USB_CLAIM_TAKE_OVER 0x01   /* detach the OS driver if it has it (asks the user once) */
t5_err_t t5_usb_claim(t5_usb_handle_t *h, uint8_t iface, uint8_t alt, uint32_t flags);
t5_err_t t5_usb_release(t5_usb_handle_t *h, uint8_t iface);   /* the OS driver gets it back */

/* Control transfer on endpoint 0. Returns the bytes moved in the data
 * stage, or a negative t5_err_t (T5_ERR_IO for a STALL). */
int t5_usb_control(t5_usb_handle_t *h, uint8_t bm_request_type, uint8_t b_request, uint16_t w_value,
                   uint16_t w_index, void *data, uint16_t len, uint32_t timeout_ms);
/* Bulk or interrupt on a claimed interface's endpoint; the direction is the
 * address's top bit. */
int t5_usb_transfer(t5_usb_handle_t *h, uint8_t ep, void *data, size_t len, uint32_t timeout_ms);
/* Keep interrupt or bulk IN transfers queued so nothing is missed between
 * calls (HID input, audio-less streaming). */
t5_err_t t5_usb_stream_start(t5_usb_handle_t *h, uint8_t ep, uint8_t depth);
int t5_usb_stream_next(t5_usb_handle_t *h, uint8_t ep, void *buf, size_t cap, uint32_t timeout_ms);
/* After unplugging, calls return T5_ERR_STATE; the handle stays until closed. */
bool t5_usb_handle_up(t5_usb_handle_t *h);
t5_err_t t5_usb_close(t5_usb_handle_t *h);   /* releases every claim */
```

### Implementation

- The `usb.t5` client already opens every device to build the device list, and claims MIDI interfaces itself (`usb_midi.c`). Raw handles go through that same client, so there's one owner per interface and no second client to coordinate.
- **Taking over from an OS driver.** HID interfaces close in `hid_host` (the keyboard or mouse disappears from the UI's input devices), MIDI and CDC-ACM close their streams, and the interface is then claimed for the app. Releasing it, or the app ending, gives it back and the OS driver attaches as if the device had just been plugged in. Mass storage is never taken over while mounted: the app gets `T5_ERR_BUSY` and should ask the user to eject first. Hubs can't be claimed.
- The OS opened every HID interface it was offered, including ones it ignored. It now opens only boot keyboards and mice, so gamepads, macro pads and vendor HID devices are free to claim without a take-over question.
- **Isochronous transfers** (USB audio interfaces, webcams outside UVC) are left out of the first version: they need scheduling and buffer sizes chosen per device. They come later (see Decisions).

## 4. Bluetooth LE HID and MIDI: `t5/ble.h` additions

### What has to come first

The GATT server (`t5_ble_server_*`) has one service per handle, no descriptors, and no pairing. HID needs more than it can offer:

- **Pairing and bonding.** Turn on NimBLE's security manager use (LE Secure Connections, which is already compiled in: `CONFIG_BT_NIMBLE_SM_SC=y`), persistent bonds in the P4's NVS (`CONFIG_BT_NIMBLE_NVS_PERSIST`, up to 8 bonds) and address resolution (IRKs), so hosts using random private addresses reconnect. IO capability is "display, yes/no": the host shows a 6-digit number and the Tab5 shows the same number for the user to confirm (numeric comparison).
- **The pairing prompt belongs to the system**, not the app. The shell shows "Pair with *host name*? Code 123456" so an app can't confirm pairings on its own or hide the code.
- **Settings > Bluetooth** gets a "Paired devices" list with Forget. Forget only removes the Tab5's side of the bond, so the user also has to remove the Tab5 on the host.

### HID over GATT

A built-in profile in `t5_net/ble_hid.c` (not on top of the generic server), registering the standard services:

- HID (0x1812): HID Information, Report Map (the descriptor from `t5/hid.h` or the app's own), one Report characteristic per report ID with its Report Reference descriptor, Protocol Mode, HID Control Point, and the boot keyboard/mouse reports.
- Battery (0x180F) with the Tab5's real battery level, and Device Information (0x180A) with PnP ID.
- Advertising with the HID service UUID and the matching appearance (keyboard 0x03C1, mouse 0x03C2, gamepad 0x03C4, generic 0x03C0). Characteristics need an encrypted link, which starts pairing on first connect.

```c
typedef struct t5_ble_hid t5_ble_hid_t;
typedef struct {
    uint32_t size;
    const char *name;             /* advertised; NULL: "Tab5" */
    uint16_t appearance;          /* T5_BLE_APPEARANCE_KEYBOARD, _MOUSE, _GAMEPAD; 0: HID */
} t5_ble_hid_cfg_t;
t5_err_t t5_ble_hid_open(const t5_ble_hid_cfg_t *cfg, t5_ble_hid_t **out);
/* part: T5_HID_PART_KEYBOARD, _MOUSE, _CONSUMER, _GAMEPAD or _ABSOLUTE */
t5_err_t t5_ble_hid_send(t5_ble_hid_t *h, uint32_t part, const void *report, size_t len, uint32_t timeout_ms);
bool t5_ble_hid_connected(t5_ble_hid_t *h);
/* Events: connected (bonded host's address and name), disconnected,
 * output report (LEDs), protocol mode, suspend/exit suspend. */
int t5_ble_hid_next(t5_ble_hid_t *h, t5_ble_hid_ev_t *ev, void *buf, size_t cap, uint32_t timeout_ms);
t5_err_t t5_ble_hid_close(t5_ble_hid_t *h);
```

- **One identity, one report map.** The first plan gave each HID app its own random static address, so the host would see separate devices. NimBLE keys its bond store by the peer's address alone, so a second identity bonded to the same computer would overwrite the first one's keys. Instead the HID, battery and device information services are registered at every stack start, before any app's dynamic service, so their handles never move, and every app shares one report map: t5_hid's descriptor of every part (keyboard, mouse, media keys, gamepad, absolute pointer, with fixed report IDs). A computer that paired once keeps a correct cached copy whichever app is in front. Apps send reports by part (`t5_ble_hid_send(h, T5_HID_PART_MOUSE, ...)`).
- **Reconnecting** to a bonded host uses directed advertising for a few seconds, then normal advertising if `pairable`.
- **Latency.** HCI runs over SDIO to the C6, which adds some delay that hasn't been measured yet.
- **Foreground only.** In the background the link stays up but reports stop, as on USB-C.
- **Bond slots.** The store keeps 16 bonds in NVS; when it's full, the oldest goes. Settings > Bluetooth lists paired devices and forgets them.
- **Limits.** The HID profile uses the advertiser, so it gets `T5_ERR_BUSY` if an app has `t5_ble_adv_start` running, and vice versa. It counts one link against `T5_BLE_MAX_LINKS`.

### BLE MIDI

Not built as its own API: an app can already do it with the GATT server (`t5_ble_server_open` with the BLE MIDI service and characteristic, a connectable advertiser), packing the BLE MIDI timestamps itself. A helper can come later if apps need it.

## 5. Wi-Fi monitor mode and raw frames: `t5/wifi.h`

### C6 firmware

`c6/main` gets a monitor module on top of esp_hosted:

- **Control**: start (channel, frame-type filter, control-frame filter, snap length, rate limit), stop, set channel, transmit frame. These are esp_hosted peer-data messages (`CONFIG_ESP_HOSTED_CP_FEAT_PEER_DATA` on the C6, `eh_host_peer_data_send` on the P4).
- **Frames to the P4**: the promiscuous callback copies each frame (cut to the snap length) with its `rx_ctrl` metadata into a ring buffer. A task packs them into batches of up to ~1.5 KB and sends those to the P4. When the ring is full, frames are dropped and counted, and the count travels with the next batch, so the app sees exactly how much it missed.
- **Throughput is the main risk.** A busy 2.4 GHz channel carries 1,000 to 3,000 frames a second, about 400 KB/s at a 128-byte snap length. Peer data travels with RPC messages. If measurement shows it can't keep up, frames move to their own esp_hosted interface type on the SDIO data path, which already carries Wi-Fi traffic at several MB/s. The default filter is management frames only, which is enough for scanners and much lighter.
- **Raw transmit** uses `esp_wifi_80211_tx` as ESP-IDF provides it, including the frame types it refuses in its sanity check. Tab5 OS doesn't patch around those. A rate limit (default 50 frames/s, at most 500, set at open) is enforced on the C6.
- **Hopping** is the app's loop over `t5_wifimon_set_channel`; there is no separate call.
- **Size.** The C6 image grew to 1,504,480 bytes of the 1,572,864-byte `c6fw` partition (96%).

### P4 API

```c
typedef struct t5_wifimon t5_wifimon_t;
#define T5_WIFIMON_MGMT 0x1
#define T5_WIFIMON_CTRL 0x2
#define T5_WIFIMON_DATA 0x4
#define T5_WIFIMON_MISC 0x8
typedef struct {
    uint32_t size;
    uint8_t channel;              /* 0: the current one */
    uint8_t types;                /* T5_WIFIMON_* */
    uint16_t snaplen;             /* bytes kept per frame, 24..1350; 0 = 256 */
    bool exclusive;               /* may pause the station connection to change channel */
    uint16_t tx_per_s;            /* raw transmit limit, 1..500; 0 = 50 */
} t5_wifimon_cfg_t;
typedef struct {
    uint64_t time_us;             /* the C6's receive time */
    int8_t rssi, noise_floor;
    uint8_t channel, second_channel;
    uint8_t rate, sig_mode;       /* legacy, HT, VHT, HE */
    uint16_t len;                 /* length on the air (the copy may be shorter) */
    uint8_t type;                 /* T5_WIFIMON_* */
    uint8_t fcs_ok;
} t5_wifimon_frame_t;

/* One monitor at a time (T5_ERR_BUSY). Asks the user the first time. */
t5_err_t t5_wifimon_open(const t5_wifimon_cfg_t *cfg, t5_wifimon_t **out);
/* The next frame's bytes (cut to cap): returns the count, T5_ERR_TIMEOUT,
 * or a negative error. */
int t5_wifimon_next(t5_wifimon_t *m, t5_wifimon_frame_t *meta, void *buf, size_t cap, uint32_t timeout_ms);
/* T5_ERR_STATE while the station is connected and the monitor isn't exclusive. */
t5_err_t t5_wifimon_set_channel(t5_wifimon_t *m, uint8_t channel);
typedef struct { uint32_t frames, dropped_radio, dropped_link, dropped_queue, sent, refused; uint8_t channel; } t5_wifimon_stats_t;
void t5_wifimon_stats(t5_wifimon_t *m, t5_wifimon_stats_t *out);
/* Send one raw 802.11 frame (asks the user the first time). T5_ERR_DENIED
 * for frame types ESP-IDF refuses, T5_ERR_BUSY over the rate limit. */
t5_err_t t5_wifimon_send(t5_wifimon_t *m, const void *frame, size_t len, bool use_sys_seq);
t5_err_t t5_wifimon_close(t5_wifimon_t *m);

/* pcap (LINKTYPE_IEEE802_11_RADIOTAP) helpers, so a capture written to /sd
 * opens in Wireshark. */
size_t t5_wifimon_pcap_header(uint8_t out[24]);
size_t t5_wifimon_pcap_record(const t5_wifimon_frame_t *meta, const void *frame, size_t n, uint8_t *out, size_t cap);
```

- **Station interplay.** Without `exclusive`, the monitor runs alongside the connection on its channel, and networking keeps working. With `exclusive`, the first channel change disconnects the station. The status bar says "Wi-Fi paused by *app*", other apps' network calls return `T5_ERR_STATE`, and the connection comes back when the monitor closes. Power save is off while a monitor is open.
- **Channels** follow the Wi-Fi country setting (1 to 11 or 1 to 13). The ESP32-C6 is 2.4 GHz only, so 5 GHz traffic can't be seen.

## 6. Exclusive control

An app that wants the hardware itself, rather than the OS's view of it, claims the whole subsystem. A claim:

- asks the user the first time (see Permissions), then waits for the OS to let go: managed handles held by other apps close with `T5_ERR_STATE`, and OS features using that hardware stop;
- lasts until the app closes the handle, exits or crashes, or is killed for not responding. It survives going to the background (only HID reports are foreground-only), and the Quick Settings tile can always end it;
- gives everything back on release: the OS resets the hardware to a known state and restarts its own drivers and stacks, as after boot.

```c
typedef enum { T5_EXCL_USBC, T5_EXCL_USBA, T5_EXCL_WIFI, T5_EXCL_BT } t5_excl_kind_t;
typedef struct t5_excl t5_excl_t;
/* Blocks until the user answers and the OS has let go. T5_ERR_BUSY if another app holds it. */
t5_err_t t5_excl_claim(t5_excl_kind_t what, t5_excl_t **out);
bool t5_excl_active(t5_excl_t *h);       /* false once the person stopped it */
t5_err_t t5_excl_release(t5_excl_t *h);
```

The calls below take the handle, so they only work while it's held.

### USB-C: a USB device of the app's own design

What stops: the console and audio (as with any device mode), storage mode, and managed device mode.

The app supplies the whole descriptor set (device, configuration, strings, and BOS with Microsoft OS 2.0 descriptors so Windows binds WinUSB without a driver), and then handles every class and vendor control request and every endpoint itself. This is a TinyUSB application class driver (`usbd_app_driver_get_cb`) that passes everything through to the app, so any class works: vendor-specific/WebUSB, CDC, HID with its own control handling, audio, DFU-style protocols.

```c
t5_err_t t5_usbc_raw_start(t5_excl_t *h, const t5_usbc_raw_desc_t *desc);
/* Events: mounted, unmounted, suspend/resume, and class or vendor
 * requests; one with needs_reply waits for t5_usbc_raw_ctrl_reply (data or
 * zero-length) or t5_usbc_raw_ctrl_stall. */
int t5_usbc_raw_next(t5_excl_t *h, t5_usbc_raw_ev_t *ev, void *buf, size_t cap, uint32_t timeout_ms);
t5_err_t t5_usbc_raw_ctrl_reply(t5_excl_t *h, const void *data, size_t len);
t5_err_t t5_usbc_raw_ctrl_stall(t5_excl_t *h);
int t5_usbc_raw_xfer(t5_excl_t *h, uint8_t ep, void *buf, size_t len, uint32_t timeout_ms);
t5_err_t t5_usbc_raw_stall(t5_excl_t *h, uint8_t ep, bool on);
```

The controller is full speed (12 Mbit/s); endpoints 1..5, bulk or interrupt, up to 64-byte packets, checked before the PHY moves. Requests that send data to the device are acknowledged automatically and reach the app with their bytes. Python apps don't get this one yet.

### USB-A: the whole bus

What stops: every OS class driver (keyboard and mouse input, `/usb` storage, which is unmounted cleanly first, serial, MIDI, UVC), and other apps' raw handles.

The app sees every device with the raw handles from section 3, can claim any interface without take-over prompts, and also gets:

- `t5_usba_power(h, on)`: switch the port's 5 V and root port, to power-cycle every device.

Taking the bus unmounts a drive cleanly, then cycles the power with the OS's drivers held off, so everything enumerates again free for the app. The ESP-IDF host library has no per-device port reset, so there is no single-device reset.

The hub driver itself stays in the ESP-IDF host library, because the bus doesn't work without it. Isochronous transfers come later (see Decisions).

### Wi-Fi: the radio

What stops: the OS's station connection, so every app's networking, plus developer mode, update checks and time sync. They come back on release, with the saved network reconnecting.

The app runs the C6's Wi-Fi itself:

- **Mode**: off, station, soft AP, station + AP, monitor.
- **Station**: scan with every parameter (active/passive, dwell time per channel, hidden SSIDs, BSSID), connect to a network of its choosing with its own credentials (never saved by the OS), and get an lwIP network interface, so sockets and `t5/net.h` work over it.
- **Soft AP**: SSID, security, channel, max clients, client list and events, with a DHCP server on its interface.
- **Radio settings**: country, channel and bandwidth (HT20/HT40), TX power, protocol set (b/g/n/ax), power save, MAC address of each interface.
- **Monitor and raw frames** from section 5, without the station limits: any channel, hopping, and a higher TX limit (500 frames/s). ESP-IDF's frame-type restrictions on raw transmit stay.
- **ESP-NOW**: not built yet.

The calls are `t5_wifi_excl_scan`, `_connect`, `_disconnect`, `_ap_start`, `_ap_stop`, `_ap_clients`, `_radio` and `_mac`. Most of this is esp_wifi_remote calls that esp_hosted already forwards. The net task still handles the station's events, so a network the app joins gets the OS's netif and sockets, but it never reconnects by itself while the claim is held.

### Bluetooth: the controller over HCI

What stops: the OS's NimBLE host, so every BLE handle, system Bluetooth features and the HID profile. Saved bonds are kept.

The app gets the C6's Bluetooth LE controller as raw HCI. It can run its own host stack (BTstack, its own NimBLE build, a small custom one) or send commands directly:

```c
/* H4 frames: the packet type byte (1 command, 2 ACL, 4 event, 5 ISO) first */
t5_err_t t5_bt_hci_send(t5_excl_t *h, const void *frame, size_t len);
int t5_bt_hci_next(t5_excl_t *h, void *buf, size_t cap, uint32_t timeout_ms);
```

On claim, NimBLE stops and the controller is reset. The app owns it from HCI Reset onwards, including Espressif vendor commands. On release, the OS resets the controller and starts NimBLE again. The controller is the C6's: Bluetooth LE 5.x, no Classic.

## Python

The same calls with the same permissions and questions (`t5_py/mod_io.c`):

- `t5.hid`: `descriptor(parts)`, `keys(text, layout)`, `keyboard()`, `mouse()`, `consumer()`, `gamepad()`, `absolute()`.
- `t5.usbdev.open(hid=[desc, ...], midi=False, product=None, boot=None)` with `send()`, `midi()`, `event()`, `mounted()`.
- `t5.ble.hid(name, appearance)` with `send(part, report)`, `event()`, `connected()`.
- `t5.wifi.monitor(channel, types=, snaplen=, exclusive=, tx_per_s=)` with `frame()`, `set_channel()`, `stats()`, `send()`, `pcap(path, seconds)`.
- `t5.usb.devices()` and `t5.usb.open(addr)` with descriptors, `claim()`, `control()`, `read()`, `write()`, `stream()`, `next()`.
- `t5.exclusive("usba" | "wifi" | "bt")` as a context manager, with `power()`, `scan()`, `connect()`, `ap()`, `radio()`, `mac()`, `hci_send()`, `hci_next()`. Exclusive USB-C is native-only for now.

## ABI

All new symbols are level 2 (`since=2` in `sdk/abi/api_v1.sym`, `T5_API_LEVEL` 2), together with notifications. The new headers (`t5/hid.h`, `t5/usbdev.h`, `t5/wifi.h`) and the additions to `t5/usb.h` and `t5/ble.h` go through the layout lock as usual. Nothing in level 1 changes.

## Status

Everything above is built on the `io-access` branch: host tests pass (`t5_hid`, the claims, pcap records), the firmware and the C6 image build, and the ABI and lint checks pass. **None of it has run on hardware yet.** Next, on Unit A, in order:

1. `t5 hil usbc_device` (USB-C HID and MIDI echo against this Mac, with the console coming back).
2. `net mon 10` and `net mon 10 6 all` (monitor frames, drops; needs the new C6 image: `t5 c6-stage`, then `net update`).
3. Bluetooth HID: pair the Mac with the Tab5 from System Settings, confirm the code, type from a test app.
4. USB-A raw access with a gamepad or vendor device; exclusive USB-A with a drive mounted.
5. Exclusive Wi-Fi and Bluetooth: release brings the network and NimBLE back.
6. Exclusive USB-C with a vendor-class loopback.

## Decisions

1. **USB vendor/product ID: nothing new needed.** Device mode uses the same IDs storage mode already does (esp_tinyusb's defaults): Espressif's VID 0x303A, with the PID built from the classes present (`0x4000 | CDC<<0 | MSC<<1 | HID<<2 | MIDI<<3 | ...`). HID and MIDI bind by class, so the ID only identifies the device, and a different class mix gets a different PID, which keeps a host's cache of one layout from being applied to another. Apps that need a specific driver (vendor class in exclusive mode) set their own.
2. **Per-app Bluetooth identity: yes**, with 16 bond slots and least-recently-used eviction (section 4).
3. **Isochronous transfers on USB-A: later**, after phase 3.
4. **HID only from the foreground.** Reports stop in the background with everything released on the host. The port and link stay held until the app closes. Exclusive claims also last until the app closes, background included.
