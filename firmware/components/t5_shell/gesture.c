/* SPDX-License-Identifier: Apache-2.0
 * System gestures (plan 6.3) and tap routing for the keyboard.
 *
 * LVGL sends PRESSED, RELEASED and CLICKED to the input device itself
 * before the object, so the shell sees every touch without touching the
 * app's objects. Edge swipes are tracked with a short poll while the finger
 * is down (scrolling objects never see LVGL's own gesture events):
 *   from the top edge, down 40 dp  -> quick settings
 *   from the bottom edge, up 40 dp -> home (in apps), or closes the
 *                                     quick settings, app drawer or file picker;
 *                                     in full screen a touch here shows the
 *                                     gesture bar's handle (bars.c)
 *   anywhere on home, up 40 dp     -> the app drawer
 * A triggered swipe cancels the press, so the object under the finger
 * doesn't scroll or click.
 */
#include "shell_priv.h"

#define EDGE_TOP_DP SHELL_STATUS_H_DP
#define EDGE_BOTTOM_DP 20
#define SWIPE_DP 40

typedef enum { EDGE_NONE, EDGE_TOP, EDGE_BOTTOM, EDGE_HOME } edge_t;

static edge_t s_edge;
static lv_point_t s_start;
static lv_timer_t *s_poll;
static lv_indev_t *s_edge_indev;

static void stop_poll(void)
{
    if (s_poll) {
        lv_timer_delete(s_poll);
        s_poll = NULL;
    }
    s_edge = EDGE_NONE;
}

static void cancel_press(lv_indev_t *indev)
{
    lv_indev_reset(indev, NULL);
    lv_indev_wait_release(indev);
}

static void poll(lv_timer_t *t)
{
    (void)t;
    lv_indev_t *indev = s_edge_indev;
    if (!indev || lv_indev_get_state(indev) != LV_INDEV_STATE_PRESSED) {
        stop_poll();
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dy = p.y - s_start.y;
    int32_t dx = p.x - s_start.x;
    if (s_edge == EDGE_TOP && dy > T5_DP(SWIPE_DP)) {
        stop_poll();
        cancel_press(indev);
        qs_open();
    } else if (s_edge == EDGE_BOTTOM && dy < -T5_DP(SWIPE_DP)) {
        stop_poll();
        cancel_press(indev);
        if (qs_is_open()) qs_close();
        else if (launcher_drawer_is_open()) launcher_drawer_close(true);
        else if (shell_sys_dismiss_modal()) {}       /* a modal goes first; the app stays */
        else if (shell_in_app()) shell_sys_home();
    } else if (s_edge == EDGE_HOME && dy < -T5_DP(SWIPE_DP) && -dy > LV_ABS(dx)) {
        stop_poll();
        cancel_press(indev);
        if (!shell_in_app() && !qs_is_open()) launcher_drawer_open();
    }
}

static bool is_textarea(lv_obj_t *o)
{
    return o && lv_obj_check_type(o, &lv_textarea_class);
}

static void indev_event(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = lv_event_get_param(e);
    if (code == LV_EVENT_PRESSED) {
        if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) return;
        lv_indev_get_point(indev, &s_start);
        int32_t h = lv_display_get_vertical_resolution(lv_display_get_default());
        stop_poll();
        if (s_start.y < T5_DP(EDGE_TOP_DP) && !qs_is_open()) s_edge = EDGE_TOP;
        else if (s_start.y > h - T5_DP(EDGE_BOTTOM_DP) && (shell_in_app() || qs_is_open() || launcher_drawer_is_open())) {
            s_edge = EDGE_BOTTOM;
            if (shell_fullscreen()) bars_handle_peek();   /* the gesture bar shows where to swipe */
        }
        else if (!shell_in_app() && !qs_is_open() && !launcher_drawer_is_open() && obj &&
                 lv_obj_get_screen(obj) == launcher_get())   /* not a menu or dialog over it */
            s_edge = EDGE_HOME;
        if (s_edge != EDGE_NONE) {
            s_edge_indev = indev;
            s_poll = lv_timer_create(poll, 16, NULL);
        }
    } else if (code == LV_EVENT_CLICKED) {
        if (is_textarea(obj)) {
            keyboard_show(obj);
        } else if (keyboard_visible() && !keyboard_owns(obj)) {
            keyboard_hide();
        }
    }
}

void gesture_attach_all(void)
{
    for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) {
        if (lv_indev_get_type(i) != LV_INDEV_TYPE_POINTER) continue;
        lv_indev_remove_event_cb_with_user_data(i, indev_event, NULL);
        lv_indev_add_event_cb(i, indev_event, LV_EVENT_PRESSED, NULL);
        lv_indev_add_event_cb(i, indev_event, LV_EVENT_CLICKED, NULL);
    }
}

void gesture_reset(void)
{
    stop_poll();
    s_edge_indev = NULL;
}
