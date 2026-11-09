/* SPDX-License-Identifier: Apache-2.0
 * USB diagnostics for the console (not part of the app SDK).
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How far the last USB-C hand-back got (0x7B00000n, step n), kept across a
 * reset: a hang there leaves the step it stopped at. */
uint32_t t5_usbc_trace(void);

#ifdef __cplusplus
}
#endif
