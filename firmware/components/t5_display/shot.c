/* SPDX-License-Identifier: Apache-2.0
 * Console screenshots: see shot.h.
 */
#include <stdio.h>
#include "mbedtls/base64.h"
#include "t5_display/shot.h"

#define LINE_BYTES 48

static void line(const uint8_t *b, size_t n)
{
    unsigned char out[LINE_BYTES / 3 * 4 + 1];
    size_t olen = 0;
    mbedtls_base64_encode(out, sizeof out, &olen, b, n);
    out[olen] = 0;
    printf("%s\n", out);
}

void t5_display_print_shot(const uint16_t *px, int w, int h, int stride, int step, int rotation)
{
    if (rotation >= 0) printf("rotation %d\n", rotation);
    printf("SHOT %d %d\n", w, h);
    uint8_t buf[LINE_BYTES];
    size_t nb = 0;
    for (int y = 0; y < h; y++) {
        const uint16_t *row = px + (size_t)y * step * stride;
        for (int x = 0; x < w; x++) {
            uint16_t p = row[(size_t)x * step];
            buf[nb++] = (uint8_t)(p & 0xff);
            buf[nb++] = (uint8_t)(p >> 8);
            if (nb == sizeof buf) {
                line(buf, nb);
                nb = 0;
            }
        }
    }
    if (nb) line(buf, nb);
    printf("END\n");
}
