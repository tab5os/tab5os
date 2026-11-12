/* SPDX-License-Identifier: Apache-2.0
 * The bar the shell draws around an app's screen, besides the status bar.
 * There is no app bar: an app that wants a title or actions draws its own
 * (a T5UI_MD_APPBAR or T5UI_MD_PAGE).
 *
 *   Gesture bar  a handle centred in a 16 dp strip at the bottom: swipe up
 *                from here to go home. The app's screen is padded clear of
 *                the strip. In full screen it is hidden; a finger on the
 *                bottom edge shows it until SHELL_PEEK_MS after it lifts,
 *                and entering full screen shows it for SHELL_PEEK_MS, so the
 *                way out stays visible.
 *
 * It lives on the top layer behind everything else there (quick
 * settings, menus, dialogs, the keyboard). UI task only.
 */
#include "shell_priv.h"

#define HANDLE_W_DP 96
#define HANDLE_H_DP 4

static t5ui_obj_t *s_handle;
static t5ui_timer_t *s_peek;
static bool s_handle_wanted;   /* shown outside full screen */

/* ---- gesture bar ------------------------------------------------------------------ */
static void handle_create(void)
{
    if (s_handle) return;
    void *saved = shell_sys_enter();
    s_handle = shell_box(t5ui_sys_layer(T5UI_LAYER_TOP));
    shell_size(s_handle, t5ui_dp(HANDLE_W_DP), t5ui_dp(HANDLE_H_DP));
    t5ui_set_int(s_handle, T5UI_P_RADIUS, T5UI_RADIUS_CIRCLE);
    t5ui_set_color(s_handle, T5UI_P_BG_COLOR, t5ui_theme_color(T5UI_COLOR_ON_SURFACE));
    t5ui_set_int(s_handle, T5UI_P_BG_OPA, t5ui_theme_emphasis(T5UI_EMPHASIS_MEDIUM));
    /* A thin rim in the background colour keeps it visible on light content. */
    t5ui_set_int(s_handle, T5UI_P_OUTLINE_WIDTH, t5ui_dp(1));
    t5ui_set_color(s_handle, T5UI_P_OUTLINE_COLOR, t5ui_theme_color(T5UI_COLOR_BACKGROUND));
    t5ui_set_int(s_handle, T5UI_P_OUTLINE_OPA, 102);   /* 40% */
    t5ui_set_int(s_handle, T5UI_P_ALIGN, T5UI_ALIGN_BOTTOM_MID);
    t5ui_set_int(s_handle, T5UI_P_X, 0);
    t5ui_set_int(s_handle, T5UI_P_Y, -t5ui_dp((SHELL_GESTURE_H_DP - HANDLE_H_DP) / 2));
    t5ui_set_int(s_handle, T5UI_P_CLICKABLE, false);
    t5ui_move_to(s_handle, 0);
    t5ui_set_int(s_handle, T5UI_P_HIDDEN, true);
    shell_sys_leave(saved);
}

static void peek_end(t5ui_timer_t *t, void *user)
{
    (void)user;
    t5ui_timer_delete(t);           /* once */
    s_peek = NULL;
    if (s_handle && !s_handle_wanted) t5ui_set_int(s_handle, T5UI_P_HIDDEN, true);
}

/* shown: the handle sits in its strip (an app outside full screen), behind
 * everything else on the top layer. */
void bars_handle_show(bool shown)
{
    handle_create();
    s_handle_wanted = shown;
    if (s_peek) {
        t5ui_timer_delete(s_peek);
        s_peek = NULL;
    }
    t5ui_move_to(s_handle, 0);
    t5ui_set_int(s_handle, T5UI_P_HIDDEN, !shown);
}

/* Full screen: show the handle over the app for SHELL_PEEK_MS, in front of
 * whatever else is on the top layer. */
void bars_handle_peek(void)
{
    handle_create();
    if (s_handle_wanted) return;
    void *saved = shell_sys_enter();
    t5ui_move_to(s_handle, -1);
    t5ui_set_int(s_handle, T5UI_P_HIDDEN, false);
    if (s_peek) t5ui_timer_reset(s_peek);
    else s_peek = t5ui_timer(SHELL_PEEK_MS, peek_end, NULL);
    shell_sys_leave(saved);
}

void bars_reset(void)
{
    s_handle = NULL;
    s_peek = NULL;
    s_handle_wanted = false;
}
