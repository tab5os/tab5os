/* SPDX-License-Identifier: Apache-2.0
 * USB-C device mode (API level 2, needs usb; in the SDK through
 * t5/usbdev.h): the Tab5 enumerates on a computer as a device the app
 * describes, with up to three HID interfaces and USB MIDI. Build HID
 * descriptors and reports with t5/hid.h.
 *
 * The cost: USB-C runs on the full-speed controller by borrowing the
 * console's PHY, whose spare pads are the I2S pins, so while device mode
 * is open the USB-C console is gone and sound is off (as in USB-C mass
 * storage mode). Opening asks the person the first time; from the UI task
 * that call returns T5_ERR_DENIED while the question shows, so open from
 * another task or call again after they answer.
 *
 * The port stays the app's until t5_usbdev_close, until the app exits or
 * crashes, or until the person stops it in Quick Settings: then the
 * console comes back. HID reports only go out while the app is in the
 * foreground and resumed. When it's suspended (Sleep, screen off) the OS
 * sends an all-released report on each HID interface, so nothing stays
 * held on the computer, and t5_usbdev_hid_send returns T5_ERR_STATE until
 * it resumes. MIDI keeps working. While open, the device stays awake.
 *
 * Events reach the app through t5_usbdev_next() on its own task. Any task.
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

#define T5_USBDEV_MAX_HID 3
#define T5_USBDEV_HID_REPORT_MAX 63    /* bytes of one input report, without its ID */
#define T5_USBDEV_HID_DESC_MAX 512
#define T5_USBDEV_FOREVER UINT32_MAX

typedef struct t5_usbdev t5_usbdev_t;

typedef enum {
    T5_USBDEV_BOOT_NONE,
    T5_USBDEV_BOOT_KEYBOARD,  /* answers BIOS/UEFI boot protocol: the descriptor must be a keyboard without IDs */
    T5_USBDEV_BOOT_MOUSE,     /* ...a mouse without IDs */
} t5_usbdev_boot_t;

typedef struct {
    const uint8_t *report_desc;   /* e.g. from t5_hid_descriptor(); copied at open */
    uint16_t desc_len;            /* 1..T5_USBDEV_HID_DESC_MAX */
    uint8_t boot;                 /* t5_usbdev_boot_t */
    uint8_t poll_ms;              /* 1..255, 0 = 5 */
} t5_usbdev_hid_t;

typedef struct {
    uint32_t size;                /* sizeof(t5_usbdev_cfg_t) */
    uint16_t vid, pid;            /* 0: Espressif's VID and a PID from the classes present */
    const char *manufacturer;     /* NULL: "Tab5 OS"; strings are copied, up to 31 bytes */
    const char *product;          /* NULL: the app's name */
    const char *serial;           /* NULL: from the chip's MAC */
    uint8_t hid_count;            /* 0..T5_USBDEV_MAX_HID */
    t5_usbdev_hid_t hid[T5_USBDEV_MAX_HID];
    bool midi;                    /* one USB MIDI port (cable 0) */
} t5_usbdev_cfg_t;

/* Switch USB-C to device mode. T5_ERR_INVALID_ARG for a bad config (no
 * interface at all, a descriptor too long), T5_ERR_BUSY while USB-C is in
 * mass storage mode or another app has it, T5_ERR_DENIED if the person
 * said no. Blocks until the question is answered. */
t5_err_t t5_usbdev_open(const t5_usbdev_cfg_t *cfg, t5_usbdev_t **out);
/* A computer has configured the device (and hasn't suspended it). */
bool t5_usbdev_mounted(t5_usbdev_t *d);
/* An input report on HID interface hid (the report ID first on the wire if
 * report_id isn't 0). Waits up to timeout_ms for the previous one to go
 * out: T5_ERR_TIMEOUT. T5_ERR_STATE when not mounted or not foreground. */
t5_err_t t5_usbdev_hid_send(t5_usbdev_t *d, int hid, uint8_t report_id, const void *data, size_t len,
                            uint32_t timeout_ms);
/* One MIDI message (1..3 bytes, a full status byte first; SysEx up to 3
 * bytes per call, in order). T5_ERR_STATE when not mounted. */
t5_err_t t5_usbdev_midi_send(t5_usbdev_t *d, const uint8_t *msg, size_t len);
/* Ask a suspended computer to wake (only if it allowed remote wake-up). */
t5_err_t t5_usbdev_wakeup(t5_usbdev_t *d);

typedef enum {
    T5_USBDEV_MOUNTED,            /* the computer configured the device */
    T5_USBDEV_UNMOUNTED,          /* unplugged, or the computer let it go */
    T5_USBDEV_SUSPENDED,          /* the computer is asleep (remote_wakeup: it may be woken) */
    T5_USBDEV_RESUMED,
    T5_USBDEV_HID_OUTPUT,         /* an output report (keyboard LEDs): bytes in buf */
    T5_USBDEV_HID_PROTOCOL,       /* boot or report protocol chosen (boot_protocol) */
    T5_USBDEV_MIDI_IN,            /* a MIDI message from the computer: bytes in buf */
} t5_usbdev_ev_type_t;

