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

static lv_obj_t *s_kb, *s_ta, *s_panned;
static bool s_hw;

bool keyboard_visible(void)
{
    return s_kb != NULL;
}

static void set_y_anim(void *o, int32_t v)
{
    lv_obj_set_y(o, v);
}

static void unpan(void)
{
    if (s_panned) {
        lv_obj_set_style_translate_y(s_panned, 0, 0);
        s_panned = NULL;
    }
}

/* The child of a screen or of the top layer that holds obj. */
static lv_obj_t *top_container(lv_obj_t *obj)
{
    lv_obj_t *o = obj;
    for (;;) {
        lv_obj_t *p = lv_obj_get_parent(o);
        if (!p || lv_obj_get_parent(p) == NULL) return o;
        o = p;
    }
}

static void pan_for(lv_obj_t *ta)
{
    unpan();
    if (!s_kb || !ta) return;
    lv_obj_update_layout(s_kb);
    lv_area_t ka, ta_a;
    lv_obj_get_coords(s_kb, &ka);
    lv_obj_get_coords(ta, &ta_a);
    int32_t kb_top = lv_obj_get_height(lv_layer_top()) - lv_obj_get_height(s_kb);
    (void)ka;
    int32_t overlap = ta_a.y2 + T5_DP(8) - kb_top;
    if (overlap <= 0) return;
    lv_obj_t *c = top_container(ta);
    if (c == s_kb) return;
    s_panned = c;
    lv_obj_set_style_translate_y(c, -overlap, 0);
}

static void ta_deleted(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != s_ta) return;
    s_ta = NULL;
    if (s_kb) lv_keyboard_set_textarea(s_kb, NULL);
    keyboard_hide();
}

static void kb_done(lv_event_t *e)
{
    (void)e;
    keyboard_hide();
}

static void kb_deleted(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) == s_kb) s_kb = NULL;
}

static void kb_clicked(lv_event_t *e)
{
    (void)e;
    shell_sys_click();
}

void keyboard_show(lv_obj_t *ta)
{
    if (!ta) return;
    if (s_hw) {
        /* A USB keyboard types into the default group's focused object. */
        lv_group_t *g = lv_group_get_default();
        if (g) {
            if (lv_obj_get_group(ta) != g) lv_group_add_obj(g, ta);
            lv_group_focus_obj(ta);
        }
        return;
    }
    if (s_ta && s_ta != ta) lv_obj_remove_event_cb_with_user_data(s_ta, ta_deleted, NULL);
    if (s_ta != ta) lv_obj_add_event_cb(ta, ta_deleted, LV_EVENT_DELETE, NULL);
    s_ta = ta;
    if (!s_kb) {
        void *saved = shell_sys_enter();
        s_kb = lv_keyboard_create(lv_layer_top());
        shell_sys_leave(saved);
        int32_t h = lv_obj_get_height(lv_layer_top());
        int32_t w = lv_obj_get_width(lv_layer_top());
        lv_obj_set_size(s_kb, LV_PCT(100), w > h ? h * 46 / 100 : h * 34 / 100);
        lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_add_event_cb(s_kb, kb_done, LV_EVENT_READY, NULL);
        lv_obj_add_event_cb(s_kb, kb_done, LV_EVENT_CANCEL, NULL);
        lv_obj_add_event_cb(s_kb, kb_deleted, LV_EVENT_DELETE, NULL);
        lv_obj_add_event_cb(s_kb, kb_clicked, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_update_layout(s_kb);
        /* Aligned to the bottom, so y is an offset from there: slide the
         * offset from the keyboard's height to 0. (Animating to its absolute
         * y left it a keyboard's height below the screen: no keyboard.) */
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_kb);
        lv_anim_set_exec_cb(&a, set_y_anim);
        lv_anim_set_values(&a, lv_obj_get_height(s_kb), 0);
        lv_anim_set_duration(&a, MD_DUR_DRAWER_OUT_MS);
        md_anim_set_curve(&a, MD_CURVE_DECELERATION);
        lv_anim_start(&a);
    }
    lv_obj_move_foreground(s_kb);
    lv_keyboard_set_textarea(s_kb, ta);
    lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    pan_for(ta);
}

void keyboard_hide(void)
{
    lv_obj_t *ta = s_ta;
    unpan();
    if (s_ta) {
        lv_obj_remove_event_cb_with_user_data(s_ta, ta_deleted, NULL);
        s_ta = NULL;
    }
    if (s_kb) {
        lv_keyboard_set_textarea(s_kb, NULL);
        lv_obj_t *kb = s_kb;
        s_kb = NULL;
        lv_obj_delete_async(kb);
    }
    if (ta && lv_obj_has_state(ta, LV_STATE_FOCUSED)) {
        lv_obj_remove_state(ta, LV_STATE_FOCUSED);
        lv_obj_send_event(ta, LV_EVENT_DEFOCUSED, NULL);
    }
}

void keyboard_init(void)
{
}

static void hide_async(void *p)
{
    (void)p;
    keyboard_hide();
}

void t5_shell_set_hw_keyboard(bool present)
{
    s_hw = present;                /* any task: read on the UI task */
    if (present) t5_ui_async(hide_async, NULL);
}

bool keyboard_hw_present(void)
{
    return s_hw;
}

bool keyboard_owns(lv_obj_t *obj)
{
    for (lv_obj_t *o = obj; o; o = lv_obj_get_parent(o)) {
        if (o == s_kb) return true;
    }
    return false;
}

lv_obj_t *keyboard_textarea(void)
{
    return s_ta;
}

void keyboard_reset(void)
{
    s_kb = s_ta = s_panned = NULL;
}
