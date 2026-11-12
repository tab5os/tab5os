/* SPDX-License-Identifier: Apache-2.0
 * t5ui's core: argument and type checks in front of the backend (see
 * t5ui_backend.h). Nothing here knows what draws the UI.
 */
#include <stdarg.h>
#include <stdio.h>
#include "t5ui_backend.h"

#define I T5UI_T_INT
#define C T5UI_T_COLOR
#define S T5UI_T_STR
#define OB T5UI_T_OBJ
#define L T5UI_F_LOOK

static const uint8_t PROPS[] = {
    [T5UI_P_X] = I | L, [T5UI_P_Y] = I | L, [T5UI_P_WIDTH] = I | L, [T5UI_P_HEIGHT] = I | L,
    [T5UI_P_MIN_WIDTH] = I | L, [T5UI_P_MAX_WIDTH] = I | L, [T5UI_P_MIN_HEIGHT] = I | L,
    [T5UI_P_MAX_HEIGHT] = I | L, [T5UI_P_ALIGN] = I | L,
    [T5UI_P_FLOW] = I | L, [T5UI_P_PLACE_MAIN] = I | L, [T5UI_P_PLACE_CROSS] = I | L,
    [T5UI_P_PLACE_TRACK] = I | L, [T5UI_P_GROW] = I | L, [T5UI_P_FLOATING] = I,
    [T5UI_P_PAD_TOP] = I | L, [T5UI_P_PAD_BOTTOM] = I | L, [T5UI_P_PAD_LEFT] = I | L, [T5UI_P_PAD_RIGHT] = I | L,
    [T5UI_P_PAD_ALL] = I | L | T5UI_F_NOREAD, [T5UI_P_PAD_ROW] = I | L, [T5UI_P_PAD_COLUMN] = I | L,
    [T5UI_P_MARGIN_TOP] = I | L, [T5UI_P_MARGIN_BOTTOM] = I | L, [T5UI_P_MARGIN_LEFT] = I | L,
    [T5UI_P_MARGIN_RIGHT] = I | L,
    [T5UI_P_BG_COLOR] = C | L, [T5UI_P_BG_OPA] = I | L, [T5UI_P_BG_GRAD_COLOR] = C | L, [T5UI_P_BG_GRAD_DIR] = I | L,
    [T5UI_P_BORDER_WIDTH] = I | L, [T5UI_P_BORDER_COLOR] = C | L, [T5UI_P_BORDER_OPA] = I | L,
    [T5UI_P_BORDER_SIDES] = I | L,
    [T5UI_P_OUTLINE_WIDTH] = I | L, [T5UI_P_OUTLINE_COLOR] = C | L, [T5UI_P_OUTLINE_OPA] = I | L,
    [T5UI_P_OUTLINE_PAD] = I | L,
    [T5UI_P_SHADOW_WIDTH] = I | L, [T5UI_P_SHADOW_COLOR] = C | L, [T5UI_P_SHADOW_OPA] = I | L,
    [T5UI_P_SHADOW_OFFSET_X] = I | L, [T5UI_P_SHADOW_OFFSET_Y] = I | L, [T5UI_P_SHADOW_SPREAD] = I | L,
    [T5UI_P_RADIUS] = I | L, [T5UI_P_CLIP_CORNER] = I | L, [T5UI_P_OPA] = I | L, [T5UI_P_ELEVATION] = I,
    [T5UI_P_TRANSLATE_X] = I | L, [T5UI_P_TRANSLATE_Y] = I | L, [T5UI_P_SCALE] = I | L, [T5UI_P_ROTATION] = I | L,
    [T5UI_P_TEXT] = S, [T5UI_P_TEXT_COLOR] = C | L, [T5UI_P_TEXT_OPA] = I | L, [T5UI_P_TEXT_STYLE] = I | L,
    [T5UI_P_TEXT_ALIGN] = I | L, [T5UI_P_LETTER_SPACE] = I | L, [T5UI_P_LINE_SPACE] = I | L,
    [T5UI_P_TEXT_DECOR] = I | L, [T5UI_P_LONG_MODE] = I, [T5UI_P_SECONDARY] = S, [T5UI_P_ICON] = S,
    [T5UI_P_ICON_SIZE] = I, [T5UI_P_PLACEHOLDER] = S, [T5UI_P_HELPER] = S, [T5UI_P_HELPER_ERROR] = I,
    [T5UI_P_TITLE] = S,
    [T5UI_P_LINE_WIDTH] = I | L, [T5UI_P_LINE_COLOR] = C | L, [T5UI_P_LINE_OPA] = I | L,
    [T5UI_P_LINE_ROUNDED] = I | L, [T5UI_P_ARC_WIDTH] = I | L, [T5UI_P_ARC_COLOR] = C | L,
    [T5UI_P_ARC_OPA] = I | L, [T5UI_P_ARC_ROUNDED] = I | L,
    [T5UI_P_IMAGE_RECOLOR] = C | L, [T5UI_P_IMAGE_RECOLOR_OPA] = I | L, [T5UI_P_IMAGE_SCALE] = I,
    [T5UI_P_IMAGE_ROTATION] = I,
    [T5UI_P_HIDDEN] = I, [T5UI_P_CLICKABLE] = I, [T5UI_P_SCROLLABLE] = I, [T5UI_P_SCROLL_DIR] = I,
    [T5UI_P_SCROLLBAR] = I, [T5UI_P_SCROLL_X] = I, [T5UI_P_SCROLL_Y] = I, [T5UI_P_CHECKED] = I,
    [T5UI_P_DISABLED] = I, [T5UI_P_CHECKABLE] = I, [T5UI_P_BUBBLE] = I,
    [T5UI_P_VALUE] = I, [T5UI_P_MIN] = I, [T5UI_P_MAX] = I, [T5UI_P_START_VALUE] = I, [T5UI_P_ARC_START] = I,
    [T5UI_P_ARC_END] = I, [T5UI_P_ARC_ROTATION] = I, [T5UI_P_SELECTED] = I, [T5UI_P_OPTIONS] = S,
    [T5UI_P_COUNT] = I, [T5UI_P_ROWS] = I, [T5UI_P_COLUMNS] = I, [T5UI_P_ONE_LINE] = I, [T5UI_P_PASSWORD] = I,
    [T5UI_P_MAX_LENGTH] = I, [T5UI_P_TARGET] = OB, [T5UI_P_KEYBOARD_MODE] = I, [T5UI_P_CHART_TYPE] = I,
    [T5UI_P_CHART_GRID_ROWS] = I, [T5UI_P_CHART_GRID_COLUMNS] = I, [T5UI_P_ROW_DP] = I, [T5UI_P_ANIM_TIME] = I,
};

