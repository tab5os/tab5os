/* SPDX-License-Identifier: Apache-2.0
 * Updates over Wi-Fi: the published releases (T5_OTA_RELEASES_API in
 * ota_rule.h) are checked about once a day, and on request, for one newer
 * than the running system. Nothing installs by itself: Settings > Updates
 * shows what was found and installs it on a tap, through t5_ota_install_url
 * as T5_OTA_FROM_SETTINGS without consent, so only an image signed by a
 * built-in key, and not older than this system, goes in.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    T5_UPDATE_UNKNOWN,      /* not checked yet */
    T5_UPDATE_CHECKING,
    T5_UPDATE_CURRENT,      /* no newer release */
    T5_UPDATE_AVAILABLE,    /* version and url name it */
    T5_UPDATE_INSTALLING,   /* t5_ota_status() has the progress */
    T5_UPDATE_FAILED,       /* error says why (the check or the install) */
} t5_update_state_t;

typedef struct {
    t5_update_state_t state;
    char version[32];       /* the newer release's */
    char url[192];          /* its .t5os */
    char error[96];
    int64_t checked_us;     /* t5_time_us() of the last check that finished, 0 never */
} t5_update_status_t;

/* Start the daily checks: the first a couple of minutes after Wi-Fi first
 * connects, then about every 24 hours while it's connected. found (may be
 * NULL) is called on a system task once for each newer version found. */
T5_MUST_CHECK t5_err_t t5_update_start(void (*found)(const char *version));
void t5_update_status(t5_update_status_t *out);
/* Check now, in the background (T5_ERR_STATE if a check or install runs). */
T5_MUST_CHECK t5_err_t t5_update_check_now(void);
/* Install the release the last check found, in the background
 * (T5_ERR_STATE if there is none, or something runs already). */
T5_MUST_CHECK t5_err_t t5_update_install(void);

#ifdef __cplusplus
}
#endif
