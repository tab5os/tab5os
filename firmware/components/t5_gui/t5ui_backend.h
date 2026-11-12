/* SPDX-License-Identifier: Apache-2.0
 * Between t5ui's core (t5ui_core.c) and the backend that draws it.
 *
 * The core implements the property, style, animation, event and command
 * entry points of t5_gui/t5ui.h: it checks arguments and each property's
 * type against the table below, so every backend sees only well-formed
 * calls. A backend implements the t5ui_be_* calls here and every other
 * function in t5ui.h. Today that is t5ui_lvgl9.c (LVGL 9 and md); moving
 * the OS to another UI library means writing another backend, and the
 * public header and apps stay as they are.
 */
#pragma once
#include "t5_gui/t5ui.h"

/* A property's value type, and what can be done with it. */
enum {
    T5UI_T_INT = 1,
    T5UI_T_COLOR = 2,
    T5UI_T_STR = 3,
    T5UI_T_OBJ = 4,
    T5UI_T_MASK = 0x0f,
    T5UI_F_LOOK = 0x10,        /* settable for a part and states, and in styles */
    T5UI_F_NOREAD = 0x20,      /* set only */
};
/* 0 for a number that isn't a property. */
uint8_t t5ui_prop_info(t5ui_prop_t p);

typedef struct {
    uint8_t type;              /* T5UI_T_* */
    union {
        int32_t i;
        t5ui_color_t c;
        const char *s;         /* never NULL when set */
        t5ui_obj_t *o;
    };
} t5ui_val_t;

/* The part (T5UI_PART_*) and states in an at selector. */
#define T5UI_AT_PART(at) ((at) >> 16)
#define T5UI_AT_STATES(at) ((at) & 0xffff)
#define T5UI_STATE_ALL 0x1f
#define T5UI_PART_LAST T5UI_PART_CURSOR

/* ---- the backend: arguments already checked ------------------------------------------ */
/* kind is a known kind; parent isn't NULL. */
t5ui_obj_t *t5ui_be_create(t5ui_obj_t *parent, t5ui_kind_t kind);
/* v's type matches p; at is 0 unless p is a look property. */
t5_err_t t5ui_be_set(t5ui_obj_t *obj, t5ui_prop_t p, uint32_t at, const t5ui_val_t *v);
/* v->type says the type wanted; false: obj has no such property. */
bool t5ui_be_get(t5ui_obj_t *obj, t5ui_prop_t p, t5ui_val_t *v);
/* p is a look property. */
t5_err_t t5ui_be_style_set(t5ui_style_t *s, t5ui_prop_t p, const t5ui_val_t *v);
t5_err_t t5ui_be_style_add(t5ui_obj_t *obj, t5ui_style_t *s, uint32_t at);
/* p is an int property that can be set. */
t5_err_t t5ui_be_animate(t5ui_obj_t *obj, t5ui_prop_t p, int32_t from, int32_t to, uint32_t ms, uint32_t delay_ms,
                         t5ui_ease_t ease);
/* ev is a known event; cb isn't NULL. */
t5_err_t t5ui_be_on(t5ui_obj_t *obj, t5ui_ev_t ev, t5ui_event_cb_t cb, void *user);
/* cmd is a known command. */
int32_t t5ui_be_cmd(t5ui_obj_t *obj, t5ui_cmd_t cmd, int32_t a, int32_t b, int32_t c, const char *text);
/* px is complete: a known format, a size, data. */
t5_err_t t5ui_be_image_pixels(t5ui_obj_t *img, const t5ui_pixels_t *px);