uint8_t t5ui_prop_info(t5ui_prop_t p)
{
    return (unsigned)p < sizeof PROPS ? PROPS[p] : 0;
}

static bool kind_known(t5ui_kind_t k)
{
    return (k >= T5UI_BOX && k <= T5UI_KEYBOARD) || (k >= T5UI_MD_TEXT && k <= T5UI_MD_LIST);
}

static bool at_ok(uint32_t at)
{
    return T5UI_AT_PART(at) <= T5UI_PART_LAST && !(T5UI_AT_STATES(at) & ~T5UI_STATE_ALL);
}

t5ui_obj_t *t5ui_create(t5ui_obj_t *parent, t5ui_kind_t kind)
{
    if (!parent || !kind_known(kind)) return NULL;
    return t5ui_be_create(parent, kind);
}

static t5_err_t set(t5ui_obj_t *obj, t5ui_prop_t p, uint32_t at, const t5ui_val_t *v)
{
    if (!obj) return T5_ERR_INVALID_ARG;
    uint8_t info = t5ui_prop_info(p);
    if ((info & T5UI_T_MASK) != v->type) return T5_ERR_NOT_SUPPORTED;
    if (at && !(info & T5UI_F_LOOK)) return T5_ERR_NOT_SUPPORTED;
    if (!at_ok(at)) return T5_ERR_INVALID_ARG;
    return t5ui_be_set(obj, p, at, v);
}

t5_err_t t5ui_set_int(t5ui_obj_t *obj, t5ui_prop_t p, int32_t v)
{
    return set(obj, p, 0, &(t5ui_val_t){ .type = T5UI_T_INT, .i = v });
}

t5_err_t t5ui_set_color(t5ui_obj_t *obj, t5ui_prop_t p, t5ui_color_t c)
{
    return set(obj, p, 0, &(t5ui_val_t){ .type = T5UI_T_COLOR, .c = c & 0xffffff });
}

t5_err_t t5ui_set_str(t5ui_obj_t *obj, t5ui_prop_t p, const char *s)
{
    return set(obj, p, 0, &(t5ui_val_t){ .type = T5UI_T_STR, .s = s ? s : "" });
}

t5_err_t t5ui_set_strf(t5ui_obj_t *obj, t5ui_prop_t p, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt ? fmt : "", ap);
    va_end(ap);
    return t5ui_set_str(obj, p, buf);
}

t5_err_t t5ui_set_obj(t5ui_obj_t *obj, t5ui_prop_t p, t5ui_obj_t *v)
{
    return set(obj, p, 0, &(t5ui_val_t){ .type = T5UI_T_OBJ, .o = v });
}

t5_err_t t5ui_set_int_at(t5ui_obj_t *obj, t5ui_prop_t p, uint32_t at, int32_t v)
{
    if (!at) return t5ui_set_int(obj, p, v);
    return set(obj, p, at, &(t5ui_val_t){ .type = T5UI_T_INT, .i = v });
}

