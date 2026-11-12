/* SPDX-License-Identifier: Apache-2.0
 * Keeping the C6 radio's firmware matched to the OS.
 *
 * Each OS expects the C6 firmware it was built with: "t5-c6-" and
 * c6/VERSION (T5_OTA_C6_PREFIX in t5_ota/ota.h), which is raised only when
 * the C6 image changes, so most updates leave the C6 alone. Once a boot is
 * confirmed (not on trial, not safe mode) and the C6 is up, the sync:
 *   1. stops if the C6 already runs that version;
 *   2. otherwise, if the c6fw partition doesn't hold it, takes the release's
 *      .t5c6 from the SD card (tab5os-c6-<c6 version>.t5c6 in the top folder
 *      or /update) or, with Wi-Fi, from the GitHub release of this OS version
 *      (every release publishes the C6 firmware it was built with),
 *      and writes it to c6fw (t5_ota_c6_install_stream, as
 *      T5_OTA_FROM_SETTINGS without consent: signed by a built-in key);
 *   3. has t5_net update the C6 from c6fw.
 * Not while an app holds Wi-Fi or Bluetooth exclusively. At most three
 * C6 updates per version (counted in /data/sys), and one fetch per boot, so
 * a failure can't repeat forever. A build with no published release (a
 * development build) finds nothing to fetch and leaves the C6 alone:
 * `t5 c6-stage` and `net update` stay the way to put a development C6 image on.
 */
#pragma once
#include <stddef.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

T5_MUST_CHECK t5_err_t t5_c6_sync_start(void);
/* One line on where the sync is ("C6 runs t5-c6-0.2.0", "fetching ...", ...). */
void t5_c6_sync_status(char *out, size_t n);
/* Forget this boot's outcome and the attempt count, and run again. */
T5_MUST_CHECK t5_err_t t5_c6_sync_retry(void);

#ifdef __cplusplus
}
#endif
