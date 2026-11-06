/* SPDX-License-Identifier: Apache-2.0
 * MD2 LVGL theme (plan 4.6). LVGL's default theme is the parent, so widgets
 * we don't style still work; this theme's apply callback then overrides
 * colours, shapes, sizes and type with the MD2 tokens.
 *
 * Square, opaque fills stay on the PPA path; radii are only where MD2 needs
 * them (design, "Rendering budget").
 */
#include <string.h>
#include "md/md.h"
#include "md_priv.h"

static lv_theme_t *s_theme;
static lv_display_t *s_disp;
static md_scheme_t s_scheme;
static md_accent_t s_primary = MD_ACCENT_PURPLE, s_secondary = MD_ACCENT_TEAL;
static bool s_dark = true;
static bool s_styles_inited;

static struct {
    lv_style_t scr, surface, text_body, scrollbar;
    lv_style_t btn, btn_text, btn_outlined, btn_disabled;
    lv_style_t slider_main, slider_ind, slider_knob;
    lv_style_t sw_main, sw_ind, sw_knob, sw_knob_on;
    lv_style_t cb_main, cb_box, cb_box_on;
    lv_style_t ta, ta_focus, ta_cursor;
    lv_style_t bar_main, bar_ind;
    lv_style_t list, list_btn;
    lv_style_t dropdown, dropdown_list, dropdown_sel;
    lv_style_t tab_btn, tab_btn_on;
    lv_style_t kb, kb_btn;
    lv_style_t arc_main, arc_ind;
    lv_style_t no_press;
    lv_style_t elev[MD_ELEV_LEVELS];
} s;

const int md_elev_levels[MD_ELEV_LEVELS] = { 0, 1, 2, 3, 4, 6, 8, 12, 16, 24 };

const md_scheme_t *md_scheme(void)
{
    return &s_scheme;
}

md_accent_t md_get_accent(void)
{
    return s_primary;
}

bool md_is_dark(void)
{
    return s_dark;
}

lv_style_t *md_style_btn_text(void)
{
    return &s.btn_text;
}

lv_style_t *md_style_btn_outlined(void)
{
    return &s.btn_outlined;
}

lv_style_t *md_style_no_press(void)
{
    return &s.no_press;
}

lv_style_t *md_style_elevation(int dp)
{
    int best = 0;
    for (int i = 0; i < MD_ELEV_LEVELS; i++) {
        if (md_elev_levels[i] <= dp) best = i;
    }
    return &s.elev[best];
}

static void reset_all(void)
{
    lv_style_t *all = (lv_style_t *)&s;
    size_t n = sizeof s / sizeof(lv_style_t);
    for (size_t i = 0; i < n; i++) {
        if (s_styles_inited) lv_style_reset(&all[i]);
        lv_style_init(&all[i]);
    }
    s_styles_inited = true;
}

