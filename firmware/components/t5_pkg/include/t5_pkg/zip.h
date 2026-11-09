/* SPDX-License-Identifier: Apache-2.0
 * Minimal zip reader for .t5app packages: central directory listing and
 * extraction of stored (0) and deflated (8) entries with CRC-32 and size
 * checks. Opening fails for anything a package should never contain:
 * zip64, encrypted entries, multi-disk archives, absolute paths, "..",
 * backslashes, control characters or names of T5_ZIP_NAME_MAX bytes or more.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "t5_core/err.h"
#include "t5_pkg/inflate.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_ZIP_NAME_MAX 128        /* including the terminator */
#define T5_ZIP_MAX_ENTRIES 4096
#define T5_ZIP_MAX_CDIR (1024 * 1024)

typedef struct {
    const char *name;        /* NUL-terminated, owned by the archive */
    uint16_t method;         /* 0 stored, 8 deflate; others list but don't extract */
    uint16_t flags;
    uint32_t crc32;
    uint32_t comp_size;
    uint32_t size;           /* uncompressed */
    uint32_t local_offset;
    bool is_dir;             /* name ends with '/' */
} t5_zip_entry_t;

typedef struct t5_zip t5_zip_t;

T5_MUST_CHECK t5_err_t t5_zip_open(const char *path, t5_zip_t **out);
void t5_zip_close(t5_zip_t *z);

int t5_zip_count(const t5_zip_t *z);
const t5_zip_entry_t *t5_zip_entry(const t5_zip_t *z, int index);
const t5_zip_entry_t *t5_zip_find(const t5_zip_t *z, const char *name);

/* Extract to a caller buffer. T5_ERR_NO_MEM if e->size > cap. */
T5_MUST_CHECK t5_err_t t5_zip_extract_mem(t5_zip_t *z, const t5_zip_entry_t *e, void *buf, size_t cap, size_t *out_len);
/* Extract into a new malloc'd buffer of e->size + 1 bytes, NUL-terminated
 * (handy for text). Refuses entries larger than max_size. Free with free(). */
T5_MUST_CHECK t5_err_t t5_zip_extract_alloc(t5_zip_t *z, const t5_zip_entry_t *e, size_t max_size, char **out,
                              size_t *out_len);
/* Extract to a file (created or truncated, fsync'd). The file is removed on
 * any failure, including a CRC mismatch. */
T5_MUST_CHECK t5_err_t t5_zip_extract_file(t5_zip_t *z, const t5_zip_entry_t *e, const char *path);

/* Relative, forward-slash path with no "", "." or ".." components, no
 * backslash, colon or control characters, shorter than T5_ZIP_NAME_MAX.
 * A single trailing '/' (directory) is allowed. */
bool t5_zip_name_ok(const char *name, size_t len);

#ifdef __cplusplus
}
#endif
