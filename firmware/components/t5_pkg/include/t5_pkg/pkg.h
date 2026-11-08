/* SPDX-License-Identifier: Apache-2.0
 * Package installer and registry (filesystem only).
 *
 * Layout, per root (SD "/sd/apps" or internal "/data/apps"):
 *   <root>/<id>/            installed app (zip contents, incl. manifest.toml)
 *   <root>/<id>/.install    install record: dest, size, time, version
 *   <root>/<id>~new/        staging during install
 *   <root>/<id>~old/        previous version during the swap
 *   <root>/<id>~del/        being removed
 * App data: <appdata>/<id>/ (never touched by install or upgrade), with
 * permission grants in <appdata>/<id>/.grants.
 *
 * The staging suffixes use '~' rather than '.' because '.' is legal in an
 * id: "com.foo.new" is a valid app, so "<id>.new" would be ambiguous.
 *
 * A power cut at any point leaves either the old or the new install once
 * t5_pkg_recover() has run (call it at boot, after storage is mounted).
 * Not thread-safe: the App Manager serialises calls.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"
#include "t5_pkg/manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_PKG_PATH_MAX 192

typedef enum {
    T5_PKG_DEST_AUTO,        /* existing install's root, else SD if usable, else DATA */
    T5_PKG_DEST_SD,
    T5_PKG_DEST_DATA,
} t5_pkg_dest_t;

enum {
    T5_PKG_ALLOW_DOWNGRADE = 1u << 0,
};

typedef struct {
    t5_manifest_t manifest;
    t5_pkg_dest_t root;               /* T5_PKG_DEST_SD or T5_PKG_DEST_DATA */
    char dir[T5_PKG_PATH_MAX];        /* install dir */
    char data_dir[T5_PKG_PATH_MAX];   /* <appdata>/<id> (may not exist yet) */
    char entry_path[T5_PKG_PATH_MAX]; /* absolute path of the entry file */
    uint64_t size;                    /* installed bytes */
    int64_t install_time;             /* time() at install */
    bool shadowed;                    /* also installed on the other root; this one (SD) wins */
} t5_pkg_info_t;

/* Roots; NULL keeps the current value. Defaults: /sd/apps, /data/apps, /data/appdata. */
void t5_pkg_set_roots(const char *sd_apps, const char *data_apps, const char *data_appdata);

/* Validate a .t5app without installing: manifest.toml at the root, valid,
 * entry (and icon, if named) present in the zip. */
T5_MUST_CHECK t5_err_t t5_pkg_inspect(const char *path, t5_manifest_t *out, char *err, size_t errlen);

/* Install or upgrade. Reinstalling the same version is allowed; an older
 * version is refused with T5_ERR_DENIED unless T5_PKG_ALLOW_DOWNGRADE.
 * If the app was on the other root it moves (the old copy is removed). */
T5_MUST_CHECK t5_err_t t5_pkg_install(const char *path, t5_pkg_dest_t dest, t5_pkg_info_t *out, char *err, size_t errlen);
T5_MUST_CHECK t5_err_t t5_pkg_install_ex(const char *path, t5_pkg_dest_t dest, uint32_t flags, t5_pkg_info_t *out,
                           char *err, size_t errlen);

/* Remove from every root. T5_ERR_NOT_FOUND if it was not installed (data is
 * still removed when keep_data is false). */
T5_MUST_CHECK t5_err_t t5_pkg_remove(const char *id, bool keep_data);

/* SD first, then DATA. */
T5_MUST_CHECK t5_err_t t5_pkg_find(const char *id, t5_pkg_info_t *out);
/* Fill up to max entries (SD first); returns how many were stored. */
int t5_pkg_list(t5_pkg_info_t *out, int max);

/* Finish or roll back interrupted installs/removals on both roots.
 * Returns the number of directories repaired or deleted. */
int t5_pkg_recover(void);

/* Permission grants. Missing file: T5_ERR_NOT_FOUND with both outputs 0. */
T5_MUST_CHECK t5_err_t t5_pkg_grants_load(const char *id, uint32_t *granted, uint32_t *denied);
T5_MUST_CHECK t5_err_t t5_pkg_grants_save(const char *id, uint32_t granted, uint32_t denied);

#ifdef __cplusplus
}
#endif
