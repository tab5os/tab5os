/* SPDX-License-Identifier: Apache-2.0
 * HTTP(S) GET downloads, for updates and the console's `app fetch`, on
 * t5_http (t5_net/http.h): redirects followed, server certificates checked
 * against ESP-IDF's bundle, and a NET wake lock held while one is open.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct t5_download t5_download_t;

/* GET url and read the response headers. Anything but a 200 is an error.
 * len (may be NULL): the body's length, or <= 0 when the server didn't say. */
T5_MUST_CHECK t5_err_t t5_download_open(const char *url, t5_download_t **out, int64_t *len, char *err,
                                        size_t errlen);
/* Up to n bytes of the body into buf: the count, 0 at the end, or a
 * negative t5_err_t (the shape of t5_ota_read_fn; ctx is the download). */
int t5_download_read(void *dl, void *buf, size_t n);
void t5_download_close(t5_download_t *dl);

/* The whole body into a file. bytes (may be NULL) gets its size. */
T5_MUST_CHECK t5_err_t t5_download_file(const char *url, const char *path, int64_t *bytes, char *err,
                                        size_t errlen);

#ifdef __cplusplus
}
#endif
