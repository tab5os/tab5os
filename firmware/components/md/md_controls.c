/* SPDX-License-Identifier: Apache-2.0
 * The rest of the MD2 widget set (plan 6.1, 6.2): text field with a floating
 * label, switch, slider, checkbox, chip, tabs, menu, static list rows,
 * snackbar and navigation drawer. Same rules as md_widgets.c: sizes in dp,
 * square surfaces where the spec allows (PPA fills), fades rather than
 * animated shadows.
 */
#include <stdlib.h>
#include <string.h>
#include "md/md.h"
#include "md/md_icons.h"
#include "md_priv.h"

static void free_ud(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void set_opa_anim(void *obj, int32_t v)
{
    lv_obj_set_style_opa(obj, (lv_opa_t)v, 0);
}

static void set_x_anim(void *obj, int32_t v)
{
    lv_obj_set_x(obj, v);
}

static void set_y_anim(void *obj, int32_t v)
{
    lv_obj_set_y(obj, v);
}

static void delete_on_done(lv_anim_t *a)
{
    lv_obj_delete(a->var);
}

static void anim(void *var, lv_anim_exec_xcb_t cb, int32_t from, int32_t to, uint32_t ms, md_curve_t curve,
                 lv_anim_completed_cb_t done)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, var);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    md_anim_set_curve(&a, curve);
    if (done) lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

/* ---- divider ------------------------------------------------------------------ */
lv_obj_t *md_divider_create(lv_obj_t *parent)
{
    lv_obj_t *d = lv_obj_create(parent);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, LV_PCT(100), 1);   /* px: hairline */
    lv_obj_set_style_bg_color(d, md_color(md_scheme()->c.on_surface), 0);
    lv_obj_set_style_bg_opa(d, md_scheme()->on_surface.divider, 0);
    return d;
}

/* ---- text field (filled, floating label) ---------------------------------------- */
typedef struct {
    lv_obj_t *ta, *label, *helper;
    bool floated;
} field_t;

#define FIELD_LABEL_REST_Y 18     /* dp from the top, label as placeholder */
#define FIELD_LABEL_UP_Y 8        /* dp, label floated */

static void field_float(field_t *f, bool up)
{
    if (f->floated == up) return;
    f->floated = up;
    /* MD2 animates scale and position; LVGL fonts are bitmaps, so switch the
     * size and slide the label (150 ms, standard curve). */
    md_text_style(f->label, up ? MD_TYPE_CAPTION : MD_TYPE_SUBTITLE1, md_scheme()->on_surface.medium);
    int32_t from = lv_obj_get_y(f->label);
    int32_t to = T5_DP(up ? FIELD_LABEL_UP_Y : FIELD_LABEL_REST_Y);
    anim(f->label, set_y_anim, from, to, MD_DUR_DIALOG_IN_MS, MD_CURVE_STANDARD, NULL);
}

static void field_event(lv_event_t *e)
{
    field_t *f = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    bool focused = lv_obj_has_state(f->ta, LV_STATE_FOCUSED);
    bool empty = !lv_textarea_get_text(f->ta)[0];
    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_DEFOCUSED || code == LV_EVENT_VALUE_CHANGED) {
        field_float(f, focused || !empty);
        lv_obj_set_style_text_color(f->label, md_color(focused ? md_scheme()->c.primary : md_scheme()->c.on_surface), 0);
    }
}

lv_obj_t *md_textfield_create(lv_obj_t *parent, const char *label, bool password)
{
    field_t *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, T5_DP(4), 0);
    lv_obj_set_scrollable(box, false);
    lv_obj_set_user_data(box, f);
    lv_obj_add_event_cb(box, free_ud, LV_EVENT_DELETE, f);

    /* The frame holds the text area and, drawn over it, the label. The
     * label isn't clickable, so taps fall through to the text area. */
    lv_obj_t *frame = lv_obj_create(box);
    lv_obj_remove_style_all(frame);
    lv_obj_set_size(frame, LV_PCT(100), T5_DP(MD_TEXT_FIELD_H_DP));
    lv_obj_set_scrollable(frame, false);

    f->ta = lv_textarea_create(frame);
    lv_obj_set_size(f->ta, LV_PCT(100), LV_PCT(100));
    lv_textarea_set_one_line(f->ta, true);
    lv_textarea_set_password_mode(f->ta, password);
    md_text_style(f->ta, MD_TYPE_SUBTITLE1, md_scheme()->on_surface.high);
    lv_obj_set_style_pad_top(f->ta, T5_DP(24), 0);    /* text under the floated label */
    lv_obj_set_style_pad_bottom(f->ta, T5_DP(6), 0);
    lv_obj_set_scrollbar_mode(f->ta, LV_SCROLLBAR_MODE_OFF);

    f->label = md_label_create(frame, label ? label : "", MD_TYPE_SUBTITLE1);
    lv_obj_set_style_text_opa(f->label, md_scheme()->on_surface.medium, 0);
    lv_obj_set_clickable(f->label, false);
    lv_obj_set_pos(f->label, T5_DP(16), T5_DP(FIELD_LABEL_REST_Y));

    f->helper = md_label_create(box, "", MD_TYPE_CAPTION);
    lv_obj_set_style_pad_left(f->helper, T5_DP(16), 0);
    lv_obj_set_style_text_opa(f->helper, md_scheme()->on_surface.medium, 0);
    lv_obj_set_hidden(f->helper, true);

    lv_obj_add_event_cb(f->ta, field_event, LV_EVENT_FOCUSED, f);
    lv_obj_add_event_cb(f->ta, field_event, LV_EVENT_DEFOCUSED, f);
    lv_obj_add_event_cb(f->ta, field_event, LV_EVENT_VALUE_CHANGED, f);
    return box;
}

