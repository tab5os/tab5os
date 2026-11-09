/* SPDX-License-Identifier: Apache-2.0
 * The standby screen, for the OS (Settings, the console; not in the SDK).
 * See t5_shell/shell.h for what it shows.
 *
 * It replaces Dim while USB power is in, if enabled (the default): the
 * shell holds a screen wake lock and caps the backlight low (lower still
 * from 22:00 to 07:00) until a tap or unplugging. Apps keep running under
 * it, so music plays on.
 */
#pragma once
#include <stdbool.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

bool t5_shell_standby_enabled(void);
void t5_shell_set_standby_enabled(bool on);   /* saved */
/* Show or leave it now (the console; any task). */
t5_err_t t5_shell_standby(bool on);
bool t5_shell_standby_shown(void);

#ifdef __cplusplus
}
#endif
