/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "lvgl.h"
#include "t5_app/apps.h"
#include "t5_app/fspolicy.h"
#include "t5_app/tracker.h"
#include "t5_core/fault.h"
#include "t5_core/os.h"
#include "t5_core/res.h"
#include "t5_pkg/manifest.h"
#include "t5_pkg/pkg.h"

#define T5_APP_MAX_TASKS 16      /* tasks one app may have */

typedef enum {
    APP_LOADING,
    APP_CREATED,
    APP_RESUMED,
    APP_SUSPENDED,
    APP_EXITING,
} app_state_t;

struct t5_app {
    t5_app_ctx_hdr_t hdr;           /* first: services read the id through it */
    char id[T5_APP_ID_LEN];
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
    char arg[128];                  /* t5_apps_launch_arg */
    bool restored;                  /* launched by session restore */
    /* isolation (isolate.h) */
    uintptr_t text_lo, text_hi;     /* native code, for blaming faults */
    volatile int file_calls;        /* filesystem calls in progress (wrappers.c) */
    volatile bool net_closed;       /* the exit closed its sockets (app_close_sockets) */
    volatile bool faulted;          /* a fault or a stopped hang ended it */
    volatile bool oom;              /* closed for memory */
    uint32_t instance;              /* unique among all launches since boot */
    /* files (fspolicy.h) */
    t5_fs_pick_t picks[T5_FS_MAX_PICKS];
    uint8_t npicks, pick_next;
    int64_t fs_used;                /* data dir bytes, grows with writes; <0: not counted yet */
    uint32_t fs_since_check;        /* bytes since the last look at free space */
    FILE *qfiles[12];               /* open for writing in the data dir */
    t5_fault_info_t fault;
};

/* Runtime-wide lock for trackers (app tasks allocate concurrently). */
void apps_lock(void);
void apps_unlock(void);
/* At exit: close the app's own sockets, waking its tasks blocked on them. */
int app_close_sockets(t5_app_t *a);
void apps_fs_add_pick(t5_app_t *a, const char *path, bool write);
void picker_app_gone(t5_app_t *app);
bool picker_dismiss(void);
void picker_init(void);
/* Any one of the permissions in mask; asks if it can. */
t5_err_t perm_check_any(t5_app_t *a, uint32_t mask);
/* The running app of that launch, NULL once it has exited or is exiting. */
t5_app_t *apps_by_instance(uint32_t instance);
/* The foreground (or headless) app, for wrappers called without a context. */
t5_app_t *apps_fallback(void);

/* wrappers.c: how many of the app's own mutexes and semaphores task holds
 * (apps lock held). */
int t5_app_mutexes_held_by(t5_app_t *a, void *task);
/* wrappers.c: drop the app's refs to animations that have finished (apps
 * lock and UI lock held). */
void t5_app_anims_prune(t5_app_t *app);
/* perm.c */
void perm_app_gone(t5_app_t *app);     /* UI task: close its open prompts */
/* Installed as t5_core's permission check, which services call before using
 * the camera or mic on an app's behalf. System callers always pass. */
int perm_check_self(uint32_t perm);
/* wrappers.c: installed as t5_core's path check (the file policy for the
 * calling app). */
bool apps_path_check(const char *path, bool write);
/* anr.c */
t5_err_t anr_init(void);
void anr_app_changed(t5_app_t *app);
/* lifecycle.c: the registered home screen */
lv_obj_t *apps_home_screen(void);
void apps_home_refresh(void);
/* energy accounting (lifecycle.c) */
void apps_energy_sample(void);
/* lifecycle.c: the ANR dialog's Close app. Stops the app without a reboot
 * when its code can be left; otherwise records the hang and reboots. */
void apps_restart_after_anr(t5_app_t *app);
/* lifecycle.c: close the foreground app because memory is critically low
 * (any task). */
void apps_close_for_memory(void);
/* lifecycle.c: move app tasks that hog a core down to the lowest priority
 * (the watchdog calls it once a second). */
void apps_check_hogs(void);
void apps_throttle_tick(void);