t5_err_t t5ui_set_color_at(t5ui_obj_t *obj, t5ui_prop_t p, uint32_t at, t5ui_color_t c)
{
    if (!at) return t5ui_set_color(obj, p, c);
    return set(obj, p, at, &(t5ui_val_t){ .type = T5UI_T_COLOR, .c = c & 0xffffff });
}

static bool get(t5ui_obj_t *obj, t5ui_prop_t p, t5ui_val_t *v)
{
    uint8_t info = t5ui_prop_info(p);
    if (!obj || (info & T5UI_T_MASK) != v->type || (info & T5UI_F_NOREAD)) return false;
    return t5ui_be_get(obj, p, v);
}

int32_t t5ui_get_int(t5ui_obj_t *obj, t5ui_prop_t p, int32_t def)
{
    t5ui_val_t v = { .type = T5UI_T_INT };
    return get(obj, p, &v) ? v.i : def;
}

t5ui_color_t t5ui_get_color(t5ui_obj_t *obj, t5ui_prop_t p, t5ui_color_t def)
{
    t5ui_val_t v = { .type = T5UI_T_COLOR };
    return get(obj, p, &v) ? (v.c & 0xffffff) : def;
}

const char *t5ui_get_str(t5ui_obj_t *obj, t5ui_prop_t p, const char *def)
{
    t5ui_val_t v = { .type = T5UI_T_STR };
    return get(obj, p, &v) && v.s ? v.s : def;
}

t5ui_obj_t *t5ui_get_obj(t5ui_obj_t *obj, t5ui_prop_t p)
{
    t5ui_val_t v = { .type = T5UI_T_OBJ };
    return get(obj, p, &v) ? v.o : NULL;
}

static t5_err_t style_set(t5ui_style_t *s, t5ui_prop_t p, const t5ui_val_t *v)
{
    if (!s) return T5_ERR_INVALID_ARG;
    uint8_t info = t5ui_prop_info(p);
    if (!(info & T5UI_F_LOOK) || (info & T5UI_T_MASK) != v->type) return T5_ERR_NOT_SUPPORTED;
    return t5ui_be_style_set(s, p, v);
}

t5_err_t t5ui_style_set_int(t5ui_style_t *s, t5ui_prop_t p, int32_t v)
{
    return style_set(s, p, &(t5ui_val_t){ .type = T5UI_T_INT, .i = v });
}

t5_err_t t5ui_style_set_color(t5ui_style_t *s, t5ui_prop_t p, t5ui_color_t c)
{
    return style_set(s, p, &(t5ui_val_t){ .type = T5UI_T_COLOR, .c = c & 0xffffff });
}

t5_err_t t5ui_style_add(t5ui_obj_t *obj, t5ui_style_t *s, uint32_t at)
{
    if (!obj || !s || !at_ok(at)) return T5_ERR_INVALID_ARG;
    return t5ui_be_style_add(obj, s, at);
}

t5_err_t t5ui_animate(t5ui_obj_t *obj, t5ui_prop_t p, int32_t from, int32_t to, uint32_t ms, uint32_t delay_ms,
                      t5ui_ease_t ease)
{
    if (!obj || (unsigned)ease > T5UI_EASE_BOUNCE) return T5_ERR_INVALID_ARG;
    if ((t5ui_prop_info(p) & T5UI_T_MASK) != T5UI_T_INT) return T5_ERR_NOT_SUPPORTED;
    return t5ui_be_animate(obj, p, from, to, ms, delay_ms, ease);
}

t5_err_t t5ui_on(t5ui_obj_t *obj, t5ui_ev_t ev, t5ui_event_cb_t cb, void *user)
{
    if (!obj || !cb) return T5_ERR_INVALID_ARG;
    if (ev < T5UI_EV_CLICK || ev > T5UI_EV_CANCEL) return T5_ERR_NOT_SUPPORTED;
    return t5ui_be_on(obj, ev, cb, user);
}

int32_t t5ui_cmd(t5ui_obj_t *obj, t5ui_cmd_t cmd, int32_t a, int32_t b, int32_t c, const char *text)
{
    if (!obj) return T5_ERR_INVALID_ARG;
    if (cmd < T5UI_CMD_CHART_ADD_SERIES || cmd > T5UI_CMD_FOCUS) return T5_ERR_NOT_SUPPORTED;
    return t5ui_be_cmd(obj, cmd, a, b, c, text);
}

t5_err_t t5ui_image_pixels(t5ui_obj_t *img, const t5ui_pixels_t *px)
{
    if (!img || !px || px->size < sizeof *px || !px->w || !px->h || !px->data || px->format > T5UI_PX_A8) {
        return T5_ERR_INVALID_ARG;
    }
    return t5ui_be_image_pixels(img, px);
}
