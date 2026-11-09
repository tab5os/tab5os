/* SPDX-License-Identifier: Apache-2.0
 * What a computer does to the device, over the developer HTTP API (dev.c)
 * or the USB console (t5_diag): both use these, so the two ways in behave
 * the same. App installs go to t5_apps_install(), which replaces a running
 * copy, and OS images to t5_ota_install_stream() or t5_ota_install_file().
 */
#pragma once
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The largest file a computer may push (an app over HTTP, `app put` over USB). */
#define T5_DEV_PUSH_MAX (16u * 1024u * 1024u)

typedef struct {
    uint16_t *px;         /* RGB565, native portrait, w * h */
    int w, h;
    int rotation;         /* the UI's rotation in degrees: the receiver turns the image */
} t5_dev_shot_t;

/* The frame being scanned out, every div-th pixel each way (div 1..16).
 * T5_ERR_STATE without a framebuffer. Free px with t5_dev_shot_free(). */
T5_MUST_CHECK t5_err_t t5_dev_shot(int div, t5_dev_shot_t *out);
void t5_dev_shot_free(t5_dev_shot_t *shot);

#ifdef __cplusplus
}
#endif
