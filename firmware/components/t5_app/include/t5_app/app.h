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
#include <time.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* API level this header describes. An app built against level N runs on any
 * OS whose level is >= N (sdk/abi/api_v<N>.sym never loses or changes a
 * symbol). */
#define T5_API_LEVEL 1

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

/* Ask to exit. Runs on_suspend, on_destroy and the cleanup after the
 * current callback returns. */
void t5_app_exit(t5_app_t *app);
/* For loops in app tasks: false once the app is exiting. */
bool t5_app_running(t5_app_t *app);

/* Permissions (plan 5.9). Declared groups are granted at install, except
 * camera and mic, which also ask the person the first time they are used.
 * request() shows that prompt (UI task, non-blocking); cb gets the answer. */
typedef void (*t5_perm_cb_t)(t5_app_t *app, uint32_t perm, bool granted, void *user);
t5_err_t t5_app_request_permission(t5_app_t *app, uint32_t perm, t5_perm_cb_t cb, void *user);
bool t5_app_has_permission(t5_app_t *app, uint32_t perm);

/* Wake the device from soft off at utc and run on_alarm headless, then go
 * back to soft off (needs [background] and background_wake). */
t5_err_t t5_app_schedule_alarm(t5_app_t *app, time_t utc);

#ifdef __cplusplus
}
#endif
