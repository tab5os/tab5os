/* SPDX-License-Identifier: Apache-2.0
 * The bar the shell draws around an app's screen, besides the status bar.
 * There is no app bar: an app that wants a title or actions draws its own
 * (md_appbar_create, md_page_create).
 *
 *   Gesture bar  a handle centred in a 16 dp strip at the bottom: swipe up
 *                from here to go home. The app's screen is padded clear of
 *                the strip. In full screen it is hidden; a finger on the
 *                bottom edge shows it until SHELL_PEEK_MS after it lifts,
 *                and entering full screen shows it for SHELL_PEEK_MS, so the
 *                way out stays visible.
 *
 * It lives on lv_layer_top() behind everything else there (quick
 * settings, menus, dialogs, the keyboard). UI task only.
 */
#include "shell_priv.h"

#define HANDLE_W_DP 96
#define HANDLE_H_DP 4

static lv_obj_t *s_handle;
static lv_timer_t *s_peek;
static bool s_handle_wanted;   /* shown outside full screen */

/* ---- gesture bar ------------------------------------------------------------------ */
static void handle_create(void)
{
    if (s_handle) return;
    const md_scheme_t *sc = md_scheme();
    void *saved = shell_sys_enter();
    s_handle = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_handle);
    lv_obj_set_size(s_handle, T5_DP(HANDLE_W_DP), T5_DP(HANDLE_H_DP));
    lv_obj_set_style_radius(s_handle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_handle, md_color(sc->c.on_surface), 0);
    lv_obj_set_style_bg_opa(s_handle, sc->on_surface.medium, 0);
    /* A thin rim in the background colour keeps it visible on light content. */
    lv_obj_set_style_outline_width(s_handle, T5_DP(1), 0);
    lv_obj_set_style_outline_color(s_handle, md_color(sc->c.background), 0);
    lv_obj_set_style_outline_opa(s_handle, LV_OPA_40, 0);
    lv_obj_align(s_handle, LV_ALIGN_BOTTOM_MID, 0, -T5_DP((SHELL_GESTURE_H_DP - HANDLE_H_DP) / 2));
    lv_obj_set_clickable(s_handle, false);
    lv_obj_move_to_index(s_handle, 0);
    lv_obj_set_hidden(s_handle, true);
    shell_sys_leave(saved);
}

static void peek_end(lv_timer_t *t)
{
    (void)t;
    s_peek = NULL;
    if (s_handle && !s_handle_wanted) lv_obj_set_hidden(s_handle, true);
}

/* shown: the handle sits in its strip (an app outside full screen), behind
 * everything else on the top layer. */
void bars_handle_show(bool shown)
{
    handle_create();
    s_handle_wanted = shown;
    if (s_peek) {
        lv_timer_delete(s_peek);
        s_peek = NULL;
    }
    lv_obj_move_to_index(s_handle, 0);
    lv_obj_set_hidden(s_handle, !shown);
}

/* Full screen: show the handle over the app for SHELL_PEEK_MS, in front of
 * whatever else is on the top layer. */
void bars_handle_peek(void)
{
    handle_create();
    if (s_handle_wanted) return;
    void *saved = shell_sys_enter();
    lv_obj_move_foreground(s_handle);
    lv_obj_set_hidden(s_handle, false);
    if (s_peek) lv_timer_reset(s_peek);
    else {
        s_peek = lv_timer_create(peek_end, SHELL_PEEK_MS, NULL);
        lv_timer_set_repeat_count(s_peek, 1);
        lv_timer_set_auto_delete(s_peek, true);
    }
    shell_sys_leave(saved);
}

void bars_reset(void)
{
    s_handle = NULL;
    s_peek = NULL;
    s_handle_wanted = false;
}
