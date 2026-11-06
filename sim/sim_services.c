/* SPDX-License-Identifier: Apache-2.0
 * Mocked services for the simulator (plan 4.1): battery, Wi-Fi and ports,
 * with a panel to drive them (toolbar "Services"). Phase 6's shell reads
 * these through the same calls it uses on the device.
 */
#include <stdio.h>
#include "lvgl.h"
#include "md/md.h"
#include "md/md_icons.h"
#include "sim.h"

sim_state_t g_sim = { .battery = 76, .charging = false, .wifi = true, .rssi = -58, .grove_claimed = false };

static lv_obj_t *s_panel, *s_bat_label;

static void bat_changed(lv_event_t *e)
{
    g_sim.battery = lv_slider_get_value(lv_event_get_target_obj(e));
    lv_label_set_text_fmt(s_bat_label, "Battery %d%%", g_sim.battery);
}

static void sw_changed(lv_event_t *e)
{
    bool *flag = lv_event_get_user_data(e);
    *flag = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
}

static lv_obj_t *row(lv_obj_t *p, const char *icon, const char *text, bool *flag)
{
    lv_obj_t *r = lv_obj_create(p);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), T5_DP(MD_LIST_ROW_1_DP));
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, T5_DP(16), 0);
    md_icon_create(r, icon, MD_ICON_DP);
    lv_obj_t *l = md_label_create(r, text, MD_TYPE_SUBTITLE1);
    lv_obj_set_flex_grow(l, 1);
    if (flag) {
        lv_obj_t *sw = lv_switch_create(r);
        if (*flag) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, sw_changed, LV_EVENT_VALUE_CHANGED, flag);
    }
    return r;
}

void sim_services_toggle(void)
{
    if (s_panel) {
        lv_obj_delete(s_panel);
        s_panel = NULL;
        return;
    }
    s_panel = lv_obj_create(lv_layer_top());
    md_set_elevation(s_panel, MD_ELEV_MENU, true);
    lv_obj_set_size(s_panel, T5_DP(320), LV_SIZE_CONTENT);
    lv_obj_align(s_panel, LV_ALIGN_TOP_RIGHT, -T5_DP(8), T5_DP(56));
    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    md_label_create(s_panel, "Mocked services", MD_TYPE_OVERLINE);
    lv_obj_t *r = row(s_panel, MD_ICON_BATTERY_FULL, "", NULL);
    s_bat_label = lv_obj_get_child(r, 1);
    lv_label_set_text_fmt(s_bat_label, "Battery %d%%", g_sim.battery);
    lv_obj_t *s = lv_slider_create(s_panel);
    lv_obj_set_width(s, LV_PCT(100));
    lv_slider_set_value(s, g_sim.battery, LV_ANIM_OFF);
    lv_obj_add_event_cb(s, bat_changed, LV_EVENT_VALUE_CHANGED, NULL);
    row(s_panel, MD_ICON_BATTERY_CHARGING_FULL, "Charging", &g_sim.charging);
    row(s_panel, MD_ICON_WIFI, "Wi-Fi \"Workshop\"", &g_sim.wifi);
    row(s_panel, MD_ICON_CABLE, "grove claimed (I2C)", &g_sim.grove_claimed);
}
