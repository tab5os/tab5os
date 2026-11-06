/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "t5_app/apps.h"
#include "t5_app/tracker.h"
#include "t5_core/os.h"
#include "t5_core/res.h"
#include "t5_pkg/manifest.h"
#include "t5_pkg/pkg.h"

typedef enum {
    APP_LOADING,
    APP_CREATED,
    APP_RESUMED,
    APP_SUSPENDED,
    APP_EXITING,
} app_state_t;

struct t5_app {
    t5_app_ctx_hdr_t hdr;           /* first: services read the id through it */
    char id[64];
    t5_manifest_t man;
    char install_dir[T5_PKG_PATH_MAX];
    char data_dir[T5_PKG_PATH_MAX];
    char entry_path[T5_PKG_PATH_MAX];
    const t5_app_desc_t *desc;
    const t5_builtin_app_t *builtin;
    void *elf;                      /* apps_hw handle, NULL for built-ins */
    lv_obj_t *screen;
    t5_tracker_t trk;
    uint32_t granted, denied;       /* runtime answers for camera/mic */
    uint32_t denied_symbols;
    app_state_t state;
    volatile bool exiting;
    bool headless;
    void *user;
    uint32_t started_ms;
};

/* Runtime-wide lock for trackers (app tasks allocate concurrently). */
void apps_lock(void);
void apps_unlock(void);
/* The foreground (or headless) app, for wrappers called without a context. */
t5_app_t *apps_fallback(void);
/* Releasing a task handle: ask nicely, then delete. */
void apps_task_release(void *task);

/* perm.c */
void perm_prompt(t5_app_t *app, uint32_t perm, t5_perm_cb_t cb, void *user);
/* anr.c */
t5_err_t anr_init(void);
void anr_app_changed(t5_app_t *app);
/* launcher.c */
lv_obj_t *launcher_screen(void);
void launcher_refresh(void);
/* lifecycle.c: record a crash-kind exit for the ANR path and reboot. */
void apps_restart_after_anr(t5_app_t *app);