static void build_styles(void)
{
    const md_colors_t *c = &s_scheme.c;
    const md_emphasis_t *e = &s_scheme.on_surface;
    reset_all();

    lv_style_set_bg_color(&s.scr, md_color(c->background));
    lv_style_set_bg_opa(&s.scr, LV_OPA_COVER);
    lv_style_set_text_color(&s.scr, md_color(c->on_background));
    lv_style_set_text_opa(&s.scr, e->high);
    lv_style_set_text_font(&s.scr, md_font(MD_TYPE_BODY1));
    lv_style_set_pad_all(&s.scr, 0);

    /* Plain containers: a surface at 1 dp, medium shape, no border. */
    lv_style_set_bg_color(&s.surface, md_color(md_surface_at(&s_scheme, 1)));
    lv_style_set_bg_opa(&s.surface, LV_OPA_COVER);
    lv_style_set_radius(&s.surface, T5_DP(MD_SHAPE_MEDIUM_DP));
    lv_style_set_border_width(&s.surface, 0);
    lv_style_set_shadow_width(&s.surface, 0);
    lv_style_set_pad_all(&s.surface, T5_DP(MD_MARGIN_DP));
    lv_style_set_pad_gap(&s.surface, T5_DP(MD_GUTTER_DP));
    lv_style_set_text_color(&s.surface, md_color(c->on_surface));

    lv_style_set_text_color(&s.text_body, md_color(c->on_surface));
    lv_style_set_text_opa(&s.text_body, e->high);

    lv_style_set_bg_color(&s.scrollbar, md_color(c->on_surface));
    lv_style_set_bg_opa(&s.scrollbar, e->disabled);
    lv_style_set_width(&s.scrollbar, T5_DP(4));
    lv_style_set_radius(&s.scrollbar, T5_DP(2));
    lv_style_set_pad_right(&s.scrollbar, T5_DP(2));
    lv_style_set_pad_top(&s.scrollbar, T5_DP(2));

    /* Contained button: primary fill, 36 dp, 16 dp padding, button type. */
    lv_style_set_bg_color(&s.btn, md_color(c->primary));
    lv_style_set_bg_opa(&s.btn, LV_OPA_COVER);
    lv_style_set_text_color(&s.btn, md_color(c->on_primary));
    lv_style_set_text_font(&s.btn, md_font(MD_TYPE_BUTTON));
    lv_style_set_text_letter_space(&s.btn, md_type_tracking_px(MD_TYPE_BUTTON));
    lv_style_set_radius(&s.btn, T5_DP(MD_SHAPE_SMALL_DP));
    lv_style_set_min_height(&s.btn, T5_DP(MD_BUTTON_H_DP));
    lv_style_set_min_width(&s.btn, T5_DP(MD_BUTTON_MIN_W_DP));
    lv_style_set_pad_hor(&s.btn, T5_DP(MD_BUTTON_PAD_DP));
    lv_style_set_pad_ver(&s.btn, T5_DP(8));
    lv_style_set_border_width(&s.btn, 0);
    lv_style_set_shadow_width(&s.btn, 0);   /* flat by default (design, rule 2) */
    lv_style_set_outline_width(&s.btn, 0);
    lv_style_set_transform_width(&s.btn, 0);
    lv_style_set_transform_height(&s.btn, 0);

    lv_style_set_bg_opa(&s.btn_text, LV_OPA_TRANSP);
    lv_style_set_text_color(&s.btn_text, md_color(c->primary));
    lv_style_set_pad_hor(&s.btn_text, T5_DP(8));

    lv_style_set_border_color(&s.btn_outlined, md_color(c->on_surface));
    lv_style_set_border_opa(&s.btn_outlined, e->divider);
    lv_style_set_border_width(&s.btn_outlined, 1 /* px: hairline */);

    lv_style_set_bg_color(&s.btn_disabled, md_color(c->on_surface));
    lv_style_set_bg_opa(&s.btn_disabled, e->divider);
    lv_style_set_text_color(&s.btn_disabled, md_color(c->on_surface));
    lv_style_set_text_opa(&s.btn_disabled, e->disabled);

    /* Slider: 4 dp track, 20 dp thumb. */
    lv_style_set_bg_color(&s.slider_main, md_color(c->primary));
    lv_style_set_bg_opa(&s.slider_main, LV_OPA_30);
    lv_style_set_radius(&s.slider_main, LV_RADIUS_CIRCLE);
    lv_style_set_height(&s.slider_main, T5_DP(MD_SLIDER_TRACK_DP));
    lv_style_set_pad_ver(&s.slider_main, 0);
    lv_style_set_bg_color(&s.slider_ind, md_color(c->primary));
    lv_style_set_bg_opa(&s.slider_ind, LV_OPA_COVER);
    lv_style_set_radius(&s.slider_ind, LV_RADIUS_CIRCLE);
    lv_style_set_bg_color(&s.slider_knob, md_color(c->primary));
    lv_style_set_bg_opa(&s.slider_knob, LV_OPA_COVER);
    lv_style_set_radius(&s.slider_knob, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(&s.slider_knob, T5_DP(MD_SLIDER_THUMB_DP - MD_SLIDER_TRACK_DP) / 2);
    lv_style_set_shadow_width(&s.slider_knob, 0);

    /* Switch: 36 x 14 dp track, 20 dp handle. */
    lv_style_set_width(&s.sw_main, T5_DP(MD_SWITCH_TRACK_W_DP));
    lv_style_set_height(&s.sw_main, T5_DP(MD_SWITCH_TRACK_H_DP));
    lv_style_set_bg_color(&s.sw_main, md_color(c->on_surface));
    lv_style_set_bg_opa(&s.sw_main, LV_OPA_40);
    lv_style_set_radius(&s.sw_main, LV_RADIUS_CIRCLE);
    lv_style_set_bg_color(&s.sw_ind, md_color(c->primary));
    lv_style_set_bg_opa(&s.sw_ind, LV_OPA_50);
    lv_style_set_radius(&s.sw_ind, LV_RADIUS_CIRCLE);
    lv_style_set_bg_color(&s.sw_knob, md_color(s_dark ? 0xBDBDBD : 0xFAFAFA));
    lv_style_set_bg_opa(&s.sw_knob, LV_OPA_COVER);
    lv_style_set_radius(&s.sw_knob, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(&s.sw_knob, -T5_DP(MD_SWITCH_HANDLE_DP - MD_SWITCH_TRACK_H_DP) / 2);
    lv_style_set_bg_color(&s.sw_knob_on, md_color(c->primary));

    /* Checkbox: 18 dp box with a 2 dp border. */
    lv_style_set_pad_column(&s.cb_main, T5_DP(16));
    lv_style_set_text_color(&s.cb_main, md_color(c->on_surface));
    lv_style_set_width(&s.cb_box, T5_DP(MD_CHECKBOX_DP));
    lv_style_set_height(&s.cb_box, T5_DP(MD_CHECKBOX_DP));
    lv_style_set_radius(&s.cb_box, T5_DP(2));
    lv_style_set_border_width(&s.cb_box, T5_DP(2));
    lv_style_set_border_color(&s.cb_box, md_color(c->on_surface));
    lv_style_set_border_opa(&s.cb_box, e->medium);
    lv_style_set_bg_opa(&s.cb_box, LV_OPA_TRANSP);
    lv_style_set_bg_color(&s.cb_box_on, md_color(c->primary));
    lv_style_set_bg_opa(&s.cb_box_on, LV_OPA_COVER);
    lv_style_set_border_color(&s.cb_box_on, md_color(c->primary));
    lv_style_set_border_opa(&s.cb_box_on, LV_OPA_COVER);
    lv_style_set_text_color(&s.cb_box_on, md_color(c->on_primary));

    /* Filled text field: tinted fill, 1 dp underline, 2 dp primary when focused. */
    lv_style_set_bg_color(&s.ta, md_color(c->on_surface));
    lv_style_set_bg_opa(&s.ta, s_dark ? LV_OPA_10 : 10);
    lv_style_set_radius(&s.ta, T5_DP(MD_SHAPE_SMALL_DP));
    lv_style_set_border_side(&s.ta, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_width(&s.ta, 1 /* px: hairline */);
    lv_style_set_border_color(&s.ta, md_color(c->on_surface));
    lv_style_set_border_opa(&s.ta, e->disabled);
    lv_style_set_pad_hor(&s.ta, T5_DP(16));
    lv_style_set_pad_ver(&s.ta, T5_DP(16));
    lv_style_set_text_color(&s.ta, md_color(c->on_surface));
    lv_style_set_border_width(&s.ta_focus, T5_DP(2));
    lv_style_set_border_color(&s.ta_focus, md_color(c->primary));
    lv_style_set_border_opa(&s.ta_focus, LV_OPA_COVER);
    lv_style_set_border_color(&s.ta_cursor, md_color(c->primary));
    lv_style_set_border_width(&s.ta_cursor, T5_DP(1));

    lv_style_set_bg_color(&s.bar_main, md_color(c->primary));
    lv_style_set_bg_opa(&s.bar_main, LV_OPA_30);
    lv_style_set_radius(&s.bar_main, 0);
    lv_style_set_bg_color(&s.bar_ind, md_color(c->primary));
    lv_style_set_bg_opa(&s.bar_ind, LV_OPA_COVER);
    lv_style_set_radius(&s.bar_ind, 0);

    lv_style_set_bg_color(&s.arc_main, md_color(c->primary));
    lv_style_set_arc_color(&s.arc_main, md_color(c->primary));
    lv_style_set_arc_opa(&s.arc_main, LV_OPA_30);
    lv_style_set_arc_width(&s.arc_main, T5_DP(4));
    lv_style_set_arc_color(&s.arc_ind, md_color(c->primary));
    lv_style_set_arc_width(&s.arc_ind, T5_DP(4));

    /* Lists: square, flat rows (PPA path), dividers off. */
    lv_style_set_bg_color(&s.list, md_color(c->surface));
    lv_style_set_bg_opa(&s.list, LV_OPA_COVER);
    lv_style_set_radius(&s.list, 0);
    lv_style_set_pad_all(&s.list, 0);
    lv_style_set_pad_row(&s.list, 0);
    lv_style_set_border_width(&s.list, 0);
    lv_style_set_bg_opa(&s.list_btn, LV_OPA_TRANSP);
    lv_style_set_min_height(&s.list_btn, T5_DP(MD_LIST_ROW_1_DP));
    lv_style_set_pad_hor(&s.list_btn, T5_DP(16));
    lv_style_set_pad_column(&s.list_btn, T5_DP(32));
    lv_style_set_text_font(&s.list_btn, md_font(MD_TYPE_SUBTITLE1));
    lv_style_set_text_color(&s.list_btn, md_color(c->on_surface));
    lv_style_set_border_width(&s.list_btn, 0);
    lv_style_set_radius(&s.list_btn, 0);

    lv_style_set_bg_color(&s.dropdown, md_color(c->on_surface));
    lv_style_set_bg_opa(&s.dropdown, LV_OPA_10);
    lv_style_set_border_width(&s.dropdown, 0);
    lv_style_set_radius(&s.dropdown, T5_DP(MD_SHAPE_SMALL_DP));
    lv_style_set_pad_all(&s.dropdown, T5_DP(16));
    lv_style_set_text_color(&s.dropdown, md_color(c->on_surface));
    lv_style_set_bg_color(&s.dropdown_list, md_color(md_surface_at(&s_scheme, MD_ELEV_MENU)));
    lv_style_set_bg_opa(&s.dropdown_list, LV_OPA_COVER);
    lv_style_set_radius(&s.dropdown_list, T5_DP(MD_SHAPE_MEDIUM_DP));
    lv_style_set_border_width(&s.dropdown_list, 0);
    lv_style_set_text_line_space(&s.dropdown_list, T5_DP(24));
    lv_style_set_pad_all(&s.dropdown_list, T5_DP(8));
    lv_style_set_bg_color(&s.dropdown_sel, md_color(c->primary));
    lv_style_set_bg_opa(&s.dropdown_sel, s_scheme.states.selected * 2);
    lv_style_set_text_color(&s.dropdown_sel, md_color(c->primary));

    lv_style_set_text_font(&s.tab_btn, md_font(MD_TYPE_BUTTON));
    lv_style_set_text_color(&s.tab_btn, md_color(c->on_surface));
    lv_style_set_text_opa(&s.tab_btn, e->medium);
    lv_style_set_bg_opa(&s.tab_btn, LV_OPA_TRANSP);
    lv_style_set_text_color(&s.tab_btn_on, md_color(c->primary));
    lv_style_set_text_opa(&s.tab_btn_on, LV_OPA_COVER);
    lv_style_set_border_side(&s.tab_btn_on, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_width(&s.tab_btn_on, T5_DP(2));
    lv_style_set_border_color(&s.tab_btn_on, md_color(c->primary));

    lv_style_set_bg_color(&s.kb, md_color(md_surface_at(&s_scheme, 8)));
    lv_style_set_bg_opa(&s.kb, LV_OPA_COVER);
    lv_style_set_radius(&s.kb, 0);
    lv_style_set_pad_all(&s.kb, T5_DP(4));
    lv_style_set_pad_gap(&s.kb, T5_DP(4));
    lv_style_set_border_width(&s.kb, 0);
    lv_style_set_bg_color(&s.kb_btn, md_color(md_surface_at(&s_scheme, 24)));
    lv_style_set_bg_opa(&s.kb_btn, LV_OPA_COVER);
    lv_style_set_radius(&s.kb_btn, T5_DP(MD_SHAPE_SMALL_DP));
    lv_style_set_text_color(&s.kb_btn, md_color(c->on_surface));
    lv_style_set_border_width(&s.kb_btn, 0);
    lv_style_set_shadow_width(&s.kb_btn, 0);

    lv_style_set_color_filter_opa(&s.no_press, LV_OPA_TRANSP);
    lv_style_set_transform_width(&s.no_press, 0);
    lv_style_set_transform_height(&s.no_press, 0);

    for (int i = 0; i < MD_ELEV_LEVELS; i++) {
        lv_style_set_bg_color(&s.elev[i], md_color(md_surface_at(&s_scheme, md_elev_levels[i])));
        lv_style_set_bg_opa(&s.elev[i], LV_OPA_COVER);
    }
}

static void apply_cb(lv_theme_t *th, lv_obj_t *obj)
{
    (void)th;
    lv_obj_t *parent = lv_obj_get_parent(obj);
    if (parent == NULL) {
        lv_obj_add_style(obj, &s.scr, 0);
        lv_obj_add_style(obj, &s.scrollbar, LV_PART_SCROLLBAR);
        return;
    }
    if (lv_obj_check_type(obj, &lv_obj_class)) {
        lv_obj_add_style(obj, &s.surface, 0);
        lv_obj_add_style(obj, &s.scrollbar, LV_PART_SCROLLBAR);
    }
#if LV_USE_BUTTON
    else if (lv_obj_check_type(obj, &lv_button_class)) {
        /* List rows are buttons too. */
        if (lv_obj_check_type(parent, &lv_list_class)) {
            lv_obj_add_style(obj, &s.list_btn, 0);
        } else {
            lv_obj_add_style(obj, &s.btn, 0);
            lv_obj_add_style(obj, &s.btn_disabled, LV_STATE_DISABLED);
        }
        md_ripple_attach(obj);
    }
#endif
#if LV_USE_SLIDER
    else if (lv_obj_check_type(obj, &lv_slider_class)) {
        lv_obj_add_style(obj, &s.slider_main, 0);
        lv_obj_add_style(obj, &s.slider_ind, LV_PART_INDICATOR);
        lv_obj_add_style(obj, &s.slider_knob, LV_PART_KNOB);
    }
#endif
#if LV_USE_SWITCH
    else if (lv_obj_check_type(obj, &lv_switch_class)) {
        lv_obj_add_style(obj, &s.sw_main, 0);
        lv_obj_add_style(obj, &s.sw_ind, LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_add_style(obj, &s.sw_knob, LV_PART_KNOB);
        lv_obj_add_style(obj, &s.sw_knob_on, LV_PART_KNOB | LV_STATE_CHECKED);
        /* 48 dp touch target around the small track. */
        lv_obj_set_ext_click_area(obj, T5_DP(MD_TOUCH_DP - MD_SWITCH_TRACK_H_DP) / 2);
    }
#endif
#if LV_USE_CHECKBOX
    else if (lv_obj_check_type(obj, &lv_checkbox_class)) {
        lv_obj_add_style(obj, &s.cb_main, 0);
        lv_obj_add_style(obj, &s.cb_box, LV_PART_INDICATOR);
        lv_obj_add_style(obj, &s.cb_box_on, LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_ext_click_area(obj, T5_DP(MD_TOUCH_DP - MD_CHECKBOX_DP) / 2);
    }
#endif
#if LV_USE_TEXTAREA
    else if (lv_obj_check_type(obj, &lv_textarea_class)) {
        lv_obj_add_style(obj, &s.ta, 0);
        lv_obj_add_style(obj, &s.ta_focus, LV_STATE_FOCUSED);
        lv_obj_add_style(obj, &s.ta_cursor, LV_PART_CURSOR | LV_STATE_FOCUSED);
        lv_obj_add_style(obj, &s.scrollbar, LV_PART_SCROLLBAR);
    }
#endif
#if LV_USE_BAR
    else if (lv_obj_check_type(obj, &lv_bar_class)) {
        lv_obj_add_style(obj, &s.bar_main, 0);
        lv_obj_add_style(obj, &s.bar_ind, LV_PART_INDICATOR);
    }
#endif
#if LV_USE_ARC
    else if (lv_obj_check_type(obj, &lv_arc_class)) {
        lv_obj_add_style(obj, &s.arc_main, 0);
        lv_obj_add_style(obj, &s.arc_ind, LV_PART_INDICATOR);
    }
#endif
#if LV_USE_LIST
    else if (lv_obj_check_type(obj, &lv_list_class)) {
        lv_obj_add_style(obj, &s.list, 0);
        lv_obj_add_style(obj, &s.scrollbar, LV_PART_SCROLLBAR);
    }
#endif
#if LV_USE_DROPDOWN
    else if (lv_obj_check_type(obj, &lv_dropdown_class)) {
        lv_obj_add_style(obj, &s.dropdown, 0);
    } else if (lv_obj_check_type(obj, &lv_dropdownlist_class)) {
        lv_obj_add_style(obj, &s.dropdown_list, 0);
        lv_obj_add_style(obj, &s.dropdown_sel, LV_PART_SELECTED | LV_STATE_CHECKED);
        lv_obj_add_style(obj, &s.dropdown_sel, LV_PART_SELECTED | LV_STATE_PRESSED);
        lv_obj_add_style(obj, &s.scrollbar, LV_PART_SCROLLBAR);
    }
#endif
#if LV_USE_KEYBOARD
    else if (lv_obj_check_type(obj, &lv_keyboard_class)) {
        lv_obj_add_style(obj, &s.kb, 0);
        lv_obj_add_style(obj, &s.kb_btn, LV_PART_ITEMS);
    }
#endif
#if LV_USE_BUTTONMATRIX
    else if (lv_obj_check_type(obj, &lv_buttonmatrix_class)) {
        if (lv_obj_check_type(parent, &lv_tabview_class)) {
            lv_obj_add_style(obj, &s.tab_btn, LV_PART_ITEMS);
            lv_obj_add_style(obj, &s.tab_btn_on, LV_PART_ITEMS | LV_STATE_CHECKED);
        }
    }
#endif
}

static void install(void)
{
    md_scheme_build(&s_scheme, s_dark, s_primary, s_secondary);
    build_styles();
    md_shadow_scheme_changed();
    if (!s_disp) return;
    lv_theme_t *parent = lv_theme_default_init(s_disp, md_color(s_scheme.c.primary), md_color(s_scheme.c.secondary),
                                               s_dark, md_font(MD_TYPE_BODY1));
    if (!s_theme) {
        s_theme = lv_theme_create();
        lv_theme_set_apply_cb(s_theme, apply_cb);
    }
    lv_theme_set_parent(s_theme, parent);
    lv_display_set_theme(s_disp, s_theme);
}

void md_init(lv_display_t *disp)
{
    s_disp = disp;
    install();
}

void md_set_accent(md_accent_t primary, md_accent_t secondary)
{
    s_primary = primary;
    s_secondary = secondary;
    install();
    lv_obj_report_style_change(NULL);
}

void md_set_dark(bool dark)
{
    s_dark = dark;
    install();
    lv_obj_report_style_change(NULL);
}

void md_anim_set_curve(lv_anim_t *a, md_curve_t curve)
{
    const md_bezier_t *b = md_curve(curve);
    lv_anim_set_path_cb(a, lv_anim_path_custom_bezier3);
    lv_anim_set_bezier3_param(a, (int16_t)LV_BEZIER_VAL_FLOAT(b->x1), (int16_t)LV_BEZIER_VAL_FLOAT(b->y1),
                              (int16_t)LV_BEZIER_VAL_FLOAT(b->x2), (int16_t)LV_BEZIER_VAL_FLOAT(b->y2));
}