typedef struct {
    t5_usbdev_ev_type_t type;
    uint8_t hid;                  /* HID_OUTPUT, HID_PROTOCOL: which interface */
    uint8_t report_id;            /* HID_OUTPUT */
    bool boot_protocol;           /* HID_PROTOCOL */
    bool remote_wakeup;           /* SUSPENDED */
} t5_usbdev_ev_t;

/* The next event; for one with bytes, they go to buf (cut to cap) and the
 * count is returned. 0 for other events, T5_ERR_TIMEOUT, or a negative
 * error. 32 events are queued; older ones are kept when it's full. */
int t5_usbdev_next(t5_usbdev_t *d, t5_usbdev_ev_t *ev, void *buf, size_t cap, uint32_t timeout_ms);
/* Give USB-C back to the console. */
t5_err_t t5_usbdev_close(t5_usbdev_t *d);

/* ---- a device of the app's own design: exclusive USB-C (API level 2) -------
 * With t5_excl_claim(T5_EXCL_USBC) (t5/excl.h) the app describes the whole
 * device (descriptors byte for byte) and runs every interface itself: any
 * class, vendor-specific with WebUSB or WinUSB (give a BOS descriptor with
 * Microsoft OS 2.0 and answer its vendor request), CDC, audio... at full
 * speed (12 Mbit/s), with endpoints 1..5 each way, interrupt and bulk.
 * Standard requests (addresses, configurations, descriptors) are answered
 * by the OS from what the app gave; every class and vendor request comes to
 * the app as a T5_USBC_RAW_SETUP event. One with needs_reply must be
 * answered (t5_usbc_raw_ctrl_reply or _ctrl_stall) within about a second,
 * or the computer gives up; one that carried data to the device arrives
 * with its bytes in buf and is already acknowledged. Console and sound are
 * off while it runs, as in t5_usbdev_open. */
#define T5_USBC_RAW_MAX_STRINGS 7
#define T5_USBC_RAW_CTRL_MAX 512       /* bytes of one control transfer's data */

typedef struct {
    uint32_t size;                     /* sizeof(t5_usbc_raw_desc_t) */
    const uint8_t *device;             /* the 18-byte device descriptor */
    const uint8_t *config;             /* the whole configuration descriptor (wTotalLength bytes) */
    const char *const *strings;        /* string descriptors 1..n, UTF-8 (ASCII shown best); NULL entries allowed */
    uint8_t string_count;              /* 0..T5_USBC_RAW_MAX_STRINGS */
    const uint8_t *bos;                /* NULL, or a BOS descriptor (wTotalLength bytes) */
} t5_usbc_raw_desc_t;

/* Copy the descriptors and go: the computer sees the device. T5_ERR_STATE
 * without the claim, T5_ERR_INVALID_ARG for descriptors the controller
 * can't do. */
t5_err_t t5_usbc_raw_start(t5_excl_t *h, const t5_usbc_raw_desc_t *desc);

typedef enum {
    T5_USBC_RAW_MOUNTED,               /* configured by the computer */
    T5_USBC_RAW_UNMOUNTED,
    T5_USBC_RAW_SUSPENDED,
    T5_USBC_RAW_RESUMED,
    T5_USBC_RAW_SETUP,                 /* a class or vendor control request */
} t5_usbc_raw_ev_type_t;
typedef struct {
    t5_usbc_raw_ev_type_t type;
    uint8_t bm_request_type, b_request;
    uint16_t w_value, w_index, w_length;
    bool needs_reply;                  /* the computer waits for reply or stall */
} t5_usbc_raw_ev_t;
/* The next event; a SETUP that carried data puts its bytes in buf (cut to
 * cap) and returns the count. 0 for others, T5_ERR_TIMEOUT, T5_ERR_STATE
 * once the claim ended. */
int t5_usbc_raw_next(t5_excl_t *h, t5_usbc_raw_ev_t *ev, void *buf, size_t cap, uint32_t timeout_ms);
/* Answer the pending request: data for an IN request (cut to its
 * wLength), or nothing (len 0) to acknowledge one without data. */
t5_err_t t5_usbc_raw_ctrl_reply(t5_excl_t *h, const void *data, size_t len);
t5_err_t t5_usbc_raw_ctrl_stall(t5_excl_t *h);
/* A bulk or interrupt transfer on an endpoint of the configuration (0x81 IN,
 * 0x01 OUT). Up to 4096 bytes; an OUT-direction (receive) ends at a short
 * packet. Returns the bytes moved, T5_ERR_TIMEOUT (the transfer stays
 * queued: call again to wait on), or T5_ERR_STATE when not mounted. */
int t5_usbc_raw_xfer(t5_excl_t *h, uint8_t ep, void *buf, size_t len, uint32_t timeout_ms);
t5_err_t t5_usbc_raw_stall(t5_excl_t *h, uint8_t ep, bool on);

#ifdef __cplusplus
}
#endif