lv_obj_t *md_textfield_ta(lv_obj_t *field)
{
    field_t *f = field ? lv_obj_get_user_data(field) : NULL;
    return f ? f->ta : NULL;
}

const char *md_textfield_text(lv_obj_t *field)
{
    lv_obj_t *ta = md_textfield_ta(field);
    return ta ? lv_textarea_get_text(ta) : "";
}

void md_textfield_set_text(lv_obj_t *field, const char *text)
{
    field_t *f = lv_obj_get_user_data(field);
    if (!f) return;
    lv_textarea_set_text(f->ta, text ? text : "");
    field_float(f, (text && text[0]) || lv_obj_has_state(f->ta, LV_STATE_FOCUSED));
}

void md_textfield_set_helper(lv_obj_t *field, const char *text, bool error)
{
    field_t *f = lv_obj_get_user_data(field);
    if (!f) return;
    const md_scheme_t *sc = md_scheme();
    lv_obj_set_hidden(f->helper, !text || !text[0]);
    lv_label_set_text(f->helper, text ? text : "");
    lv_obj_set_style_text_color(f->helper, md_color(error ? sc->c.error : sc->c.on_surface), 0);
    lv_obj_set_style_text_opa(f->helper, error ? LV_OPA_COVER : sc->on_surface.medium, 0);
    lv_obj_set_style_border_color(f->ta, md_color(error ? sc->c.error : sc->c.primary), LV_STATE_FOCUSED);
}

/* ---- selection controls ------------------------------------------------------------ */
lv_obj_t *md_switch_create(lv_obj_t *parent, bool on)
{
    lv_obj_t *sw = lv_switch_create(parent);   /* theme: track, handle, 48 dp touch */
    lv_obj_set_size(sw, T5_DP(MD_SWITCH_TRACK_W_DP), T5_DP(MD_SWITCH_TRACK_H_DP));
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    return sw;
}

lv_obj_t *md_slider_create(lv_obj_t *parent, int32_t min, int32_t max, int32_t value)
{
    lv_obj_t *s = lv_slider_create(parent);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, value, LV_ANIM_OFF);
    lv_obj_set_height(s, T5_DP(MD_SLIDER_TRACK_DP));
    lv_obj_set_ext_click_area(s, T5_DP(MD_TOUCH_DP - MD_SLIDER_TRACK_DP) / 2);
    return s;
}

lv_obj_t *md_checkbox_create(lv_obj_t *parent, const char *text, bool checked)
{
    lv_obj_t *cb = lv_checkbox_create(parent);
    lv_checkbox_set_text(cb, text ? text : "");
    md_text_style(cb, MD_TYPE_BODY1, md_scheme()->on_surface.high);
    if (checked) lv_obj_add_state(cb, LV_STATE_CHECKED);
    return cb;
}

/* ---- chips ---------------------------------------------------------------------------- */
typedef struct {
    md_chip_kind_t kind;
    lv_obj_t *check;          /* filter chips: leading check when selected */
} chip_t;

static void chip_style(lv_obj_t *chip, bool selected)
{
    const md_scheme_t *sc = md_scheme();
    chip_t *c = lv_obj_get_user_data(chip);
    lv_obj_set_style_bg_color(chip, md_color(selected ? sc->c.primary : sc->c.on_surface), 0);
    lv_obj_set_style_bg_opa(chip, selected ? (lv_opa_t)(sc->states.selected * 2 + 20) : LV_OPA_10 + 2, 0);
    lv_obj_set_style_text_color(chip, md_color(selected ? sc->c.primary : sc->c.on_surface), 0);
    if (c && c->check) lv_obj_set_hidden(c->check, !selected);
}

