/* SPDX-License-Identifier: Apache-2.0
 * t5ui for the OS's own UI only: the shell, system apps, dialogs. Not in
 * the SDK and not exported to apps.
 *
 * The OS draws with the same t5ui as apps. This adds what only the system
 * may do: draw above every app (the layers), change the theme, capture the
 * screen. It also marks the few places where the platform code under the UI
 * (app screens, input hooks) hands an object to UI code or takes one back:
 * t5ui_sys_obj / t5ui_sys_native. Nothing else in the UI should know what
 * draws it.
 */
#pragma once
#include "t5_gui/t5ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    T5UI_LAYER_TOP = 0,        /* over every screen: the shell's bars, panels, keyboard */
    T5UI_LAYER_BOTTOM = 1,     /* under every screen */
    T5UI_LAYER_SCREEN = 2,     /* the screen shown now */
} t5ui_layer_t;
t5ui_obj_t *t5ui_sys_layer(t5ui_layer_t layer);
/* A screen of its own (no parent), for the runtime to load: the home
 * screen. Deleted with t5ui_delete. */
t5ui_obj_t *t5ui_sys_screen_create(void);

/* The theme's accents (Settings). */
void t5ui_sys_set_accent(t5ui_accent_t primary, t5ui_accent_t secondary);

/* A copy of what obj shows, as RGB565 pixels; free with t5ui_sys_snapshot_free. */
t5_err_t t5ui_sys_snapshot(t5ui_obj_t *obj, t5ui_pixels_t *out);
void t5ui_sys_snapshot_free(t5ui_pixels_t *px);

/* A label shows text from the caller's buffer without copying it (a
 * console's big scrollback). The buffer must stay until the label goes or
 * gets other text; call again after changing it. */
t5_err_t t5ui_sys_set_text_static(t5ui_obj_t *label, const char *text);

/* What draws the UI, for About and Licences pages: "LVGL 9.6". */
const char *t5ui_sys_backend_name(void);

/* The boundary with the platform code: its native object (an lv_obj_t *
 * today) as a t5ui object, and back. */
t5ui_obj_t *t5ui_sys_obj(void *native);
void *t5ui_sys_native(t5ui_obj_t *obj);
/* The native object that takes typing: a text field's text area, or a
 * textarea itself; NULL for other objects. */
void *t5ui_sys_native_input(t5ui_obj_t *obj);

#ifdef __cplusplus
}
#endif
