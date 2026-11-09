/* SPDX-License-Identifier: Apache-2.0
 * t5_pkg internals the host tests drive directly (test/host/test_pkg.c).
 * Not part of the OS's API.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "t5_pkg/inflate.h"
#include "t5_pkg/pkg.h"
#include "t5_pkg/zip.h"

enum {
    T5_PKG_ALLOW_DOWNGRADE = 1u << 0,
};

/* Roots; NULL keeps the current value. Defaults: T5_APPS_SD_DIR,
 * T5_APPS_DATA_DIR, T5_APPDATA_DIR. */
void t5_pkg_set_roots(const char *sd_apps, const char *data_apps, const char *data_appdata);
/* t5_pkg_install with flags. */
T5_MUST_CHECK t5_err_t t5_pkg_install_ex(const char *path, t5_pkg_dest_t dest, uint32_t flags, t5_pkg_info_t *out,
                                         char *err, size_t errlen);

/* Over an already open file (own_file: closed by t5_zip_close). */
T5_MUST_CHECK t5_err_t t5_zip_open_file(FILE *f, bool own_file, t5_zip_t **out);
/* Stream an entry to a sink (same contract as the inflate sink). */
T5_MUST_CHECK t5_err_t t5_zip_extract_cb(t5_zip_t *z, const t5_zip_entry_t *e, t5_inflate_write_fn wr, void *ctx);

/* Decode src into dst. T5_ERR_NO_MEM if the output does not fit in dst_cap. */
T5_MUST_CHECK t5_err_t t5_inflate_mem(const void *src, size_t src_len, void *dst, size_t dst_cap, size_t *out_len);
