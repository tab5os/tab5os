/* SPDX-License-Identifier: Apache-2.0
 * On-screen keyboard (plan 6.2). Shown when a text area is tapped, hidden
 * when something else is tapped, on its close or OK key, when the text area
 * goes away and when the foreground changes. A USB keyboard (Phase 7) keeps
 * it hidden.
 *
 * The keyboard covers the bottom of the screen. If the text area would be
 * under it, the text area's top-level container is panned up (Android's
 * "adjustPan"), so this works for app screens and for dialogs alike.
 */
#include <string.h>
#include "shell_priv.h"
#include "t5_ui/t5_ui.h"

static t5ui_obj_t *s_kb, *s_ta, *s_panned;

bool keyboard_visible(void)
{
    return s_kb != NULL;
}

static int32_t height_of(t5ui_obj_t *o)
{
    t5ui_rect_t r;
    t5ui_coords(o, &r);
    return r.y2 - r.y1 + 1;
}

static void unpan(void)
{
    if (s_panned) {
        t5ui_set_int(s_panned, T5UI_P_TRANSLATE_Y, 0);
        s_panned = NULL;
    }
}

/* The child of a screen or of the top layer that holds obj. */
static t5ui_obj_t *top_container(t5ui_obj_t *obj)
{
    t5ui_obj_t *o = obj;
    for (;;) {
        t5ui_obj_t *p = t5ui_parent(o);
        if (!p || t5ui_parent(p) == NULL) return o;
        o = p;
    }
}

static void pan_for(t5ui_obj_t *ta)
{
    unpan();
    if (!s_kb || !ta) return;
    t5ui_layout_now(s_kb);
    t5ui_rect_t ta_a;
    t5ui_coords(ta, &ta_a);
    int32_t kb_top = height_of(t5ui_sys_layer(T5UI_LAYER_TOP)) - height_of(s_kb);
    int32_t overlap = ta_a.y2 + t5ui_dp(8) - kb_top;
    if (overlap <= 0) return;
    t5ui_obj_t *c = top_container(ta);
    if (c == s_kb) return;
    s_panned = c;
    t5ui_set_int(c, T5UI_P_TRANSLATE_Y, -overlap);
}

static void ta_deleted(t5ui_event_t *e)
{
    if (t5ui_event_target(e) != s_ta) return;
    s_ta = NULL;
    if (s_kb) t5ui_set_obj(s_kb, T5UI_P_TARGET, NULL);
    keyboard_hide();
}

static void kb_done(t5ui_event_t *e)
{
    (void)e;
    keyboard_hide();
}

static void kb_deleted(t5ui_event_t *e)
{
    if (t5ui_event_target(e) == s_kb) s_kb = NULL;
}

static void kb_clicked(t5ui_event_t *e)
{
    (void)e;
    shell_sys_click();
}

/* ta: a text area anywhere in the system, often an app's own (its LVGL
 * object, as t5ui_sys_obj() hands it over). */
void keyboard_show(t5ui_obj_t *ta)
{
    if (!ta) return;
    if (t5_ui_hw_keyboard()) {
        /* A USB keyboard types into the default group's focused object. */
        t5ui_cmd(ta, T5UI_CMD_FOCUS, 0, 0, 0, NULL);
        return;
    }
    if (s_ta && s_ta != ta) t5ui_off(s_ta, T5UI_EV_DELETE, ta_deleted, NULL);
    if (s_ta != ta) t5ui_on(ta, T5UI_EV_DELETE, ta_deleted, NULL);
    s_ta = ta;
    if (!s_kb) {
        t5ui_obj_t *top = t5ui_sys_layer(T5UI_LAYER_TOP);
        void *saved = shell_sys_enter();
        s_kb = t5ui_create(top, T5UI_KEYBOARD);
        shell_sys_leave(saved);
        t5ui_rect_t r;
        t5ui_coords(top, &r);
        int32_t h = r.y2 - r.y1 + 1;
        int32_t w = r.x2 - r.x1 + 1;
        shell_size(s_kb, T5UI_PCT(100), w > h ? h * 46 / 100 : h * 34 / 100);
        t5ui_set_int(s_kb, T5UI_P_ALIGN, T5UI_ALIGN_BOTTOM_MID);
        t5ui_set_int(s_kb, T5UI_P_X, 0);
        t5ui_set_int(s_kb, T5UI_P_Y, 0);
        t5ui_on(s_kb, T5UI_EV_READY, kb_done, NULL);
        t5ui_on(s_kb, T5UI_EV_CANCEL, kb_done, NULL);
        t5ui_on(s_kb, T5UI_EV_DELETE, kb_deleted, NULL);
        t5ui_on(s_kb, T5UI_EV_VALUE, kb_clicked, NULL);
        /* Aligned to the bottom, so y is an offset from there: slide the
         * offset from the keyboard's height to 0. (Animating to its absolute
         * y left it a keyboard's height below the screen: no keyboard.) */
        t5ui_animate(s_kb, T5UI_P_Y, height_of(s_kb), 0, SHELL_DUR_DRAWER_OUT_MS, 0, T5UI_EASE_DECELERATE);
    }
    t5ui_move_to(s_kb, -1);
    t5ui_set_obj(s_kb, T5UI_P_TARGET, ta);
    t5ui_set_int(s_kb, T5UI_P_KEYBOARD_MODE, T5UI_KB_TEXT);
    t5ui_set_int(ta, T5UI_P_STATE, t5ui_get_int(ta, T5UI_P_STATE, 0) | T5UI_STATE_FOCUSED);
    pan_for(ta);
}

void keyboard_hide(void)
{
    t5ui_obj_t *ta = s_ta;
    unpan();
    if (s_ta) {
        t5ui_off(s_ta, T5UI_EV_DELETE, ta_deleted, NULL);
        s_ta = NULL;
    }
    if (s_kb) {
        t5ui_set_obj(s_kb, T5UI_P_TARGET, NULL);
        t5ui_obj_t *kb = s_kb;
        s_kb = NULL;
        t5ui_cmd(kb, T5UI_CMD_DELETE_LATER, 0, 0, 0, NULL);
    }
    int32_t st = ta ? t5ui_get_int(ta, T5UI_P_STATE, 0) : 0;
    if (st & T5UI_STATE_FOCUSED) {
        t5ui_set_int(ta, T5UI_P_STATE, st & ~T5UI_STATE_FOCUSED);
        t5ui_cmd(ta, T5UI_CMD_SEND, T5UI_EV_DEFOCUS, 0, 0, NULL);
    }
}

bool keyboard_owns(t5ui_obj_t *obj)
{
    for (t5ui_obj_t *o = obj; o; o = t5ui_parent(o)) {
        if (o == s_kb) return true;
    }
    return false;
}

void keyboard_reset(void)
{
    s_kb = s_ta = s_panned = NULL;
}