static void chip_clicked(lv_event_t *e)
{
    lv_obj_t *chip = lv_event_get_current_target_obj(e);
    chip_t *c = lv_obj_get_user_data(chip);
    if (!c || c->kind == MD_CHIP_ACTION) return;
    if (c->kind == MD_CHIP_CHOICE) {
        /* One selected among its siblings. */
        lv_obj_t *p = lv_obj_get_parent(chip);
        for (uint32_t i = 0; i < lv_obj_get_child_count(p); i++) {
            lv_obj_t *s = lv_obj_get_child(p, (int32_t)i);
            chip_t *sc = lv_obj_get_user_data(s);
            if (s != chip && sc && sc->kind == MD_CHIP_CHOICE && lv_obj_has_state(s, LV_STATE_CHECKED)) {
                lv_obj_remove_state(s, LV_STATE_CHECKED);
                chip_style(s, false);
            }
        }
        lv_obj_add_state(chip, LV_STATE_CHECKED);
    } else if (lv_obj_has_state(chip, LV_STATE_CHECKED)) {
        lv_obj_remove_state(chip, LV_STATE_CHECKED);
    } else {
        lv_obj_add_state(chip, LV_STATE_CHECKED);
    }
    chip_style(chip, lv_obj_has_state(chip, LV_STATE_CHECKED));
    lv_obj_send_event(chip, LV_EVENT_VALUE_CHANGED, NULL);
}

lv_obj_t *md_chip_create(lv_obj_t *parent, const char *text, const char *icon, md_chip_kind_t kind)
{
    chip_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->kind = kind;
    lv_obj_t *chip = lv_obj_create(parent);
    lv_obj_remove_style_all(chip);
    lv_obj_set_user_data(chip, c);
    lv_obj_add_event_cb(chip, free_ud, LV_EVENT_DELETE, c);
    lv_obj_set_size(chip, LV_SIZE_CONTENT, T5_DP(MD_CHIP_H_DP));
    lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(chip, T5_DP(12), 0);
    lv_obj_set_style_pad_column(chip, T5_DP(8), 0);
    lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(chip, false);
    lv_obj_set_clickable(chip, true);
    lv_obj_set_ext_click_area(chip, T5_DP(MD_TOUCH_DP - MD_CHIP_H_DP) / 2);
    md_ripple_attach(chip);
    if (kind == MD_CHIP_FILTER) {
        c->check = md_icon_create(chip, MD_ICON_CHECK, 18);
        lv_obj_set_hidden(c->check, true);
    }
    if (icon) md_icon_create(chip, icon, 18);
    md_label_create(chip, text ? text : "", MD_TYPE_BODY2);   /* inherits the chip's colour */
    chip_style(chip, false);
    lv_obj_add_event_cb(chip, chip_clicked, LV_EVENT_CLICKED, NULL);
    return chip;
}

bool md_chip_selected(lv_obj_t *chip)
{
    return chip && lv_obj_has_state(chip, LV_STATE_CHECKED);
}

void md_chip_set_selected(lv_obj_t *chip, bool on)
{
    if (!chip) return;
    if (on) lv_obj_add_state(chip, LV_STATE_CHECKED);
    else lv_obj_remove_state(chip, LV_STATE_CHECKED);
    chip_style(chip, on);
}

