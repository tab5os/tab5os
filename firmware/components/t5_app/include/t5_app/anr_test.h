/* SPDX-License-Identifier: Apache-2.0
 * The watchdog's dialog, for the console's diagnostics.
 */
#pragma once

#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Draw the dialog over a healthy UI (it is only rendered once an app has
 * started). */
T5_MUST_CHECK t5_err_t anr_test_show(void);
T5_MUST_CHECK t5_err_t anr_test_hide(void);
/* Press "Close app" while the UI task is stuck in an app (T5_ERR_STATE if
 * it isn't). Returns if the app was stopped without a restart; otherwise the
 * device restarts. */
T5_MUST_CHECK t5_err_t anr_test_close(void);

#ifdef __cplusplus
}
#endif
