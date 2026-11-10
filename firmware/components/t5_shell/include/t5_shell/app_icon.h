/* SPDX-License-Identifier: Apache-2.0
 * Installed apps' own icons: the PNG a manifest names as its icon, drawn as
 * a circle. The launcher and the App Manager use it; apps whose package has
 * no icon (and the built-in apps) keep their glyph. UI task.
 */
#pragma once
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The app's icon as an image on parent, size_px across, cropped to a
 * circle. NULL if the app has no icon or it can't be read (the reason is
 * logged once), and then the caller draws the glyph instead. Decoded once
 * per size and kept until the next install or removal. */
lv_obj_t *t5_shell_app_icon_create(lv_obj_t *parent, const char *id, int32_t size_px);

#ifdef __cplusplus
}
#endif
