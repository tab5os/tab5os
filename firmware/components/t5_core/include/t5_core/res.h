/* SPDX-License-Identifier: Apache-2.0
 * App resource tracking hooks (plan 5.4).
 *
 * Services that hand out something an app must give back (a port claim, a
 * rail hold, a wake lock, a task, an LVGL timer...) report it here when the
 * caller is an app (t5_app_ctx_get() != NULL), and report the release. The
 * app runtime (t5_app) installs the hooks and frees whatever is left, in a
 * fixed order, when the app exits or crashes.
 *
 * Without hooks installed (host tests, early boot) these do nothing, so a
 * service never depends on t5_app.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Release order on app exit is the order of this enum: tasks stop first so
 * nothing re-acquires what is being freed, memory goes last. */
typedef enum {
    T5_RES_TASK,
    T5_RES_TIMER,      /* lv_timer_t */
    T5_RES_ANIM,       /* lv_anim_t variable */
    T5_RES_UI,         /* LVGL root objects */
    T5_RES_PORT,
    T5_RES_RAIL,
    T5_RES_LOCK,
    T5_RES_OTHER,
    T5_RES_KIND_COUNT,
} t5_res_kind_t;

/* "tasks", "timers", "anims", "ui", "ports", "rails", "locks", "other". */
const char *t5_res_kind_name(t5_res_kind_t kind);

typedef void (*t5_res_release_fn_t)(void *handle);

typedef struct {
    void (*add)(void *app, t5_res_kind_t kind, void *handle, t5_res_release_fn_t release);
    void (*remove)(void *app, void *handle);
} t5_res_hooks_t;

/* Installed once by the app runtime. NULL removes them. */
void t5_res_set_hooks(const t5_res_hooks_t *hooks);

/* Record handle for the calling task's app. No-op for system callers. */
void t5_res_add(t5_res_kind_t kind, void *handle, t5_res_release_fn_t release);
/* Forget handle (it was released normally). Safe for unknown handles. */
void t5_res_remove(void *handle);
/* Same, for a handle released by someone other than its app (a timeout). */
void t5_res_remove_app(void *app, void *handle);

/* Every app context starts with this header, so a service can name the
 * owner of a claim without depending on t5_app. */
typedef struct {
    uint32_t magic;          /* T5_APP_CTX_MAGIC */
    const char *id;          /* "com.example.app" */
    const char *name;        /* what the person sees ("Air Log"), "" until known */
} t5_app_ctx_hdr_t;

#define T5_APP_CTX_MAGIC 0x54354150u   /* "T5AP" */

/* Runtime permission check for services (camera, mic) without a
 * dependency on t5_app: the app runtime installs the check; with none
 * installed everything is allowed. Bits match T5_PERM_* in t5_pkg. */
#define T5_CORE_PERM_CAMERA (1u << 0)
#define T5_CORE_PERM_MIC (1u << 1)
#define T5_CORE_PERM_NET (1u << 2)
#define T5_CORE_PERM_BLE (1u << 3)
#define T5_CORE_PERM_USB (1u << 5)
/* Capabilities: a first-use question inside a manifest group, for what the
 * group alone shouldn't grant (docs/io-access-design.md). Checked with
 * t5_core_perm_check like the bits above; the app must declare the group.
 * Stored with the app's grants above the manifest bits, never in a
 * manifest. */
#define T5_CORE_CAP_USB_DEVICE (1u << 16)    /* usb: USB-C device mode */
#define T5_CORE_CAP_USB_TAKEOVER (1u << 17)  /* usb: take a USB-A interface from the OS */
#define T5_CORE_CAP_BLE_HID (1u << 18)       /* ble: act as a Bluetooth keyboard/mouse */
#define T5_CORE_CAP_WIFI_MONITOR (1u << 19)  /* net: see all Wi-Fi traffic */
#define T5_CORE_CAP_WIFI_TX (1u << 20)       /* net: send raw Wi-Fi frames */
#define T5_CORE_CAP_EXCL_USBC (1u << 21)     /* usb: exclusive USB-C */
#define T5_CORE_CAP_EXCL_USBA (1u << 22)     /* usb: exclusive USB-A bus */
#define T5_CORE_CAP_EXCL_WIFI (1u << 23)     /* net: exclusive Wi-Fi radio */
#define T5_CORE_CAP_EXCL_BT (1u << 24)       /* ble: exclusive Bluetooth controller */
#define T5_CORE_CAPS 0x01ff0000u
/* The manifest group a capability belongs to (T5_PERM_* value), 0 if none. */
uint32_t t5_core_cap_group(uint32_t cap);

typedef int (*t5_perm_check_fn_t)(uint32_t perm);
void t5_core_set_perm_check(t5_perm_check_fn_t fn);
/* T5_OK, T5_ERR_DENIED, or T5_ERR_TIMEOUT if the person didn't answer. */
int t5_core_perm_check(uint32_t perm);

/* Is the calling task's app the foreground app, resumed (not suspended for
 * Sleep or screen off)? For services that only serve the app the person is
 * using (HID reports). The app runtime installs the check; with none
 * installed, or for system callers, true. */
typedef bool (*t5_fg_check_fn_t)(void);
void t5_core_set_fg_check(t5_fg_check_fn_t fn);
bool t5_core_app_foreground(void);

/* May the calling app read (write: change) path? For services that open
 * files for an app (camera photos, recordings). The app runtime installs
 * its file policy; with none installed, or for system callers, yes. */
typedef bool (*t5_path_check_fn_t)(const char *path, bool write);
void t5_core_set_path_check(t5_path_check_fn_t fn);
bool t5_core_path_check(const char *path, bool write);

/* Owner name for the calling task: the app id, or "system". */
const char *t5_app_ctx_id(void *ctx);
const char *t5_caller_id(void);

#ifdef __cplusplus
}
#endif
