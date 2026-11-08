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
T5_MUST_CHECK t5_err_t t5_apps_boot_check(void);
/* Safe mode (plan 9.3): before t5_apps_init(). No session restore, no
 * background alarms, and installed apps don't launch (they can still be
 * removed in the App Manager). Built-in apps work. */
void t5_apps_set_safe_mode(bool on);
bool t5_apps_safe_mode(void);

/* Start the runtime: MD2 theme, launcher, watchdog, hooks. Needs
 * t5_ui_init() first. */
T5_MUST_CHECK t5_err_t t5_apps_init(void);
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
    const char *icon;               /* MD_ICON_* glyph for the launcher, NULL = generic */
} t5_builtin_app_t;
T5_MUST_CHECK t5_err_t t5_apps_register_builtin(const t5_builtin_app_t *app);

/* Runtimes other than native ELF (plan 8.1): apps whose manifest says
 * runtime = "micropython" are run through desc, like a built-in app, with
 * the same tracking. t5_py registers itself here before t5_apps_init(). */
T5_MUST_CHECK t5_err_t t5_apps_set_runtime(int runtime, const t5_app_desc_t *desc);
/* For a runtime: the installed entry file, the [background] entry name and
 * the manifest's min_heap in bytes (0 if unset). */
const char *t5_apps_entry_path(t5_app_t *app);
const char *t5_apps_bg_entry(t5_app_t *app);
size_t t5_apps_min_heap(t5_app_t *app);

/* The home screen (plan 6.3). The shell registers it before t5_apps_init();
 * without one a plain app list is used. All hooks run on the UI task.
 *   screen   the home screen object (created on first call)
 *   refresh  the app list changed (install, removal, crash counts)
 *   prepare  an app's root screen was just created (status bar inset)
 *   changed  the foreground changed: an app id, or "" for home */
struct _lv_obj_t;
typedef struct {
    struct _lv_obj_t *(*screen)(void);
    void (*refresh)(void);
    void (*prepare)(struct _lv_obj_t *app_screen, const char *id);
    void (*changed)(const char *id);
} t5_apps_home_t;
void t5_apps_set_home(const t5_apps_home_t *home);

/* Safe from any task: the work runs on the UI task. */
T5_MUST_CHECK t5_err_t t5_apps_launch(const char *id);
/* Launch with an argument the app reads with t5_app_launch_arg() (a path,
 * a settings page, an app id). If the app is already in front with another
 * argument it is restarted. */
T5_MUST_CHECK t5_err_t t5_apps_launch_arg(const char *id, const char *arg);
T5_MUST_CHECK t5_err_t t5_apps_exit_current(void);
/* Blocking variants for the console and tests (wait up to timeout_ms). */
T5_MUST_CHECK t5_err_t t5_apps_launch_wait(const char *id, uint32_t timeout_ms);
T5_MUST_CHECK t5_err_t t5_apps_exit_wait(uint32_t timeout_ms);
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
    char icon[8];                   /* MD_ICON_* glyph, "" for none */
    uint32_t perms;                 /* T5_PERM_* declared */
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
T5_MUST_CHECK t5_err_t t5_apps_usage(t5_app_usage_t *out);

/* App Manager (plan 6.7): everything known about one app. */
typedef struct {
    t5_app_entry_t entry;
    char install_dir[192];
    char data_dir[192];
    uint64_t install_bytes;           /* package size, 0 for built-ins */
    uint64_t data_bytes;              /* app data dir, recursive */
    bool on_sd;
    int64_t install_time;
    uint32_t granted, denied;         /* runtime answers for camera and mic */
    uint16_t crashes_total;
    uint8_t last_crash_kind;          /* t5_crash_kind_t */
    int64_t last_crash_time;
    bool has_dump;
    char dump_path[192];
    char runtime[12];                 /* "native", "micropython", "built-in" */
} t5_app_info_t;
T5_MUST_CHECK t5_err_t t5_apps_info(const char *id, t5_app_info_t *out);
/* Change a camera or mic answer (App Manager); granted=false also
 * forgets "never asked", so the app is asked again next time. */
T5_MUST_CHECK t5_err_t t5_apps_set_grant(const char *id, uint32_t perm, bool granted);
T5_MUST_CHECK t5_err_t t5_apps_forget_grant(const char *id, uint32_t perm);
/* Clear the crash record (and the auto-launch block). */
T5_MUST_CHECK t5_err_t t5_apps_crash_reset(const char *id);

/* Per-app average draw (plan 6.8), highest first. See t5_app/energy.h. */
#include "t5_app/energy.h"
int t5_apps_energy(t5_energy_rec_t *out, int max);
void t5_apps_energy_reset(void);

/* Install a .t5app (plan 5.8): validates the manifest and, for native
 * apps, that every symbol the ELF imports is in the ABI (plan 5.6).
 * dest: 0 auto, 1 SD, 2 /data. */
T5_MUST_CHECK t5_err_t t5_apps_install(const char *path, int dest, char *err, size_t errlen);
T5_MUST_CHECK t5_err_t t5_apps_remove(const char *id, bool keep_data);
/* The ABI check alone. missing gets a comma-separated list. */
T5_MUST_CHECK t5_err_t t5_apps_check_abi(const uint8_t *elf, size_t len, char *missing, size_t n);

bool t5_apps_autolaunch_allowed(const char *id);
int t5_apps_crashes(t5_crash_rec_t *out, int max);
/* Where an app's last core dump was saved, if any. */
T5_MUST_CHECK t5_err_t t5_apps_coredump_path(const char *id, char *path, size_t n);

/* Run an app's on_alarm headless (plan 5.10), bounded by its manifest's
 * max_seconds. Blocks the caller. */
T5_MUST_CHECK t5_err_t t5_apps_run_alarm(const char *id);

/* Services call this before using camera or mic on an app's behalf
 * (Phase 7). System callers always pass. */
T5_MUST_CHECK t5_err_t t5_perm_check(uint32_t perm);

/* Diagnostics: the watchdog's dialog drawn over a healthy UI (it is only
 * rendered once an app has started). */
T5_MUST_CHECK t5_err_t anr_test_show(void);
T5_MUST_CHECK t5_err_t anr_test_hide(void);
/* Diagnostics: press "Close app" while the UI task is stuck in an app
 * (T5_ERR_STATE if it isn't). Returns if the app was stopped without a
 * restart; otherwise the device restarts. */
T5_MUST_CHECK t5_err_t anr_test_close(void);
/* Faults survived without a reboot since boot (t5_core/fault.h). */
uint32_t t5_apps_faults_survived(void);

/* Close the file picker as if the person had backed out: true if one was
 * open. For the home gesture. */
bool t5_apps_modal_dismiss(void);
/* May app use the normalised or raw path abs? May ask the person (not on the
 * UI task: there it asks and answers no for this call). */
bool t5_app_fs_check(t5_app_t *app, const char *abs, bool write);
/* Would writing n more bytes keep the app inside its data-dir quota? */
bool t5_app_fs_room(t5_app_t *app, size_t n);

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
