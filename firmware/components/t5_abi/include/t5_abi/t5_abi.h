/* SPDX-License-Identifier: Apache-2.0
 * The generated app ABI table (sdk/abi/api_v1.sym). Linking it pulls every
 * exported function into the image; t5_abi_register() hands it to the app
 * loader (t5_app/abi.h). Call once at boot, before t5_apps_init().
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void t5_abi_register(void);

#ifdef __cplusplus
}
#endif
