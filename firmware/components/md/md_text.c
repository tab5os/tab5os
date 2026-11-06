/* SPDX-License-Identifier: Apache-2.0
 * Type roles to fonts (design "Type"), labels and icons.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "md/md.h"
#include "md/md_fonts.h"
#include "md_priv.h"

const lv_font_t *md_font(md_type_role_t role)
{
    switch (role) {
    case MD_TYPE_HEADLINE4: return &md_font_roboto_regular_68;
    case MD_TYPE_HEADLINE5: return &md_font_roboto_regular_48;
    case MD_TYPE_HEADLINE6: return &md_font_roboto_medium_40;
    case MD_TYPE_SUBTITLE1:
    case MD_TYPE_BODY1: return &md_font_roboto_regular_32;
    case MD_TYPE_BODY2: return &md_font_roboto_regular_28;
    case MD_TYPE_BUTTON: return &md_font_roboto_medium_28;
    case MD_TYPE_CAPTION: return &md_font_roboto_regular_24;
    case MD_TYPE_OVERLINE: return &md_font_roboto_medium_24;
    default: return &md_font_roboto_regular_32;
    }
}

const lv_font_t *md_icon_font(int size_dp)
{
    if (size_dp <= 18) return &md_font_icons_36;
    if (size_dp >= 40) return &md_font_icons_80;
    return &md_font_icons_48;
}

void md_text_style(lv_obj_t *label, md_type_role_t role, lv_opa_t emphasis)
{
    const md_type_t *t = md_type(role);
    const lv_font_t *f = md_font(role);
    lv_obj_set_style_text_font(label, f, 0);
    lv_obj_set_style_text_letter_space(label, md_type_tracking_px(role), 0);
    /* MD line height = baseline-to-baseline; LVGL adds line_space between lines. */
    int32_t extra = T5_DP(t->line_dp) - lv_font_get_line_height(f);
    lv_obj_set_style_text_line_space(label, extra > 0 ? extra : 0, 0);
    lv_obj_set_style_text_opa(label, emphasis, 0);
}

void md_label_set_text(lv_obj_t *label, const char *text, md_type_role_t role)
{
    if (!md_type(role)->uppercase || !text) {
        lv_label_set_text(label, text ? text : "");
        return;
    }
    size_t n = strlen(text);
    char *u = malloc(n + 1);
    if (!u) {
        lv_label_set_text(label, text);
        return;
    }
    for (size_t i = 0; i <= n; i++) {
        unsigned char ch = (unsigned char)text[i];
        u[i] = ch < 0x80 ? (char)toupper(ch) : (char)ch;   /* ASCII only; UTF-8 bytes untouched */
    }
    lv_label_set_text(label, u);
    free(u);
}

lv_obj_t *md_label_create(lv_obj_t *parent, const char *text, md_type_role_t role)
{
    lv_obj_t *l = lv_label_create(parent);
    md_text_style(l, role, md_scheme()->on_surface.high);
    md_label_set_text(l, text, role);
    return l;
}

lv_obj_t *md_icon_create(lv_obj_t *parent, const char *icon, int size_dp)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, md_icon_font(size_dp), 0);
    lv_label_set_text(l, icon);
    return l;
}
