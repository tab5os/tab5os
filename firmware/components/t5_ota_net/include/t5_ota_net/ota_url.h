/* SPDX-License-Identifier: Apache-2.0
 * Install a .t5os from a URL (http:// or https://, server certificates
 * checked against ESP-IDF's bundle): t5_ota_install_stream fed by a
 * download (t5_net/download.h). See t5_ota/ota.h.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "t5_core/err.h"
#include "t5_ota/ota.h"

#ifdef __cplusplus
extern "C" {
#endif

T5_MUST_CHECK t5_err_t t5_ota_install_url(const char *url, t5_ota_source_t from, bool consent, char *err,
                                          size_t errlen);

#ifdef __cplusplus
}
#endif