/* ---- tabs ------------------------------------------------------------------------------- */
lv_obj_t *md_tabs_create(lv_obj_t *parent)
{
    const md_scheme_t *sc = md_scheme();
    lv_obj_t *tv = lv_tabview_create(parent);
    lv_tabview_set_tab_bar_size(tv, T5_DP(MD_TABS_H_DP));
    lv_obj_t *bar = lv_tabview_get_tab_bar(tv);
    /* Square, full width, the app bar's surface: a PPA fill. */
    md_set_elevation(bar, MD_ELEV_APP_BAR, false);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_t *content = lv_tabview_get_content(tv);
    lv_obj_set_style_bg_color(content, md_color(sc->c.background), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    return tv;
}

lv_obj_t *md_tabs_add(lv_obj_t *tabs, const char *name)
{
    const md_scheme_t *sc = md_scheme();
    lv_obj_t *page = lv_tabview_add_tab(tabs, name);
    lv_obj_set_style_pad_all(page, T5_DP(MD_MARGIN_DP), 0);
    lv_obj_t *bar = lv_tabview_get_tab_bar(tabs);
    uint32_t n = lv_obj_get_child_count(bar);
    lv_obj_t *btn = lv_obj_get_child(bar, (int32_t)n - 1);
    if (btn) {
        lv_obj_add_style(btn, md_style_btn_text(), 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_set_style_text_color(btn, md_color(sc->c.on_surface), 0);
        lv_obj_set_style_text_opa(btn, sc->on_surface.medium, 0);
        lv_obj_set_style_text_color(btn, md_color(sc->c.primary), LV_STATE_CHECKED);
        lv_obj_set_style_text_opa(btn, LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_BOTTOM, LV_STATE_CHECKED);
        lv_obj_set_style_border_width(btn, T5_DP(2), LV_STATE_CHECKED);
        lv_obj_set_style_border_color(btn, md_color(sc->c.primary), LV_STATE_CHECKED);
        lv_obj_t *l = lv_obj_get_child(btn, 0);
        if (l) md_label_set_text(l, name, MD_TYPE_BUTTON);
        if (l) lv_obj_remove_local_style_prop(l, LV_STYLE_TEXT_OPA, 0);
    }
    return page;
}

/* ---- static list rows (settings pages) --------------------------------------------------- */
typedef struct {
    lv_obj_t *primary, *secondary, *icon, *trailing;
} row_t;

lv_obj_t *md_row_create(lv_obj_t *parent, const char *icon, const char *primary, const char *secondary)
{
    const md_scheme_t *sc = md_scheme();
    row_t *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_user_data(row, r);
    lv_obj_add_event_cb(row, free_ud, LV_EVENT_DELETE, r);
    /* Grows when the secondary line wraps, instead of clipping it: two short
     * lines plus 8 dp above and below come to exactly the spec's 64 dp. */
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, T5_DP(secondary ? MD_LIST_ROW_2_DP : MD_LIST_ROW_1_DP), 0);
    lv_obj_set_style_pad_hor(row, T5_DP(16), 0);
    lv_obj_set_style_pad_ver(row, T5_DP(8), 0);
    lv_obj_set_style_pad_column(row, T5_DP(16), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_style_text_color(row, md_color(sc->c.on_surface), 0);
    if (icon) {
        r->icon = md_icon_create(row, icon, MD_ICON_DP);
        lv_obj_set_style_text_opa(r->icon, sc->on_surface.medium, 0);
        lv_obj_set_style_min_width(r->icon, T5_DP(40), 0);
    }
    lv_obj_t *text = lv_obj_create(row);
    lv_obj_remove_style_all(text);
    lv_obj_set_height(text, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_clickable(text, false);
    lv_obj_set_event_bubble(text, true);
    r->primary = md_label_create(text, primary ? primary : "", MD_TYPE_SUBTITLE1);
    lv_obj_set_width(r->primary, LV_PCT(100));
    lv_label_set_long_mode(r->primary, LV_LABEL_LONG_MODE_DOTS);
    r->secondary = md_label_create(text, secondary ? secondary : "", MD_TYPE_BODY2);
    lv_obj_set_width(r->secondary, LV_PCT(100));
    lv_label_set_long_mode(r->secondary, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_opa(r->secondary, sc->on_surface.medium, 0);
    lv_obj_set_hidden(r->secondary, !secondary);
    return row;
}

void md_row_set_clickable(lv_obj_t *row, lv_event_cb_t cb, void *user_data)
{
    lv_obj_set_clickable(row, true);
    md_ripple_attach(row);
    if (cb) lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
}

void md_row_set_text(lv_obj_t *row, const char *primary)
{
    row_t *r = row ? lv_obj_get_user_data(row) : NULL;
    if (r && primary) lv_label_set_text(r->primary, primary);
}

void md_row_set_secondary(lv_obj_t *row, const char *text)
{
    row_t *r = row ? lv_obj_get_user_data(row) : NULL;
    if (!r) return;
    lv_label_set_text(r->secondary, text ? text : "");
    bool show = text && text[0];
    lv_obj_set_hidden(r->secondary, !show);
    lv_obj_set_height(row, T5_DP(show ? MD_LIST_ROW_2_DP : MD_LIST_ROW_1_DP));
}

void md_row_set_icon(lv_obj_t *row, const char *icon)
{
    row_t *r = row ? lv_obj_get_user_data(row) : NULL;
    if (r && r->icon && icon) lv_label_set_text(r->icon, icon);
}

lv_obj_t *md_row_switch(lv_obj_t *row, bool on)
{
    lv_obj_t *sw = md_switch_create(row, on);
    row_t *r = lv_obj_get_user_data(row);
    if (r) r->trailing = sw;
    return sw;
}

lv_obj_t *md_row_trailing_text(lv_obj_t *row, const char *text)
{
    lv_obj_t *l = md_label_create(row, text ? text : "", MD_TYPE_BODY2);
    lv_obj_set_style_text_opa(l, md_scheme()->on_surface.medium, 0);
    row_t *r = lv_obj_get_user_data(row);
    if (r) r->trailing = l;
    return l;
}

lv_obj_t *md_section_create(lv_obj_t *parent, const char *title)
{
    /* A group header: body2 size, medium weight, accent colour, sentence
     * case, on the 16 dp margin. */
    lv_obj_t *l = md_label_create(parent, title ? title : "", MD_TYPE_OVERLINE);
    lv_obj_set_style_text_font(l, md_font(MD_TYPE_BUTTON), 0);
    lv_obj_set_style_text_letter_space(l, 0, 0);
    lv_obj_set_style_text_color(l, md_color(md_scheme()->c.primary), 0);
    lv_obj_set_style_pad_left(l, T5_DP(16), 0);
    lv_obj_set_style_pad_top(l, T5_DP(20), 0);
    lv_obj_set_style_pad_bottom(l, T5_DP(4), 0);
    return l;
}

/* ---- page: app bar plus a scrolling column ---------------------------------------------- */
lv_obj_t *md_page_create(lv_obj_t *parent, const char *title, lv_event_cb_t back_cb, void *user_data,
                         lv_obj_t **bar_out)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(page, md_color(md_scheme()->c.background), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(page, false);
    lv_obj_t *bar = md_appbar_create(page, title, back_cb ? MD_ICON_ARROW_BACK : NULL, back_cb, user_data);
    if (bar_out) *bar_out = bar;
    lv_obj_t *body = lv_obj_create(page);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_style_pad_bottom(body, T5_DP(16), 0);
    lv_obj_set_user_data(page, body);
    return page;
}

lv_obj_t *md_page_body(lv_obj_t *page)
{
    return page ? lv_obj_get_user_data(page) : NULL;
}

/* ---- menu ------------------------------------------------------------------------------- */
typedef struct {
    lv_obj_t *catcher, *surface;
    md_menu_cb_t cb;
    void *ud;
    bool closing;
} menu_t;

void md_menu_close(lv_obj_t *catcher)
{
    menu_t *m = catcher ? lv_obj_get_user_data(catcher) : NULL;
    if (!m || m->closing) return;
    m->closing = true;
    anim(catcher, set_opa_anim, LV_OPA_COVER, LV_OPA_TRANSP, MD_DUR_MENU_FADE_OUT_MS, MD_CURVE_ACCELERATION,
         delete_on_done);
}

static void menu_item(lv_event_t *e)
{
    lv_obj_t *catcher = lv_event_get_user_data(e);
    menu_t *m = lv_obj_get_user_data(catcher);
    if (m->closing) return;
    int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    if (m->cb) m->cb(idx, m->ud);
    md_menu_close(catcher);
}

static void menu_outside(lv_event_t *e)
{
    lv_obj_t *catcher = lv_event_get_current_target_obj(e);
    if (lv_event_get_target_obj(e) != catcher) return;
    menu_t *m = lv_obj_get_user_data(catcher);
    if (m->closing) return;
    if (m->cb) m->cb(-1, m->ud);
    md_menu_close(catcher);
}

lv_obj_t *md_menu_show(lv_obj_t *anchor, const char *const *items, int n, md_menu_cb_t cb, void *user_data)
{
    menu_t *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->cb = cb;
    m->ud = user_data;
    lv_obj_t *catcher = lv_obj_create(lv_layer_top());
    m->catcher = catcher;
    lv_obj_remove_style_all(catcher);
    lv_obj_set_size(catcher, LV_PCT(100), LV_PCT(100));
    lv_obj_set_clickable(catcher, true);
    lv_obj_set_user_data(catcher, m);
    lv_obj_add_event_cb(catcher, free_ud, LV_EVENT_DELETE, m);
    lv_obj_add_event_cb(catcher, menu_outside, LV_EVENT_CLICKED, NULL);

    lv_obj_t *s = lv_obj_create(catcher);
    m->surface = s;
    md_set_elevation(s, MD_ELEV_MENU, true);
    lv_obj_set_style_radius(s, T5_DP(MD_SHAPE_MEDIUM_DP), 0);
    lv_obj_set_style_pad_hor(s, 0, 0);
    lv_obj_set_style_pad_ver(s, T5_DP(8), 0);
    lv_obj_set_style_pad_row(s, 0, 0);
    lv_obj_set_size(s, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_min_width(s, T5_DP(112), 0);
    lv_obj_set_style_max_width(s, T5_DP(280), 0);
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(s, false);
    for (int i = 0; i < n; i++) {
        lv_obj_t *it = lv_obj_create(s);
        lv_obj_remove_style_all(it);
        /* Content-sized: a percentage of a content-sized menu measures as 0. */
        lv_obj_set_size(it, LV_SIZE_CONTENT, T5_DP(MD_LIST_ROW_1_DP));
        lv_obj_set_style_min_width(it, T5_DP(112), 0);
        lv_obj_set_style_pad_hor(it, T5_DP(16), 0);
        lv_obj_set_clickable(it, true);
        lv_obj_set_user_data(it, (void *)(intptr_t)i);
        md_ripple_attach(it);
        lv_obj_t *l = md_label_create(it, items[i], MD_TYPE_SUBTITLE1);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_add_event_cb(it, menu_item, LV_EVENT_CLICKED, catcher);
    }
    /* Every item as wide as the widest, so ripples and taps cover the row. */
    lv_obj_update_layout(catcher);
    int32_t widest = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(s); i++) {
        int32_t w = lv_obj_get_width(lv_obj_get_child(s, (int32_t)i));
        if (w > widest) widest = w;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(s); i++) lv_obj_set_width(lv_obj_get_child(s, (int32_t)i), widest);
    /* Under the anchor, right-aligned to it, kept on screen. */
    lv_obj_update_layout(catcher);
    int32_t sw = lv_obj_get_width(catcher), sh = lv_obj_get_height(catcher);
    int32_t w = lv_obj_get_width(s), h = lv_obj_get_height(s);
    int32_t x = sw - w - T5_DP(8), y = T5_DP(8);
    if (anchor) {
        lv_area_t a;
        lv_obj_get_coords(anchor, &a);
        x = a.x2 - w;
        y = a.y1;
        if (x < T5_DP(8)) x = T5_DP(8);
    }
    if (x + w > sw - T5_DP(8)) x = sw - w - T5_DP(8);
    if (y + h > sh - T5_DP(8)) y = sh - h - T5_DP(8);
    if (y < T5_DP(8)) y = T5_DP(8);
    lv_obj_set_pos(s, x, y);
    /* 120 ms scale is approximated by a short slide; fade in over 30 ms. */
    anim(s, set_y_anim, y - T5_DP(8), y, MD_DUR_MENU_SCALE_MS, MD_CURVE_DECELERATION, NULL);
    anim(catcher, set_opa_anim, LV_OPA_TRANSP, LV_OPA_COVER, MD_DUR_MENU_FADE_IN_MS + 60, MD_CURVE_STANDARD, NULL);
    return catcher;
}

/* ---- snackbar ------------------------------------------------------------------------------ */
#define SNACK_SHOW_MS 4000
#define SNACK_LONG_MS 10000

typedef struct {
    lv_obj_t *bar;
    md_action_cb_t cb;
    void *ud;
    lv_timer_t *timer;
    bool closing;
} snack_t;

static lv_obj_t *s_snack;   /* one at a time */

static void snack_gone(lv_anim_t *a)
{
    lv_obj_delete(a->var);
}

static void snack_close(lv_obj_t *bar)
{
    snack_t *sn = lv_obj_get_user_data(bar);
    if (!sn || sn->closing) return;
    sn->closing = true;
    if (sn->timer) {
        lv_timer_delete(sn->timer);
        sn->timer = NULL;
    }
    if (s_snack == bar) s_snack = NULL;
    anim(bar, set_opa_anim, LV_OPA_COVER, LV_OPA_TRANSP, MD_DUR_DIALOG_OUT_MS * 2, MD_CURVE_ACCELERATION, snack_gone);
}

static void snack_timeout(lv_timer_t *t)
{
    lv_obj_t *bar = lv_timer_get_user_data(t);
    snack_t *sn = lv_obj_get_user_data(bar);
    sn->timer = NULL;          /* auto-deleted (repeat count 1) */
    snack_close(bar);
}

static void snack_deleted(lv_event_t *e)
{
    snack_t *sn = lv_event_get_user_data(e);
    if (sn->timer) lv_timer_delete(sn->timer);
    if (s_snack == sn->bar) s_snack = NULL;
    free(sn);
}

static void snack_action(lv_event_t *e)
{
    lv_obj_t *bar = lv_event_get_user_data(e);
    snack_t *sn = lv_obj_get_user_data(bar);
    if (sn->closing) return;
    if (sn->cb) sn->cb(sn->ud);
    snack_close(bar);
}

lv_obj_t *md_snackbar_show(const char *text, const char *action, md_action_cb_t cb, void *user_data)
{
    if (s_snack) {
        /* MD2: a new snackbar replaces the old one straight away. */
        lv_obj_t *old = s_snack;
        s_snack = NULL;
        lv_obj_delete(old);
    }
    snack_t *sn = calloc(1, sizeof *sn);
    if (!sn) return NULL;
    sn->cb = cb;
    sn->ud = user_data;
    lv_obj_t *bar = lv_obj_create(lv_layer_top());
    sn->bar = bar;
    s_snack = bar;
    lv_obj_set_user_data(bar, sn);
    lv_obj_add_event_cb(bar, snack_deleted, LV_EVENT_DELETE, sn);
    /* Dark theme snackbars are light grey (#333 on light; MD2 dark uses a
     * light surface at high emphasis). */
    const md_scheme_t *sc = md_scheme();
    md_set_elevation(bar, MD_ELEV_SNACKBAR, true);
    lv_obj_set_style_bg_color(bar, md_color(sc->dark ? md_mix(0xFFFFFF, sc->c.surface, 222) : 0x333333), 0);
    lv_obj_set_style_text_color(bar, md_color(sc->dark ? 0x000000 : 0xFFFFFF), 0);
    lv_obj_set_style_radius(bar, T5_DP(MD_SHAPE_SMALL_DP), 0);
    lv_obj_set_style_pad_left(bar, T5_DP(16), 0);
    lv_obj_set_style_pad_right(bar, T5_DP(8), 0);
    lv_obj_set_style_pad_ver(bar, T5_DP(6), 0);
    lv_obj_set_style_min_height(bar, T5_DP(48), 0);
    lv_obj_set_style_min_width(bar, T5_DP(MD_SNACKBAR_MIN_W_DP), 0);
    lv_obj_set_style_max_width(bar, T5_DP(MD_SNACKBAR_MAX_W_DP), 0);
    lv_obj_set_size(bar, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, T5_DP(8), 0);
    lv_obj_set_scrollable(bar, false);
    lv_obj_t *l = md_label_create(bar, text ? text : "", MD_TYPE_BODY2);
    lv_obj_set_style_text_opa(l, 222, 0);
    lv_obj_set_style_text_color(l, md_color(sc->dark ? 0x000000 : 0xFFFFFF), 0);
    lv_obj_set_flex_grow(l, 1);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_max_width(l, T5_DP(MD_SNACKBAR_MAX_W_DP - 120), 0);
    if (action) {
        lv_obj_t *b = md_button_create(bar, action, MD_BUTTON_TEXT);
        lv_obj_set_style_text_color(b, md_color(md_accent(md_get_accent())->light), 0);
        lv_obj_add_event_cb(b, snack_action, LV_EVENT_CLICKED, bar);
    }
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -T5_DP(8));
    anim(bar, set_opa_anim, LV_OPA_TRANSP, LV_OPA_COVER, MD_DUR_DIALOG_IN_MS, MD_CURVE_DECELERATION, NULL);
    sn->timer = lv_timer_create(snack_timeout, action ? SNACK_LONG_MS : SNACK_SHOW_MS, bar);
    lv_timer_set_repeat_count(sn->timer, 1);
    return bar;
}

void md_snackbar_dismiss(void)
{
    if (s_snack) snack_close(s_snack);
}

/* ---- navigation drawer ---------------------------------------------------------------------- */
/* As a dialog (md_widgets.c): the scrim shows at once rather than fading,
 * which would redraw the whole screen every frame (~80 ms a frame live).
 * After the scrim's first frame the screen behind is frozen with the scrim
 * baked in, and once every framebuffer has it the panel slides: each frame
 * then redraws the panel's sweep over a snapshot blit. */
#define DRAWER_SCRIM_OPA 82
typedef struct {
    lv_obj_t *scrim, *panel, *backdrop;
    md_settle_t settle;
    bool closing, baked, waiting_frame, prepare_queued, deleting;
} drawer_t;

static void drawer_close_done(lv_anim_t *a)
{
    lv_obj_delete(lv_obj_get_parent(a->var));   /* the panel's scrim */
}

static void drawer_first_frame(lv_event_t *e);
static void drawer_prepare(void *arg);

void md_drawer_close(lv_obj_t *scrim)
{
    drawer_t *d = scrim ? lv_obj_get_user_data(scrim) : NULL;
    if (!d || d->closing) return;
    d->closing = true;
    md_settle_stop(&d->settle);
    if (d->waiting_frame) {
        lv_display_remove_event_cb_with_user_data(lv_obj_get_display(scrim), drawer_first_frame, d);
        d->waiting_frame = false;
    }
    lv_anim_delete(d->panel, set_x_anim);
    anim(d->panel, set_x_anim, lv_obj_get_x(d->panel), -T5_DP(MD_DRAWER_W_DP), MD_DUR_DRAWER_OUT_MS, MD_CURVE_SHARP,
         drawer_close_done);
}

static void drawer_scrim_clicked(lv_event_t *e)
{
    lv_obj_t *scrim = lv_event_get_current_target_obj(e);
    if (lv_event_get_target_obj(e) == scrim) md_drawer_close(scrim);
}

static void drawer_bake(lv_layer_t *layer, void *arg)
{
    drawer_t *d = arg;
    md_scrim_draw(layer, d->scrim, DRAWER_SCRIM_OPA, true);
}

/* The scrim over the top-layer objects below it, live, while baked. */
static void drawer_scrim_draw(lv_event_t *e)
{
    drawer_t *d = lv_event_get_user_data(e);
    if (d->baked) md_scrim_draw(lv_event_get_layer(e), d->scrim, DRAWER_SCRIM_OPA, false);
}

static void drawer_thawed(lv_obj_t *scr, void *arg)
{
    (void)scr;
    drawer_t *d = arg;
    d->backdrop = NULL;
    d->baked = false;
    if (!d->deleting) lv_obj_set_style_bg_opa(d->scrim, DRAWER_SCRIM_OPA, 0);
}

static void drawer_slide_when_settled(void *arg)
{
    drawer_t *d = arg;
    anim(d->panel, set_x_anim, -T5_DP(MD_DRAWER_W_DP), 0, MD_DUR_DRAWER_IN_MS, MD_CURVE_DECELERATION, NULL);
}

static void drawer_prepare(void *arg)
{
    drawer_t *d = arg;
    d->prepare_queued = false;
    if (d->closing) return;
    lv_display_t *disp = lv_obj_get_display(d->scrim);
    if ((md_freeze_mode() & MD_FREEZE_DIALOG_BACKDROP) && !md_screens_frozen()) {
        bool inv = lv_display_is_invalidation_enabled(disp);
        lv_display_enable_invalidation(disp, false);   /* same pixels: no frame for the swap */
        lv_obj_t *scr = lv_screen_active();
        if (md_freeze_screen(scr)) {
            d->backdrop = scr;
            d->baked = md_freeze_draw_into(scr, drawer_bake, d);
            md_freeze_set_thaw_cb(scr, drawer_thawed, d);
            if (d->baked) lv_obj_set_style_bg_opa(d->scrim, LV_OPA_TRANSP, 0);
        }
        lv_display_enable_invalidation(disp, inv);
    }
    md_settle_start(&d->settle, drawer_slide_when_settled, d);
}

static void drawer_first_frame(lv_event_t *e)
{
    drawer_t *d = lv_event_get_user_data(e);
    lv_display_remove_event_cb_with_user_data(lv_obj_get_display(d->scrim), drawer_first_frame, d);
    d->waiting_frame = false;
    d->prepare_queued = lv_async_call(drawer_prepare, d) == LV_RESULT_OK;
}

static void drawer_deleted(lv_event_t *e)
{
    drawer_t *d = lv_event_get_user_data(e);
    md_settle_stop(&d->settle);
    if (d->waiting_frame) lv_display_remove_event_cb_with_user_data(lv_obj_get_display(d->scrim), drawer_first_frame, d);
    if (d->prepare_queued) lv_async_call_cancel(drawer_prepare, d);
    if (d->backdrop) {
        d->deleting = true;
        md_thaw_screen(d->backdrop);
    }
    free(d);
}

lv_obj_t *md_drawer_open(const char *title, const char *subtitle)
{
    drawer_t *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    lv_obj_t *scrim = lv_obj_create(lv_layer_top());
    d->scrim = scrim;
    lv_obj_remove_style_all(scrim);
    lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(scrim, md_color(md_scheme()->c.scrim), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(scrim, true);
    lv_obj_set_user_data(scrim, d);
    lv_obj_add_event_cb(scrim, drawer_deleted, LV_EVENT_DELETE, d);
    lv_obj_add_event_cb(scrim, drawer_scrim_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(scrim, drawer_scrim_draw, LV_EVENT_DRAW_MAIN_END, d);

    /* Square, full height: the panel itself is a plain fill. */
    lv_obj_t *p = lv_obj_create(scrim);
    d->panel = p;
    md_set_elevation(p, MD_ELEV_DRAWER, false);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_size(p, T5_DP(MD_DRAWER_W_DP), LV_PCT(100));
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_set_style_pad_row(p, 0, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    if (title) {
        lv_obj_t *hdr = lv_obj_create(p);
        lv_obj_remove_style_all(hdr);
        lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(hdr, T5_DP(16), 0);
        lv_obj_set_style_pad_top(hdr, T5_DP(16), 0);
        lv_obj_set_style_pad_bottom(hdr, T5_DP(8), 0);
        lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_COLUMN);
        md_label_create(hdr, title, MD_TYPE_HEADLINE6);
        if (subtitle) {
            lv_obj_t *s = md_label_create(hdr, subtitle, MD_TYPE_BODY2);
            lv_obj_set_style_text_opa(s, md_scheme()->on_surface.medium, 0);
        }
        md_divider_create(p);
    }
    lv_obj_set_x(p, -T5_DP(MD_DRAWER_W_DP));
    lv_obj_set_style_bg_opa(scrim, DRAWER_SCRIM_OPA, 0);   /* at once; the panel slides after (drawer_prepare) */
    lv_display_add_event_cb(lv_obj_get_display(scrim), drawer_first_frame, LV_EVENT_REFR_READY, d);
    d->waiting_frame = true;
    return scrim;
}

lv_obj_t *md_drawer_panel(lv_obj_t *drawer)
{
    drawer_t *d = drawer ? lv_obj_get_user_data(drawer) : NULL;
    return d ? d->panel : NULL;
}

typedef struct {
    lv_obj_t *drawer;
    md_action_cb_t cb;
    void *ud;
} drawer_item_t;

static void drawer_item_clicked(lv_event_t *e)
{
    drawer_item_t *it = lv_event_get_user_data(e);
    md_drawer_close(it->drawer);       /* deleted after its 200 ms animation */
    if (it->cb) it->cb(it->ud);
}

lv_obj_t *md_drawer_add_item(lv_obj_t *drawer, const char *icon, const char *text, bool selected, md_action_cb_t cb,
                             void *user_data)
{
    const md_scheme_t *sc = md_scheme();
    lv_obj_t *panel = md_drawer_panel(drawer);
    if (!panel) return NULL;
    drawer_item_t *it = calloc(1, sizeof *it);
    if (!it) return NULL;
    *it = (drawer_item_t){ drawer, cb, user_data };
    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_remove_style_all(row);
    lv_obj_add_event_cb(row, free_ud, LV_EVENT_DELETE, it);
    lv_obj_set_size(row, LV_PCT(100), T5_DP(MD_LIST_ROW_1_DP));
    lv_obj_set_style_pad_hor(row, T5_DP(16), 0);
    lv_obj_set_style_pad_column(row, T5_DP(32), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_clickable(row, true);
    lv_obj_set_scrollable(row, false);
    md_ripple_attach(row);
    lv_obj_set_style_text_color(row, md_color(selected ? sc->c.primary : sc->c.on_surface), 0);
    if (selected) {
        lv_obj_set_style_bg_color(row, md_color(sc->c.primary), 0);
        lv_obj_set_style_bg_opa(row, (lv_opa_t)(sc->states.selected * 2), 0);
    }
    if (icon) {
        lv_obj_t *i = md_icon_create(row, icon, MD_ICON_DP);
        lv_obj_set_style_text_opa(i, selected ? LV_OPA_COVER : sc->on_surface.medium, 0);
    }
    md_label_create(row, text ? text : "", MD_TYPE_BODY2);
    lv_obj_add_event_cb(row, drawer_item_clicked, LV_EVENT_CLICKED, it);
    return row;
}
