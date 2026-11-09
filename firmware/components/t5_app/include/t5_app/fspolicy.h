/* SPDX-License-Identifier: Apache-2.0
 * Which files an app may touch. Pure, host-tested; the device side is in
 * wrappers.c and t5_py/fs.c, and the prompts are in perm.c.
 *
 *   its data dir               read and write, always
 *   its install dir            read only, always
 *   a file picked by the user  that file only, for this run of the app
 *   shared folders             with shared_read or shared_write
 *   the rest of /sd, /usb      with all_files
 *   OS files, other apps       never
 *
 * For native apps this stops mistakes, not attackers: they share the OS's
 * address space (T5_NATIVE_APP_WARNING).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_pkg/manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_FS_PATH_MAX T5_APP_PATH_MAX
#define T5_FS_MAX_PICKS 8

typedef struct {
    char path[T5_FS_PATH_MAX];
    bool write;
} t5_fs_pick_t;

typedef struct {
    const char *data_dir;
    const char *install_dir;
    const t5_fs_pick_t *picks;
    int npicks;
} t5_fs_scope_t;

/* The places with files of the person's own: all_files reaches them, and
 * the picker starts there. */
typedef struct {
    const char *path;
    const char *label;
} t5_fs_root_t;
extern const t5_fs_root_t t5_fs_roots[];
extern const size_t t5_fs_nroots;

typedef enum {
    T5_FS_ALLOW,
    T5_FS_DENY,
    T5_FS_NEEDS_PERM,       /* *need holds the permissions, any one will do */
} t5_fs_verdict_t;

/* Absolute path, duplicate slashes and "." removed. False for a relative
 * path, "..", one too long, or a name FAT would quietly read as another
 * (trailing dot or space, 8.3 alias), which could dodge the checks. */
bool t5_fs_normalise(const char *in, char *out, size_t n);

/* abs must be normalised. Case doesn't matter: FAT ignores it. */
t5_fs_verdict_t t5_fs_check(const t5_fs_scope_t *s, const char *abs, bool write, uint32_t *need);

/* Is abs at or under root? Case-insensitive. */
bool t5_fs_under(const char *abs, const char *root);
/* OS files and apps' private folders: never listed in the picker. */
bool t5_fs_protected(const char *abs);

/* Quota on an app's data dir: may used grow by n? */
#define T5_FS_QUOTA_BYTES (1024u * 1024u)
bool t5_fs_quota_ok(uint64_t used, size_t n);

/* Should this file be offered? exts: "jpg,png"; NULL or "" for any. */
bool t5_fs_ext_matches(const char *name, const char *exts);

#ifdef __cplusplus
}
#endif
