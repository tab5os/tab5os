/* SPDX-License-Identifier: Apache-2.0 */
#include "t5_dev/control.h"
#include <stddef.h>
#include "esp_heap_caps.h"
#include "t5_display/t5_display.h"
#include "t5_ui/t5_ui.h"

t5_err_t t5_dev_shot(int div, t5_dev_shot_t *out)
{
    if (!out || div < 1 || div > 16) return T5_ERR_INVALID_ARG;
    int i = t5_display_scanout();
    const uint16_t *fb = i >= 0 ? t5_display_fb_n(i) : NULL;
    if (!fb) return T5_ERR_STATE;
    int w = T5_DISPLAY_W / div, h = T5_DISPLAY_H / div;
    uint16_t *px = heap_caps_malloc((size_t)w * (size_t)h * 2, MALLOC_CAP_SPIRAM);
    if (!px) return T5_ERR_NO_MEM;
    for (int y = 0; y < h; y++) {
        const uint16_t *row = fb + (size_t)(y * div) * T5_DISPLAY_W;
        for (int x = 0; x < w; x++) px[(size_t)y * (size_t)w + (size_t)x] = row[x * div];
    }
    *out = (t5_dev_shot_t){ .px = px, .w = w, .h = h, .rotation = (int)t5_ui_rotation() * 90 };
    return T5_OK;
}

void t5_dev_shot_free(t5_dev_shot_t *shot)
{
    if (!shot) return;
    heap_caps_free(shot->px);
    shot->px = NULL;
}
