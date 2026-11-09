/* SPDX-License-Identifier: Apache-2.0
 * For the OS only (not in the SDK): lending the I2S pads.
 *
 * USB-C mass storage swaps the USB PHYs, and PHY 1's pads are the Tab5's
 * I2S pins. Holding closes both codecs (the mic first) and keeps them
 * closed: open streams stay open but go silent, and system sounds wait.
 * Releasing reopens what was open. Blocks until the codecs are closed;
 * T5_ERR_TIMEOUT if the audio task didn't get there in 100 ms.
 */
#pragma once
#include <stdbool.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

t5_err_t t5_audio_hold_pins(bool hold);

#ifdef __cplusplus
}
#endif
