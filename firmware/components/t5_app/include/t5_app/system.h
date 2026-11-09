/* SPDX-License-Identifier: Apache-2.0
 * The part of the app runtime that apps with the system permission may call
 * (sdk/include/t5/system.h): the app list, launching apps, per-app power.
 * The rest of the OS side is t5_app/apps.h.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_app/energy.h"
#include "t5_app/registry.h"
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Safe from any task: the work runs on the UI task. */
T5_MUST_CHECK t5_err_t t5_apps_launch(const char *id);
/* Launch with an argument the app reads with t5_app_launch_arg() (a path,
 * a settings page, an app id). If the app is already in front with another
 * argument it is restarted. */
T5_MUST_CHECK t5_err_t t5_apps_launch_arg(const char *id, const char *arg);

typedef struct {
    char id[T5_APP_ID_LEN];
    char name[T5_APP_NAME_LEN];
    char version[T5_APP_VERSION_LEN];
    bool builtin;
    bool running;
    bool autolaunch_blocked;
    uint16_t crashes;               /* consecutive */
    char icon[T5_APP_ICON_LEN];     /* MD_ICON_* glyph, "" for none */
    uint32_t perms;                 /* T5_PERM_* declared */
} t5_app_entry_t;
int t5_apps_list(t5_app_entry_t *out, int max);

/* App Manager (plan 6.7): everything known about one app. */
typedef struct {
    t5_app_entry_t entry;
    char install_dir[T5_APP_PATH_MAX];
    char data_dir[T5_APP_PATH_MAX];
    uint64_t install_bytes;           /* package size, 0 for built-ins */
    uint64_t data_bytes;              /* app data dir, recursive */
    bool on_sd;
    int64_t install_time;
    uint32_t granted, denied;         /* runtime answers for camera and mic */
    uint16_t crashes_total;
    uint8_t last_crash_kind;          /* t5_crash_kind_t */
    int64_t last_crash_time;
    bool has_dump;
    char dump_path[T5_APP_PATH_MAX];
    char runtime[12];                 /* "native", "micropython", "built-in" */
} t5_app_info_t;
T5_MUST_CHECK t5_err_t t5_apps_info(const char *id, t5_app_info_t *out);

/* Per-app average draw (plan 6.8), highest first. See t5_app/energy.h. */
int t5_apps_energy(t5_energy_rec_t *out, int max);

#ifdef __cplusplus
}
#endif
