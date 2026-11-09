/* SPDX-License-Identifier: Apache-2.0
 * Where things live on the device. Every service names its files through
 * these, so the layout is defined once.
 */
#pragma once

#include <stddef.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_DATA_PATH "/data"
#define T5_SD_PATH "/sd"
#define T5_SYS_DIR T5_DATA_PATH "/sys"     /* system settings and records */
#define T5_USER_DIR T5_DATA_PATH "/user"   /* the person's own files (scripts, Terminal) */
#define T5_TMP_DIR T5_DATA_PATH "/tmp"     /* uploads in progress */
#define T5_USB_PATH "/usb"                 /* a USB drive, while one is plugged in */
#define T5_DCIM_DIR T5_SD_PATH "/DCIM"     /* the Camera's photos and videos */

/* Installed apps, on the SD card or internally (t5_pkg); every app's data
 * stays internal, whichever its install went to. */
#define T5_APPS_SD_DIR T5_SD_PATH "/apps"
#define T5_APPS_DATA_DIR T5_DATA_PATH "/apps"
#define T5_APPDATA_DIR T5_DATA_PATH "/appdata"

/* Port module profiles (t5_ports): .json files read from both. */
#define T5_PROFILES_DATA_DIR T5_DATA_PATH "/profiles"
#define T5_PROFILES_SD_DIR T5_SD_PATH "/profiles"

/* The app runtime's crash records and app core dumps, in the system records
 * directory (t5_sys_path). The recovery image saves them to the SD card. */
#define T5_CRASH_DB_NAME "crashes.bin"
#define T5_DUMP_DIR_NAME "dumps"

/* The system records directory: T5_SYS_DIR, or a temporary directory that
 * host tests set. Every file in it is named through t5_sys_path(). */
const char *t5_sys_dir(void);
void t5_sys_dir_set(const char *dir);
/* "<sys dir>/<name>". T5_ERR_INVALID_ARG if it doesn't fit. */
t5_err_t t5_sys_path(char *out, size_t cap, const char *name);

#ifdef __cplusplus
}
#endif
