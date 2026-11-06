/* SPDX-License-Identifier: Apache-2.0
 * App runtime (plan 5.4–5.12): launching, the lifecycle, tracking and
 * cleanup, native ELF loading, permissions, the crash guard and the UI
 * watchdog. This is the OS side; apps see t5_app/app.h.
 *
 * One app is in the foreground at a time. Launching another exits the
 * current one (Phase 6 may keep suspended apps around).
 *
 * App context rule (plan, "Gaps"): while an app is in the foreground, the
 * UI task runs with that app as its context, so everything the app's
 * callbacks, timers and event handlers create is charged to it. System UI
 * code that runs on the UI task meanwhile (shell, quick settings) and
 * acquires resources for itself must bracket the call with
 * t5_sys_enter()/t5_sys_leave(), or the resource is released when the app
 * exits.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_app/app.h"
#include "t5_app/crashguard.h"
#include "t5_core/err.h"
#include "t5_core/res.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Crash guard check (plan 5.11): attribute a crash, save its core dump and
 * erase it from flash. Call early, after storage and before the display
 * starts: flash erases starve the panel of pixels. t5_apps_init() runs it
 * if nobody did. */
t5_err_t t5_apps_boot_check(void);
/* Start the runtime: MD2 theme, launcher, watchdog, hooks. Needs
 * t5_ui_init() first. */
t5_err_t t5_apps_init(void);
bool t5_apps_ready(void);

/* Apps compiled into the image (system apps, Phase 6). They go through the
 * same lifecycle and tracking as installed ones. */
typedef struct {
    const char *id;
    const char *name;
    const t5_app_desc_t *desc;
    uint32_t perms;                 /* T5_PERM_* */
    uint8_t orientation;            /* t5_orient_pref_t */
    const char *const *ports;       /* NULL-terminated, NULL = none */
} t5_builtin_app_t;
t5_err_t t5_apps_register_builtin(const t5_builtin_app_t *app);

/* Safe from any task: the work runs on the UI task. */
t5_err_t t5_apps_launch(const char *id);
t5_err_t t5_apps_exit_current(void);
/* Blocking variants for the console and tests (wait up to timeout_ms). */
t5_err_t t5_apps_launch_wait(const char *id, uint32_t timeout_ms);
t5_err_t t5_apps_exit_wait(uint32_t timeout_ms);
/* Copies the foreground app's id, "" if the launcher is showing. */
void t5_apps_current(char *id, size_t n);

typedef struct {
    char id[64];
    char name[48];
    char version[16];
    bool builtin;
    bool running;
    bool autolaunch_blocked;
    uint16_t crashes;               /* consecutive */
} t5_app_entry_t;
int t5_apps_list(t5_app_entry_t *out, int max);

/* The foreground app's tracked resources. */
typedef struct {
    size_t heap_bytes, heap_peak;
    uint32_t heap_blocks;
    uint32_t res[T5_RES_KIND_COUNT];
    uint32_t dropped;
    uint32_t denied_symbols;        /* imports bound to permission stubs */
} t5_app_usage_t;
t5_err_t t5_apps_usage(t5_app_usage_t *out);

/* Install a .t5app (plan 5.8): validates the manifest and, for native
 * apps, that every symbol the ELF imports is in the ABI (plan 5.6).
 * dest: 0 auto, 1 SD, 2 /data. */
t5_err_t t5_apps_install(const char *path, int dest, char *err, size_t errlen);
t5_err_t t5_apps_remove(const char *id, bool keep_data);
/* The ABI check alone. missing gets a comma-separated list. */
t5_err_t t5_apps_check_abi(const uint8_t *elf, size_t len, char *missing, size_t n);

bool t5_apps_autolaunch_allowed(const char *id);
int t5_apps_crashes(t5_crash_rec_t *out, int max);
/* Where an app's last core dump was saved, if any. */
t5_err_t t5_apps_coredump_path(const char *id, char *path, size_t n);

/* Run an app's on_alarm headless (plan 5.10), bounded by its manifest's
 * max_seconds. Blocks the caller. */
t5_err_t t5_apps_run_alarm(const char *id);

/* Services call this before using camera or mic on an app's behalf
 * (Phase 7). System callers always pass. */
t5_err_t t5_perm_check(uint32_t perm);

/* Diagnostics: the watchdog's dialog drawn over a healthy UI (it is only
 * rendered once an app has started). */
t5_err_t anr_test_show(void);
t5_err_t anr_test_hide(void);

/* System code on the UI task while an app is foreground. */
void *t5_sys_enter(void);
void t5_sys_leave(void *saved);

/* Install-time warning shown for every native app (plan 5.9). */
#define T5_NATIVE_APP_WARNING                                                         \
    "Native apps run with full access to the device. Permissions are checked for "   \
    "consent and correctness, but they cannot stop a native app from reaching the "  \
    "hardware directly. Only install apps you trust."

#ifdef __cplusplus
}
#endif
