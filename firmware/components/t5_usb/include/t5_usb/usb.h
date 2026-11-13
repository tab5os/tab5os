/* SPDX-License-Identifier: Apache-2.0
 * USB (plan 7.7, 7.8).
 *
 * USB-A, host, on the high-speed controller. USB5V_EN is a rail held while
 * the host is enabled (dropped in Sleep, so devices re-enumerate on wake).
 * Hubs are supported. Class drivers: HID keyboards and mice (decoded for an
 * input adapter, t5_usb/hid.h), mass storage mounted at /usb, CDC-ACM serial, USB
 * MIDI and UVC cameras (MJPEG frames to an app).
 *
 * USB-C: the console (USB-Serial/JTAG) by default. Mass storage mode hands
 * the SD card to a PC over the full-speed controller: /sd is unmounted
 * first and comes back afterwards, /data is never exposed, and the console
 * is gone until the mode ends (the two share one PHY; audio stops too, as
 * the swapped PHY's pads are the I2S pins). A timeout returns to console.
 *
 * Safe from any task.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"
#include "t5_core/excl.h"
#include "t5_storage/paths.h"   /* T5_USB_PATH */

#ifdef __cplusplus
extern "C" {
#endif

#define T5_USB_MAX_DEVICES 8

typedef enum {
    T5_USB_OTHER,
    T5_USB_HUB,
    T5_USB_KEYBOARD,
    T5_USB_MOUSE,
    T5_USB_STORAGE,
    T5_USB_SERIAL,
    T5_USB_MIDI,
    T5_USB_CAMERA,
    T5_USB_AUDIO,
    T5_USB_CLASS_COUNT,
} t5_usb_class_t;
const char *t5_usb_class_name(t5_usb_class_t c);

typedef struct {
    uint8_t addr;
    uint16_t vid, pid;
    uint8_t cls;              /* t5_usb_class_t, the main function */
    uint8_t speed;            /* 0 low, 1 full, 2 high */
    char product[32];
    bool driver;              /* a Tab5 OS driver has it */
} t5_usb_dev_t;

t5_err_t t5_usb_init(void);
t5_err_t t5_usb_host_enable(bool on);          /* persisted; default on */
bool t5_usb_host_enabled(void);
int t5_usb_devices(t5_usb_dev_t *out, int max);
int t5_usb_device_count(void);
bool t5_usb_keyboard_present(void);
bool t5_usb_mouse_present(void);

/* Mass storage at /usb. */
bool t5_usb_storage_mounted(void);
t5_err_t t5_usb_storage_eject(void);
/* Erase the drive at /usb (one FAT filesystem; not on the UI task). */
t5_err_t t5_usb_storage_format(void);

/* CDC-ACM serial: the first serial device. */
typedef struct t5_usb_serial t5_usb_serial_t;
bool t5_usb_serial_present(void);
t5_err_t t5_usb_serial_open(uint32_t baud, t5_usb_serial_t **out);
int t5_usb_serial_write(t5_usb_serial_t *s, const void *data, size_t n, uint32_t timeout_ms);
int t5_usb_serial_read(t5_usb_serial_t *s, void *data, size_t n, uint32_t timeout_ms);
t5_err_t t5_usb_serial_close(t5_usb_serial_t *s);

/* USB MIDI: messages (1-3 bytes, running status expanded) from any MIDI
 * device; one listener. */
typedef void (*t5_usb_midi_cb_t)(const uint8_t *msg, size_t len, void *arg);
bool t5_usb_midi_present(void);
t5_err_t t5_usb_midi_listen(t5_usb_midi_cb_t cb, void *arg);   /* NULL stops */
t5_err_t t5_usb_midi_send(const uint8_t *msg, size_t len);

/* UVC camera: MJPEG frames delivered on the USB task; return quickly. */
typedef struct t5_usb_uvc t5_usb_uvc_t;
typedef void (*t5_usb_uvc_frame_cb_t)(const uint8_t *jpeg, size_t len, void *arg);
bool t5_usb_uvc_present(void);
t5_err_t t5_usb_uvc_open(uint16_t w, uint16_t h, uint8_t fps, t5_usb_uvc_frame_cb_t cb, void *arg,
                         t5_usb_uvc_t **out);
t5_err_t t5_usb_uvc_close(t5_usb_uvc_t *u);

/* ---- raw access (API level 2) ---------------------------------------------
 * An app opens a device on USB-A (or behind a hub there), reads its
 * descriptors, claims interfaces and does its own control, bulk and
 * interrupt transfers. Interfaces the OS's drivers aren't using (gamepads,
 * vendor-specific devices) are free to claim; one the OS uses (a boot
 * keyboard or mouse, MIDI) needs T5_USB_CLAIM_TAKE_OVER, which asks the
 * person the first time and gives the interface back when released. A
 * mounted drive, an open serial port or a streaming camera is T5_ERR_BUSY:
 * eject or close it first. Hubs can't be claimed. Isochronous endpoints
 * aren't supported yet.
 *
 * Handles close themselves when the app exits or crashes, releasing every
 * claim. After the device is unplugged calls return T5_ERR_STATE; the
 * handle stays valid until closed. Any task. */
#define T5_USB_MAX_HANDLES 8         /* every app's together */
#define T5_USB_XFER_MAX 4096         /* bytes per transfer */
#define T5_USB_FOREVER UINT32_MAX
#define T5_USB_CLAIM_TAKE_OVER 0x01

typedef struct t5_usb_handle t5_usb_handle_t;

t5_err_t t5_usb_open(uint8_t addr, t5_usb_handle_t **out);
/* The device descriptor (18 bytes) and the active configuration descriptor
 * with every interface and endpoint, cut to cap. Both return the full
 * length, or a negative t5_err_t. */
int t5_usb_device_desc(t5_usb_handle_t *h, void *buf, size_t cap);
int t5_usb_config_desc(t5_usb_handle_t *h, void *buf, size_t cap);
/* String descriptor index as UTF-8 (cut to cap, NUL-terminated). Returns
 * the length, or a negative t5_err_t. */
int t5_usb_string(t5_usb_handle_t *h, uint8_t index, char *out, size_t cap);

t5_err_t t5_usb_claim(t5_usb_handle_t *h, uint8_t iface, uint8_t alt, uint32_t flags);
/* An interface taken over goes back to the OS's driver. */
t5_err_t t5_usb_release(t5_usb_handle_t *h, uint8_t iface);

/* A control transfer on endpoint 0 (the setup packet's fields). The data
 * stage's direction is bm_request_type's top bit. Returns the bytes moved,
 * or a negative t5_err_t: T5_ERR_IO for a STALL. */
int t5_usb_control(t5_usb_handle_t *h, uint8_t bm_request_type, uint8_t b_request, uint16_t w_value,
                   uint16_t w_index, void *data, uint16_t len, uint32_t timeout_ms);
/* Bulk or interrupt, on an endpoint of a claimed interface; ep's top bit is
 * the direction (0x81 IN, 0x01 OUT). len up to T5_USB_XFER_MAX. Returns the
 * bytes moved, or a negative t5_err_t (T5_ERR_TIMEOUT cancels the transfer). */
int t5_usb_transfer(t5_usb_handle_t *h, uint8_t ep, void *data, size_t len, uint32_t timeout_ms);
/* Keep depth (1..8) transfers queued on an IN endpoint so nothing is missed
 * between calls (HID input). One stream per endpoint. */
t5_err_t t5_usb_stream_start(t5_usb_handle_t *h, uint8_t ep, uint8_t depth);
/* The next completed transfer's bytes from that stream (cut to cap), or
 * T5_ERR_TIMEOUT, or T5_ERR_STATE once unplugged. 16 are queued. */
int t5_usb_stream_next(t5_usb_handle_t *h, uint8_t ep, void *buf, size_t cap, uint32_t timeout_ms);
t5_err_t t5_usb_stream_stop(t5_usb_handle_t *h, uint8_t ep);
/* False once the device is unplugged. */
bool t5_usb_handle_up(t5_usb_handle_t *h);
/* Stops streams and releases every claim. */
t5_err_t t5_usb_close(t5_usb_handle_t *h);

/* ---- the whole bus: exclusive USB-A (API level 2) -------------------------
 * With t5_excl_claim(T5_EXCL_USBA) (t5/excl.h) the OS's USB drivers let go:
 * a drive at /usb is unmounted cleanly first, then the port's power is
 * cycled so every device enumerates again with no OS driver on it
 * (keyboards and mice stop working for the system, other apps' raw handles
 * see their device leave). The app opens and claims anything with the raw
 * calls above, without take-over questions. T5_ERR_BUSY if an app has the
 * serial port or a camera open. On release the power is cycled again and
 * the OS's drivers find everything. */
/* Switch USB-A's 5 V and root port: off then on power-cycles every device. */
t5_err_t t5_usba_power(t5_excl_t *h, bool on);

/* USB-C role. Storage mode hands the SD card to the computer; the Tab5
 * takes it back when the computer ejects it, when asked
 * (t5_usbc_set_mode(T5_USBC_CONSOLE)), or after timeout_s (0 = no timeout).
 * Taking it back while the computer has it mounted is an unsafe removal on
 * the computer's side. */
typedef enum {
    T5_USBC_CONSOLE,
    T5_USBC_STORAGE,
    T5_USBC_DEVICE,           /* an app's device mode (t5_usb/usbdev.h); set_mode(CONSOLE) ends it */
} t5_usbc_mode_t;
t5_err_t t5_usbc_set_mode(t5_usbc_mode_t mode, uint32_t timeout_s);
t5_usbc_mode_t t5_usbc_mode(void);
/* The last storage session ended with the computer's eject. */
bool t5_usbc_ejected(void);

typedef struct {
    bool host_on, installed;
    uint32_t attached, detached;
    uint32_t keys, mouse_reports, midi_in, midi_out;
    uint32_t uvc_frames, uvc_errors;
    uint32_t errors;
    uint8_t devices;
} t5_usb_stats_t;
void t5_usb_stats(t5_usb_stats_t *out);

#ifdef __cplusplus
}
#endif
