/* SPDX-License-Identifier: Apache-2.0
 * App runtime (plan 5.4–5.12): launching, the lifecycle, tracking and
 * cleanup, native ELF loading, permissions, the crash guard and the UI
 * watchdog. This is the OS side; apps see t5_app/app.h, and with the
 * system permission t5_app/system.h.
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
#include "t5_app/system.h"
#include "t5_core/err.h"
#include "t5_core/res.h"
#include "t5_pkg/pkg.h"

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
T5_MUST_CHECK t5_err_t t5_apps_set_runtime(t5_runtime_t runtime, const t5_app_desc_t *desc);
/* For a runtime: the installed entry file, the [background] entry name and
 * the manifest's min_heap in bytes (0 if unset). */
const char *t5_apps_entry_path(t5_app_t *app);
const char *t5_apps_bg_entry(t5_app_t *app);
size_t t5_apps_min_heap(t5_app_t *app);

/* What the runtime tells the person about; the home screen words it. */
typedef enum {
    T5_APPS_NOTICE_SAFE_MODE,       /* an installed app was not launched: safe mode */
    T5_APPS_NOTICE_CRASHED,         /* the app crashed and was closed */
    T5_APPS_NOTICE_HUNG,            /* it stopped responding and was closed */
    T5_APPS_NOTICE_LOW_MEMORY,      /* it was closed because memory ran low */
} t5_apps_notice_t;

/* The home screen (plan 6.3). The shell registers it before t5_apps_init(),
 * which fails without one. All hooks run on the UI task.
 *   screen   the home screen object (created on first call)
 *   refresh  the app list changed (install, removal, crash counts)
 *   prepare  an app's root screen was just created (status bar inset)
 *   changed  the foreground changed: an app id, or "" for home
 *   notice   tell the person about app_name (may be NULL)
 * The order the home screen can rely on (the simulator's stand-in runtime,
 * sim/sim_shell_sys.c, keeps it too):
 *   launch  the app's screen is created, prepare(screen, id), changed(id),
 *           the app's on_create fills the screen, then it is loaded
 *   exit    the home screen is loaded, changed(""), then the app's screen
 *           is deleted, then refresh() */
struct _lv_obj_t;
typedef struct {
    struct _lv_obj_t *(*screen)(void);
    void (*refresh)(void);
    void (*prepare)(struct _lv_obj_t *app_screen, const char *id);
    void (*changed)(const char *id);
    void (*notice)(t5_apps_notice_t what, const char *app_name);
} t5_apps_home_t;
void t5_apps_set_home(const t5_apps_home_t *home);

T5_MUST_CHECK t5_err_t t5_apps_exit_current(void);
/* Blocking variants for the console and tests (wait up to timeout_ms). */
T5_MUST_CHECK t5_err_t t5_apps_launch_wait(const char *id, uint32_t timeout_ms);
T5_MUST_CHECK t5_err_t t5_apps_exit_wait(uint32_t timeout_ms);
/* Copies the foreground app's id, "" if the launcher is showing. */
void t5_apps_current(char *id, size_t n);

/* The foreground app's tracked resources. */
typedef struct {
    size_t heap_bytes, heap_peak;
    uint32_t heap_blocks;
    uint32_t res[T5_RES_KIND_COUNT];
    uint32_t dropped;
    uint32_t denied_symbols;        /* imports bound to permission stubs */
} t5_app_usage_t;
T5_MUST_CHECK t5_err_t t5_apps_usage(t5_app_usage_t *out);

/* A permission group as App Manager lists it ("See shared files", "camera"). */
const char *t5_perm_label(uint32_t perm_bit);
/* Change a camera or mic answer (App Manager); granted=false also
 * forgets "never asked", so the app is asked again next time. */
T5_MUST_CHECK t5_err_t t5_apps_set_grant(const char *id, uint32_t perm, bool granted);
/* Clear the crash record (and the auto-launch block). */
T5_MUST_CHECK t5_err_t t5_apps_crash_reset(const char *id);

void t5_apps_energy_reset(void);

/* Install a .t5app (plan 5.8): validates the manifest and, for native
 * apps, that every symbol the ELF imports is in the ABI (plan 5.6). A
 * running copy of the app is closed first; T5_ERR_BUSY if it doesn't stop.
 * Not from the UI task while that app is in front of it. man, if not NULL,
 * gets the installed app's manifest. */
T5_MUST_CHECK t5_err_t t5_apps_install(const char *path, t5_pkg_dest_t dest, t5_manifest_t *man, char *err,
                                       size_t errlen);
/* "sd", "data"; anything else (NULL, "auto") is T5_PKG_DEST_AUTO. */
t5_pkg_dest_t t5_apps_dest_parse(const char *s);
T5_MUST_CHECK t5_err_t t5_apps_remove(const char *id, bool keep_data);
/* The ABI check alone. missing gets a comma-separated list. */
T5_MUST_CHECK t5_err_t t5_apps_check_abi(const uint8_t *elf, size_t len, char *missing, size_t n);

int t5_apps_crashes(t5_crash_rec_t *out, int max);
/* Where an app's last core dump was saved, if any. */
T5_MUST_CHECK t5_err_t t5_apps_coredump_path(const char *id, char *path, size_t n);

/* Run an app's on_alarm headless (plan 5.10), bounded by its manifest's
 * max_seconds. Blocks the caller. */
T5_MUST_CHECK t5_err_t t5_apps_run_alarm(const char *id);

/* Close the file picker as if the person had backed out: true if one was
 * open. For the home gesture. */
bool t5_apps_modal_dismiss(void);
/* May app use the normalised or raw path abs? May ask the person (not on the
 * UI task: there it asks and answers no for this call). */
bool t5_app_fs_check(t5_app_t *app, const char *abs, bool write);
/* Do writes to path count against the app's data-dir quota? */
bool t5_app_fs_quota_applies(t5_app_t *app, const char *path);
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
