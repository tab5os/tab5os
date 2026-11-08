/* SPDX-License-Identifier: Apache-2.0
 * USB (plan 7.7, 7.8).
 *
 * USB-A, host, on the high-speed controller. USB5V_EN is a rail held while
 * the host is enabled (dropped in Sleep, so devices re-enumerate on wake).
 * Hubs are supported. Class drivers: HID keyboards and mice (they become
 * LVGL input devices), mass storage mounted at /usb, CDC-ACM serial, USB
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

#ifdef __cplusplus
extern "C" {
#endif

#define T5_USB_PATH "/usb"
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

/* USB-C role. Storage mode hands the SD card to the computer; the Tab5
 * takes it back when the computer ejects it, when asked
 * (t5_usbc_set_mode(T5_USBC_CONSOLE)), or after timeout_s (0 = no timeout).
 * Taking it back while the computer has it mounted is an unsafe removal on
 * the computer's side. */
typedef enum { T5_USBC_CONSOLE, T5_USBC_STORAGE } t5_usbc_mode_t;
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
