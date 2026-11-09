/* SPDX-License-Identifier: Apache-2.0
 * A screenshot on the console, in the format tools/t5/t5img.py reads:
 *
 *   rotation R          the UI's rotation in degrees (left out when the
 *                       caller passes a negative one; the pixels are native)
 *   SHOT W H
 *   base64 lines        RGB565 little-endian, 48 bytes (64 characters) a line
 *   END
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* w x h pixels taken every step pixels from px, stride pixels a row (a whole
 * framebuffer scaled down, or a buffer at its own size: step 1). */
void t5_display_print_shot(const uint16_t *px, int w, int h, int stride, int step, int rotation);

#ifdef __cplusplus
}
#endif
