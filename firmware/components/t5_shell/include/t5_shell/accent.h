/* SPDX-License-Identifier: Apache-2.0
 * The accent colour the person picked (Settings > Display), kept across
 * reboots. UI task (md calls).
 */
#pragma once
#include "md/md_tokens.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Restyle everything in accent and save it. */
void t5_shell_accent_set(md_accent_t accent);
/* Apply the saved accent, if there is one (at boot). */
void t5_shell_accent_restore(void);

#ifdef __cplusplus
}
#endif
