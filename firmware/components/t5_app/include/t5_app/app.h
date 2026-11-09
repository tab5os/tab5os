/* SPDX-License-Identifier: Apache-2.0
 * The app-facing API (plan 5.10): what an app's code calls. Part of the
 * public SDK (sdk/include/t5/app.h includes this file).
 *
 *   #include <t5/app.h>
 *   #include <t5/ui.h>
 *
 *   static void on_create(t5_app_t *app) {
 *       lv_obj_t *label = lv_label_create(t5_app_screen(app));
 *       lv_label_set_text(label, "Hello, Tab5");
 *       lv_obj_center(label);
 *   }
 *   T5_APP(.on_create = on_create);
 *
 * Threading: every callback runs on the UI task, so LVGL calls are safe in
 * them. Tasks an app creates (t5_task_create or xTaskCreate) must use
 * t5_ui_async() or t5_ui_lock()/t5_ui_unlock() for LVGL.
 *
 * Cleanup: the OS tracks the app's heap, tasks, LVGL timers and animations,
 * port claims, rails and wake locks, and frees whatever is left when the
 * app exits or crashes. Everything an app draws must live under
 * t5_app_screen() (or on lv_layer_top(), which is tracked too).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "t5_core/err.h"
#include "t5_pkg/manifest.h"     /* T5_API_LEVEL, T5_PERM_* */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct t5_app t5_app_t;
struct _lv_obj_t;

typedef struct {
    uint32_t api;                                   /* T5_API_LEVEL the app was built with */
    void (*on_create)(t5_app_t *app);               /* launched: allocate, build UI */
    void (*on_resume)(t5_app_t *app);               /* foreground or device awake: reopen hardware */
    void (*on_suspend)(t5_app_t *app);              /* background or sleep: save, stop timers. 500 ms */
    void (*on_destroy)(t5_app_t *app);              /* exiting: final cleanup; the OS reclaims the rest */
    void (*on_rotate)(t5_app_t *app, int degrees);  /* orientation = "any" apps: 0, 90, 180, 270 */
    void (*on_alarm)(t5_app_t *app);                /* headless scheduled wake ([background] in the manifest) */
} t5_app_desc_t;

/* Native apps: the loader calls t5_app_entry() (the ELF entry point) and
 * reads the descriptor through argv[0]. Built-in apps register the
 * descriptor directly. */
#define T5_APP_ENTRY_SYMBOL "t5_app_entry"
#define T5_APP(...)                                                                     \
    static const t5_app_desc_t t5__app_desc = { .api = T5_API_LEVEL, __VA_ARGS__ };     \
    __attribute__((visibility("default"))) int t5_app_entry(int argc, char **argv)      \
    {                                                                                   \
        if (argc >= 1 && argv && argv[0]) *(const t5_app_desc_t **)argv[0] = &t5__app_desc; \
        return T5_API_LEVEL;                                                            \
    }

/* The app's root screen. Delete nothing above it; it is deleted on exit. */
struct _lv_obj_t *t5_app_screen(t5_app_t *app);
const char *t5_app_id(t5_app_t *app);
/* Writable directory kept across upgrades: /data/appdata/<id>. */
const char *t5_app_data_dir(t5_app_t *app);
/* Read-only directory the package was installed into (assets). */
const char *t5_app_install_dir(t5_app_t *app);
void t5_app_set_user(t5_app_t *app, void *user);
void *t5_app_get_user(t5_app_t *app);
/* The running app of the calling task, or NULL for system code. */
t5_app_t *t5_app_self(void);

/* The argument the app was launched with (t5_apps_launch_arg), "" if none. */
const char *t5_app_launch_arg(t5_app_t *app);

/* Session restore (plan 6.10): after a soft off the OS reopens the last app
 * (not after an alarm wake, a crash or a hang). Save what the person was
 * doing in on_suspend; on_create can check t5_app_restored() and load it.
 * The state lives in the app's data dir and is written atomically. */
#define T5_APP_STATE_MAX 8192
bool t5_app_restored(t5_app_t *app);
T5_MUST_CHECK t5_err_t t5_app_state_save(t5_app_t *app, const void *data, size_t len);
T5_MUST_CHECK t5_err_t t5_app_state_load(t5_app_t *app, void *buf, size_t cap, size_t *len);

/* Ask to exit. Runs on_suspend, on_destroy and the cleanup after the
 * current callback returns. */
void t5_app_exit(t5_app_t *app);
/* For loops in app tasks: false once the app is exiting. */
bool t5_app_running(t5_app_t *app);

/* Permissions (plan 5.9). Declared groups are granted at install, except
 * camera and mic, which also ask the person the first time they are used.
 * request() shows that prompt (UI task, non-blocking); cb gets the answer. */
typedef void (*t5_perm_cb_t)(t5_app_t *app, uint32_t perm, bool granted, void *user);
T5_MUST_CHECK t5_err_t t5_app_request_permission(t5_app_t *app, uint32_t perm, t5_perm_cb_t cb, void *user);
bool t5_app_has_permission(t5_app_t *app, uint32_t perm);

/* Files. An app can always use its data dir (t5_app_data_dir, up to 1 MB) and
 * read its install dir. A file the person chooses here is open to the app
 * until it exits, with no permission needed. The picker is a large modal
 * over the app: cb gets the path, or NULL if the person backed out. It
 * isn't called if the app exits first. UI task; one picker at a time.
 * exts: "jpg,png", or NULL for any file. */
#define T5_FILE_PICK_WRITE 1u    /* the app may also change the chosen file */
typedef void (*t5_file_pick_cb_t)(t5_app_t *app, const char *path, void *user);
T5_MUST_CHECK t5_err_t t5_file_pick(t5_app_t *app, const char *title, const char *exts, uint32_t flags,
                                    t5_file_pick_cb_t cb, void *user);

/* Wake the device from soft off at utc and run on_alarm headless, then go
 * back to soft off (needs [background] and background_wake). */
T5_MUST_CHECK t5_err_t t5_app_schedule_alarm(t5_app_t *app, time_t utc);

#ifdef __cplusplus
}
#endif
