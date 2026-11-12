/* SPDX-License-Identifier: Apache-2.0
 * t5ui: the app UI API (API level 2; in the SDK through t5/t5ui.h).
 *
 * A UI API that belongs to Tab5 OS, not to the library that draws it.
 * Today a backend on LVGL 9 and Tab5's Material widgets draws it. A later
 * OS can put another backend behind the same API (a newer LVGL, or
 * something else), and apps built against t5ui keep working unchanged.
 * Nothing of LVGL reaches an app through this header.
 *
 * The model is small on purpose, so it can stay stable:
 *   - objects: t5ui_create(parent, kind) makes a tree; children go with
 *     their parent
 *   - properties: t5ui_set_int / _color / _str / _obj (obj, T5UI_P_*, value)
 *     and the getters; look properties also for a part and states
 *     (t5ui_set_int_at: a slider's knob, a pressed button)
 *   - events: t5ui_on(obj, T5UI_EV_*, cb, user)
 *   - commands for what isn't a property: t5ui_cmd(obj, T5UI_CMD_*, ...)
 *   - reusable styles, property animations, timers, canvases and drawing
 * New abilities come as new kinds, properties, events and commands (new
 * numbers), never as changes to these functions. The numbers never change
 * meaning. Something a kind doesn't have returns T5_ERR_NOT_SUPPORTED.
 *
 * Threads: call t5ui from the UI task (the app's on_create, events, timers)
 * or hand work to it with t5ui_async. Objects, styles, timers and
 * animations belong to the app and go when it exits.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- values ------------------------------------------------------------------------ */
typedef struct t5ui_obj t5ui_obj_t;
typedef struct t5ui_event t5ui_event_t;        /* valid during its callback only */
typedef struct t5ui_timer t5ui_timer_t;
typedef struct t5ui_style t5ui_style_t;
typedef struct t5ui_paint t5ui_paint_t;        /* a drawing target, valid while drawing */
typedef uint32_t t5ui_color_t;                 /* 0xRRGGBB */
typedef uint8_t t5ui_opa_t;                    /* 0 clear .. 255 opaque */

#define T5UI_RGB(r, g, b) ((t5ui_color_t)(((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b)))
#define T5UI_OPA_COVER 255
#define T5UI_OPA_TRANSP 0
/* Sizes and positions are pixels, or one of these. */
#define T5UI_PCT(n) ((int32_t)(0x40000000 | ((n) & 0xffff)))   /* percent of the parent's content */
#define T5UI_CONTENT ((int32_t)0x7ffffffe)                     /* fit the content */
#define T5UI_RADIUS_CIRCLE 0x7fff

typedef struct {
    int32_t x1, y1, x2, y2;                    /* inclusive */
} t5ui_rect_t;

/* ---- kinds of object ---------------------------------------------------------------- */
typedef enum {
    T5UI_BOX = 1,                              /* a container with no look of its own */
    T5UI_PANEL = 2,                            /* a surface: background, rounded, padded */
    T5UI_LABEL = 3,
    T5UI_BUTTON = 4,                           /* a plain button; T5UI_P_TEXT puts a label in it */
    T5UI_IMAGE = 5,                            /* shows t5ui_image_pixels */
    T5UI_CANVAS = 6,                           /* keeps what's drawn on it: t5ui_canvas_* */
    T5UI_SWITCH = 7,
    T5UI_CHECKBOX = 8,
    T5UI_SLIDER = 9,
    T5UI_ARC = 10,
    T5UI_BAR = 11,
    T5UI_SPINNER = 12,
    T5UI_LED = 13,
    T5UI_TEXTAREA = 14,
    T5UI_DROPDOWN = 15,
    T5UI_ROLLER = 16,
    T5UI_CHART = 17,
    T5UI_TABLE = 18,
    T5UI_KEYBOARD = 19,
    /* Material, the Tab5 look. Choices fixed at creation are separate kinds. */
    T5UI_MD_TEXT = 100,                        /* text in a Material type style (T5UI_P_TEXT_STYLE) */
    T5UI_MD_ICON = 101,                        /* T5UI_P_ICON, T5UI_P_ICON_SIZE */
    T5UI_MD_BUTTON_TEXT = 102,
    T5UI_MD_BUTTON_OUTLINED = 103,
    T5UI_MD_BUTTON_CONTAINED = 104,
    T5UI_MD_FAB = 105,                         /* T5UI_P_ICON */
    T5UI_MD_FAB_MINI = 106,
    T5UI_MD_CARD = 107,
    T5UI_MD_DIVIDER = 108,
    T5UI_MD_APPBAR = 109,                      /* T5UI_P_TITLE */
    T5UI_MD_APPBAR_BACK = 110,                 /* with a back arrow: T5UI_EV_BACK */
    T5UI_MD_APPBAR_ACTION = 111,               /* an icon button, made in an app bar or page */
    T5UI_MD_ROW = 112,                         /* T5UI_P_ICON, T5UI_P_TEXT, T5UI_P_SECONDARY */
    T5UI_MD_ROW_SWITCH = 113,                  /* a switch at a row's end, made in the row */
    T5UI_MD_SECTION = 114,                     /* a section header: T5UI_P_TEXT */
    T5UI_MD_SWITCH = 115,
    T5UI_MD_CHECKBOX = 116,
    T5UI_MD_SLIDER = 117,
    T5UI_MD_TEXTFIELD = 118,                   /* T5UI_P_PLACEHOLDER is its label */
    T5UI_MD_PASSWORD = 119,
    T5UI_MD_CHIP_ACTION = 120,
    T5UI_MD_CHIP_FILTER = 121,                 /* toggles T5UI_P_CHECKED */
    T5UI_MD_CHIP_CHOICE = 122,                 /* one checked among its siblings */
    T5UI_MD_TABS = 123,                        /* T5UI_P_SELECTED; make T5UI_MD_TAB in it */
    T5UI_MD_TAB = 124,                         /* T5UI_P_TEXT; the tab's content goes in it */
    T5UI_MD_PAGE = 125,                        /* full screen, back arrow, T5UI_P_TITLE; content goes in it */
    T5UI_MD_LIST = 126,                        /* a long list: T5UI_P_COUNT rows, T5UI_EV_BIND fills them */
    T5UI_MD_ROW_TRAILING = 127,                /* text at a row's end (T5UI_P_TEXT), made in the row */
} t5ui_kind_t;

/* ---- properties --------------------------------------------------------------------- */
/* Each has one type: int (also bools and enums), color, str or obj. Look
 * properties (marked L) can also be set for a part and states, and in a
 * style. */
typedef enum {
    /* geometry, int: pixels, T5UI_PCT, T5UI_CONTENT (L) */
    T5UI_P_X = 1, T5UI_P_Y = 2, T5UI_P_WIDTH = 3, T5UI_P_HEIGHT = 4,
    T5UI_P_MIN_WIDTH = 5, T5UI_P_MAX_WIDTH = 6, T5UI_P_MIN_HEIGHT = 7, T5UI_P_MAX_HEIGHT = 8,
    T5UI_P_ALIGN = 9,                          /* t5ui_align_t; X and Y then offset from it */
    /* layout of the children (L) */
    T5UI_P_FLOW = 20,                          /* t5ui_flow_t */
    T5UI_P_PLACE_MAIN = 21, T5UI_P_PLACE_CROSS = 22, T5UI_P_PLACE_TRACK = 23,   /* t5ui_place_t */
    T5UI_P_GROW = 24,                          /* share of the free space in the parent's flow */
    T5UI_P_FLOATING = 25,                      /* bool: placed by X, Y and ALIGN, outside the flow */
    /* spacing, int pixels (L) */
    T5UI_P_PAD_TOP = 40, T5UI_P_PAD_BOTTOM = 41, T5UI_P_PAD_LEFT = 42, T5UI_P_PAD_RIGHT = 43,
    T5UI_P_PAD_ALL = 44,                       /* sets the four; can't be read */
    T5UI_P_PAD_ROW = 45, T5UI_P_PAD_COLUMN = 46,   /* gaps between children */
    T5UI_P_MARGIN_TOP = 47, T5UI_P_MARGIN_BOTTOM = 48, T5UI_P_MARGIN_LEFT = 49, T5UI_P_MARGIN_RIGHT = 50,
    /* look (L) */
    T5UI_P_BG_COLOR = 60, T5UI_P_BG_OPA = 61, T5UI_P_BG_GRAD_COLOR = 62,
    T5UI_P_BG_GRAD_DIR = 63,                   /* t5ui_grad_t */
    T5UI_P_BORDER_WIDTH = 64, T5UI_P_BORDER_COLOR = 65, T5UI_P_BORDER_OPA = 66,
    T5UI_P_BORDER_SIDES = 67,                  /* T5UI_SIDE_* bits */
    T5UI_P_OUTLINE_WIDTH = 68, T5UI_P_OUTLINE_COLOR = 69, T5UI_P_OUTLINE_OPA = 70, T5UI_P_OUTLINE_PAD = 71,
    T5UI_P_SHADOW_WIDTH = 72, T5UI_P_SHADOW_COLOR = 73, T5UI_P_SHADOW_OPA = 74,
    T5UI_P_SHADOW_OFFSET_X = 75, T5UI_P_SHADOW_OFFSET_Y = 76, T5UI_P_SHADOW_SPREAD = 77,
    T5UI_P_RADIUS = 78,                        /* T5UI_RADIUS_CIRCLE: round */
    T5UI_P_CLIP_CORNER = 79,                   /* bool: children cut to the rounded corners */
    T5UI_P_OPA = 80,                           /* the object with its children */
    T5UI_P_ELEVATION = 81,                     /* Material elevation, dp: shadow and dark-theme tint; not L */
    T5UI_P_BG_MAIN_STOP = 82, T5UI_P_BG_GRAD_STOP = 83,   /* where the gradient starts and ends, 0..255 */
    /* transform: drawn moved, not laid out (L) */
    T5UI_P_TRANSLATE_X = 90, T5UI_P_TRANSLATE_Y = 91,
    T5UI_P_SCALE = 92,                         /* 256 = 1x */
    T5UI_P_ROTATION = 93,                      /* 0.1 degree */
    /* text */
    T5UI_P_TEXT = 100,                         /* str: a label's, button's, row's, tab's, textarea's... */
    T5UI_P_TEXT_COLOR = 101, T5UI_P_TEXT_OPA = 102,   /* L */
    T5UI_P_TEXT_STYLE = 103,                   /* t5ui_text_t: font, size and weight (L) */
    T5UI_P_TEXT_ALIGN = 104,                   /* t5ui_text_align_t (L) */
    T5UI_P_LETTER_SPACE = 105, T5UI_P_LINE_SPACE = 106,   /* L */
    T5UI_P_TEXT_DECOR = 107,                   /* T5UI_DECOR_* bits (L) */
    T5UI_P_LONG_MODE = 108,                    /* t5ui_long_t: text too long for a label's width */
    T5UI_P_SECONDARY = 109,                    /* str: a row's second line */
    T5UI_P_ICON = 110,                         /* str, a T5UI_ICON_*: of an icon, row, FAB, chip, action */
    T5UI_P_ICON_SIZE = 111,                    /* dp */
    T5UI_P_PLACEHOLDER = 112,                  /* str: a text field's label, a textarea's hint */
    T5UI_P_HELPER = 113,                       /* str: a text field's helper line */
    T5UI_P_HELPER_ERROR = 114,                 /* bool: the helper line is an error */
    T5UI_P_TITLE = 115,                        /* str: an app bar's or page's */
    /* lines and arcs (L) */
    T5UI_P_LINE_WIDTH = 130, T5UI_P_LINE_COLOR = 131, T5UI_P_LINE_OPA = 132, T5UI_P_LINE_ROUNDED = 133,
    T5UI_P_ARC_WIDTH = 134, T5UI_P_ARC_COLOR = 135, T5UI_P_ARC_OPA = 136, T5UI_P_ARC_ROUNDED = 137,
    /* images */
    T5UI_P_IMAGE_RECOLOR = 150, T5UI_P_IMAGE_RECOLOR_OPA = 151,   /* L */
    T5UI_P_IMAGE_SCALE = 152,                  /* 256 = 1x */
    T5UI_P_IMAGE_ROTATION = 153,               /* 0.1 degree */
    /* flags and state, int (bool unless noted) */
    T5UI_P_HIDDEN = 170, T5UI_P_CLICKABLE = 171, T5UI_P_SCROLLABLE = 172,
    T5UI_P_SCROLL_DIR = 173,                   /* t5ui_dir_t */
    T5UI_P_SCROLLBAR = 174,                    /* t5ui_scrollbar_t */
    T5UI_P_SCROLL_X = 175, T5UI_P_SCROLL_Y = 176,   /* scrolled by, px; setting scrolls at once */
    T5UI_P_CHECKED = 177, T5UI_P_DISABLED = 178,
    T5UI_P_CHECKABLE = 179,                    /* a click toggles CHECKED */
    T5UI_P_BUBBLE = 180,                       /* events go on to the parent too */
    T5UI_P_SCROLL_SNAP_X = 181, T5UI_P_SCROLL_SNAP_Y = 182,   /* t5ui_snap_t: where children stop */
    T5UI_P_SCROLL_ONE = 183,                   /* a flick scrolls one snapping child at most */
    T5UI_P_SCROLL_ELASTIC = 184,               /* scrolls past the ends and springs back */
    T5UI_P_SCROLL_CHAIN = 185,                 /* at an end, the parent scrolls on */
    T5UI_P_SNAPPABLE = 186,                    /* a scroll can stop on this child */
    T5UI_P_EXT_CLICK = 187,                    /* px: taps this far outside still hit it */
    T5UI_P_STATE = 188,                        /* T5UI_STATE_* bits, all at once */
    T5UI_P_RIPPLE = 189,                       /* Material ink on press; set only */
    /* values, int unless noted */
    T5UI_P_VALUE = 200,                        /* slider, arc, bar; LED brightness 0..255 */
    T5UI_P_MIN = 201, T5UI_P_MAX = 202,        /* slider, arc, bar; a chart's Y range */
    T5UI_P_START_VALUE = 203,                  /* a bar or slider showing a range */
    T5UI_P_ARC_START = 204, T5UI_P_ARC_END = 205,   /* an arc's track, degrees */
    T5UI_P_ARC_ROTATION = 206,                 /* degrees */
    T5UI_P_SELECTED = 207,                     /* dropdown, roller, tabs: index */
    T5UI_P_OPTIONS = 208,                      /* str: dropdown, roller; '\n' between choices */
    T5UI_P_COUNT = 209,                        /* MD list rows, chart points */
    T5UI_P_ROWS = 210, T5UI_P_COLUMNS = 211,   /* table */
    T5UI_P_ONE_LINE = 212, T5UI_P_PASSWORD = 213, T5UI_P_MAX_LENGTH = 214,   /* textarea, text field */
    T5UI_P_TARGET = 215,                       /* obj: the textarea or text field a keyboard types into */
    T5UI_P_KEYBOARD_MODE = 216,                /* t5ui_kb_mode_t */
    T5UI_P_CHART_TYPE = 217,                   /* t5ui_chart_type_t */
    T5UI_P_CHART_GRID_ROWS = 218, T5UI_P_CHART_GRID_COLUMNS = 219,
    T5UI_P_ROW_DP = 220,                       /* MD list row height: 48, 64 (two lines) or 72 */
    T5UI_P_ANIM_TIME = 221,                    /* ms: a spinner's turn */
    T5UI_P_CURSOR = 222,                       /* textarea, text field: cursor position, characters */
    T5UI_P_CONTENT = 223,                      /* obj, read only: where children made in obj go (a page's body) */
} t5ui_prop_t;

/* Where a look property applies: a part of the object, in states. */
typedef enum {
    T5UI_PART_MAIN = 0, T5UI_PART_SCROLLBAR = 1, T5UI_PART_INDICATOR = 2, T5UI_PART_KNOB = 3,
    T5UI_PART_SELECTED = 4, T5UI_PART_ITEMS = 5, T5UI_PART_CURSOR = 6,
} t5ui_part_t;
#define T5UI_STATE_DEFAULT 0x00
#define T5UI_STATE_CHECKED 0x01
#define T5UI_STATE_FOCUSED 0x02
#define T5UI_STATE_PRESSED 0x04
#define T5UI_STATE_DISABLED 0x08
#define T5UI_STATE_EDITED 0x10
#define T5UI_AT(part, states) (((uint32_t)(part) << 16) | (uint32_t)(states))

typedef enum {
    T5UI_ALIGN_DEFAULT = 0, T5UI_ALIGN_TOP_LEFT = 1, T5UI_ALIGN_TOP_MID = 2, T5UI_ALIGN_TOP_RIGHT = 3,
    T5UI_ALIGN_LEFT_MID = 4, T5UI_ALIGN_CENTER = 5, T5UI_ALIGN_RIGHT_MID = 6,
    T5UI_ALIGN_BOTTOM_LEFT = 7, T5UI_ALIGN_BOTTOM_MID = 8, T5UI_ALIGN_BOTTOM_RIGHT = 9,
} t5ui_align_t;
typedef enum {
    T5UI_FLOW_NONE = 0, T5UI_ROW = 1, T5UI_COLUMN = 2, T5UI_ROW_WRAP = 3, T5UI_COLUMN_WRAP = 4,
    T5UI_ROW_REVERSE = 5, T5UI_COLUMN_REVERSE = 6,
} t5ui_flow_t;
typedef enum { T5UI_SNAP_NONE = 0, T5UI_SNAP_START = 1, T5UI_SNAP_END = 2, T5UI_SNAP_CENTER = 3 } t5ui_snap_t;
typedef enum {
    T5UI_PLACE_START = 0, T5UI_PLACE_END = 1, T5UI_PLACE_CENTER = 2,
    T5UI_PLACE_SPACE_EVENLY = 3, T5UI_PLACE_SPACE_AROUND = 4, T5UI_PLACE_SPACE_BETWEEN = 5,
} t5ui_place_t;
typedef enum { T5UI_GRAD_NONE = 0, T5UI_GRAD_VER = 1, T5UI_GRAD_HOR = 2 } t5ui_grad_t;
#define T5UI_SIDE_TOP 0x1
#define T5UI_SIDE_BOTTOM 0x2
#define T5UI_SIDE_LEFT 0x4
#define T5UI_SIDE_RIGHT 0x8
#define T5UI_SIDE_ALL 0xf
typedef enum { T5UI_TEXT_LEFT = 0, T5UI_TEXT_CENTER = 1, T5UI_TEXT_RIGHT = 2 } t5ui_text_align_t;
#define T5UI_DECOR_UNDERLINE 0x1
#define T5UI_DECOR_STRIKE 0x2
typedef enum { T5UI_LONG_WRAP = 0, T5UI_LONG_DOTS = 1, T5UI_LONG_SCROLL = 2, T5UI_LONG_CLIP = 3 } t5ui_long_t;
typedef enum { T5UI_DIR_NONE = 0, T5UI_DIR_HOR = 1, T5UI_DIR_VER = 2, T5UI_DIR_ALL = 3 } t5ui_dir_t;
typedef enum {
    T5UI_SCROLLBAR_OFF = 0, T5UI_SCROLLBAR_ON = 1, T5UI_SCROLLBAR_ACTIVE = 2, T5UI_SCROLLBAR_AUTO = 3,
} t5ui_scrollbar_t;
typedef enum { T5UI_KB_TEXT = 0, T5UI_KB_NUMBER = 1 } t5ui_kb_mode_t;
typedef enum { T5UI_CHART_LINE = 0, T5UI_CHART_BAR = 1 } t5ui_chart_type_t;
/* Text styles: the theme's font, size, weight and spacing for each role. */
typedef enum {
    T5UI_TEXT_HEADLINE4 = 0, T5UI_TEXT_HEADLINE5 = 1, T5UI_TEXT_HEADLINE6 = 2, T5UI_TEXT_SUBTITLE1 = 3,
    T5UI_TEXT_BODY1 = 4, T5UI_TEXT_BODY2 = 5, T5UI_TEXT_BUTTON = 6, T5UI_TEXT_CAPTION = 7,
    T5UI_TEXT_OVERLINE = 8, T5UI_TEXT_MONO = 9,
    T5UI_TEXT_CLOCK = 10,                      /* 100 dp digits for a clock face: 0-9 and ':' only */
} t5ui_text_t;

/* ---- objects ------------------------------------------------------------------------ */
struct t5_app;
/* The app's screen, from on_create on: the root of its UI. */
t5ui_obj_t *t5ui_root(struct t5_app *app);
/* NULL if parent can't hold kind (a T5UI_MD_TAB outside tabs) or memory ran out. */
t5ui_obj_t *t5ui_create(t5ui_obj_t *parent, t5ui_kind_t kind);
t5ui_kind_t t5ui_kind(t5ui_obj_t *obj);        /* 0 for an object t5ui didn't make */
void t5ui_delete(t5ui_obj_t *obj);
void t5ui_clean(t5ui_obj_t *obj);              /* delete its children */
t5ui_obj_t *t5ui_parent(t5ui_obj_t *obj);
t5ui_obj_t *t5ui_child(t5ui_obj_t *obj, int32_t index);   /* negative counts from the end */
uint32_t t5ui_child_count(t5ui_obj_t *obj);
void t5ui_move_to(t5ui_obj_t *obj, int32_t index);       /* among its siblings */
void t5ui_set_user(t5ui_obj_t *obj, void *user);
void *t5ui_user(t5ui_obj_t *obj);
/* Where obj is on screen, and its content area inside the padding. */
void t5ui_coords(t5ui_obj_t *obj, t5ui_rect_t *out);
void t5ui_content_coords(t5ui_obj_t *obj, t5ui_rect_t *out);
void t5ui_layout_now(t5ui_obj_t *obj);         /* positions settle before the next frame otherwise */
void t5ui_invalidate(t5ui_obj_t *obj);         /* redraw it on the next frame */
void t5ui_scroll_into_view(t5ui_obj_t *obj, bool animate);

/* ---- properties ----------------------------------------------------------------------- */
/* T5_ERR_NOT_SUPPORTED: obj's kind has no such property, or it's another type. */
t5_err_t t5ui_set_int(t5ui_obj_t *obj, t5ui_prop_t p, int32_t v);
t5_err_t t5ui_set_color(t5ui_obj_t *obj, t5ui_prop_t p, t5ui_color_t c);
t5_err_t t5ui_set_str(t5ui_obj_t *obj, t5ui_prop_t p, const char *s);   /* copied */
t5_err_t t5ui_set_strf(t5ui_obj_t *obj, t5ui_prop_t p, const char *fmt, ...);   /* printf, to 255 bytes */
t5_err_t t5ui_set_obj(t5ui_obj_t *obj, t5ui_prop_t p, t5ui_obj_t *v);
/* A look property for a part and states, e.g. T5UI_AT(T5UI_PART_KNOB, T5UI_STATE_PRESSED). */
t5_err_t t5ui_set_int_at(t5ui_obj_t *obj, t5ui_prop_t p, uint32_t at, int32_t v);
t5_err_t t5ui_set_color_at(t5ui_obj_t *obj, t5ui_prop_t p, uint32_t at, t5ui_color_t c);
/* The value now, or def when obj has no such property. */
int32_t t5ui_get_int(t5ui_obj_t *obj, t5ui_prop_t p, int32_t def);
t5ui_color_t t5ui_get_color(t5ui_obj_t *obj, t5ui_prop_t p, t5ui_color_t def);
const char *t5ui_get_str(t5ui_obj_t *obj, t5ui_prop_t p, const char *def);   /* until it changes */
t5ui_obj_t *t5ui_get_obj(t5ui_obj_t *obj, t5ui_prop_t p);

/* Styles: shared sets of look properties. Changing one shows on every
 * object that has it. */
t5ui_style_t *t5ui_style_create(void);
t5_err_t t5ui_style_set_int(t5ui_style_t *s, t5ui_prop_t p, int32_t v);
t5_err_t t5ui_style_set_color(t5ui_style_t *s, t5ui_prop_t p, t5ui_color_t c);
t5_err_t t5ui_style_add(t5ui_obj_t *obj, t5ui_style_t *s, uint32_t at);   /* at: T5UI_AT(...) */
void t5ui_style_remove(t5ui_obj_t *obj, t5ui_style_t *s);
void t5ui_style_clear(t5ui_obj_t *obj);        /* every style, the theme's too: a bare object */
void t5ui_style_delete(t5ui_style_t *s);       /* also takes it off the app's objects */

/* Animate an int property from..to over ms, after delay_ms. Starting
 * another on the same property replaces it. T5UI_EV_ANIM_END follows a
 * finished one (not a stopped or replaced one). */
typedef enum {
    T5UI_EASE_LINEAR = 0, T5UI_EASE_IN = 1, T5UI_EASE_OUT = 2, T5UI_EASE_IN_OUT = 3, T5UI_EASE_OVERSHOOT = 4,
    T5UI_EASE_BOUNCE = 5,
    T5UI_EASE_STANDARD = 6,                    /* Material: moving on screen */
    T5UI_EASE_DECELERATE = 7,                  /* Material: entering */
    T5UI_EASE_ACCELERATE = 8,                  /* Material: leaving */
    T5UI_EASE_SHARP = 9,                       /* Material: leaving for a while */
} t5ui_ease_t;
t5_err_t t5ui_animate(t5ui_obj_t *obj, t5ui_prop_t p, int32_t from, int32_t to, uint32_t ms, uint32_t delay_ms,
                      t5ui_ease_t ease);
void t5ui_animate_stop(t5ui_obj_t *obj, t5ui_prop_t p);   /* 0: every one of obj's */

/* ---- commands: what isn't a property ----------------------------------------------------- */
typedef enum {
    T5UI_CMD_CHART_ADD_SERIES = 1,             /* a = color: the new series' index */
    T5UI_CMD_CHART_PUSH = 2,                   /* a = series, b = value: shifts the others left */
    T5UI_CMD_CHART_SET = 3,                    /* a = series, b = point, c = value */
    T5UI_CMD_CHART_CLEAR = 4,                  /* a = series */
    T5UI_CMD_TABLE_SET_CELL = 5,               /* a = row, b = column, text */
    T5UI_CMD_TEXT_INSERT = 6,                  /* textarea, text field: text at the cursor */
    T5UI_CMD_TEXT_DELETE = 7,                  /* the character before the cursor */
    T5UI_CMD_LIST_REFRESH = 8,                 /* MD list: bind the visible rows again */
    T5UI_CMD_SCROLL_TO = 9,                    /* a = x, b = y, c = animate */
    T5UI_CMD_SCROLL_BY = 10,                   /* a = dx, b = dy, c = animate */
    T5UI_CMD_OPEN = 11, T5UI_CMD_CLOSE = 12,   /* a dropdown's list */
    T5UI_CMD_FOCUS = 13,                       /* the keyboard's focus; a = keys like Tab go to obj too */
    T5UI_CMD_CHART_SET_ALL = 14,               /* a = series, b = value for every point */
    T5UI_CMD_CHART_COLOR = 15,                 /* a = series, b = color */
    T5UI_CMD_IGNORE_PRESS = 16,                /* the press going on now ends without a click */
    T5UI_CMD_DELETE_LATER = 17,                /* delete obj once the current event is over */
    T5UI_CMD_CHART_X_START = 18,               /* a = series, b = the point drawn first */
    T5UI_CMD_SEND = 19,                        /* a = T5UI_EV_*: as if it happened (callbacks, widget) */
} t5ui_cmd_t;
#define T5UI_CHART_NONE INT32_MAX              /* a chart point with no value */
/* The result (>= 0), or a negative t5_err_t. */
int32_t t5ui_cmd(t5ui_obj_t *obj, t5ui_cmd_t cmd, int32_t a, int32_t b, int32_t c, const char *text);

/* ---- events -------------------------------------------------------------------------------- */
typedef enum {
    T5UI_EV_CLICK = 1, T5UI_EV_LONG_PRESS = 2, T5UI_EV_PRESS = 3, T5UI_EV_RELEASE = 4,
    T5UI_EV_VALUE = 5,                         /* the user changed its value, state or text */
    T5UI_EV_FOCUS = 6, T5UI_EV_DEFOCUS = 7, T5UI_EV_SCROLL = 8, T5UI_EV_RESIZE = 9,
    T5UI_EV_DELETE = 10,                       /* obj is going: free what its user pointer holds */
    T5UI_EV_DRAW = 11,                         /* draw over obj with t5ui_event_paint */
    T5UI_EV_BACK = 12,                         /* an app bar's or page's back arrow */
    T5UI_EV_BIND = 13,                         /* MD list: fill row t5ui_event_target for t5ui_event_index */
    T5UI_EV_ITEM = 14,                         /* MD list: row t5ui_event_index tapped */
    T5UI_EV_READY = 15, T5UI_EV_CANCEL = 16,   /* keyboard OK or close; Enter in a one-line textarea */
    T5UI_EV_SHORT_CLICK = 17,                  /* a click that wasn't a long press */
    T5UI_EV_DOUBLE_CLICK = 18,
    T5UI_EV_PRESSING = 19,                     /* again and again while pressed */
    T5UI_EV_PRESS_LOST = 20,                   /* the finger slid off */
    T5UI_EV_SCROLL_END = 21,
    T5UI_EV_KEY = 22,                          /* t5ui_event_key; before the widget: t5ui_event_stop keeps it */
    T5UI_EV_INSERT = 23,                       /* textarea: t5ui_event_text about to go in; t5ui_event_set_text */
    T5UI_EV_GESTURE = 24,                      /* a swipe: t5ui_event_dir */
    T5UI_EV_ANIM_END = 25,                     /* an animation finished: t5ui_event_index is the property */
    T5UI_EV_ITEM_LONG = 26,                    /* MD list: row t5ui_event_index long-pressed */
    T5UI_EV_CHILD_CREATED = 27,                /* t5ui_event_target is the new child */
    T5UI_EV_DRAW_BEHIND = 28,                  /* draw under obj's children: t5ui_event_paint */
} t5ui_ev_t;
/* Keys (T5UI_EV_KEY): these, or a Unicode code point. */
#define T5UI_KEY_HOME 2
#define T5UI_KEY_END 3
#define T5UI_KEY_BACKSPACE 8
#define T5UI_KEY_NEXT 9
#define T5UI_KEY_ENTER 10
#define T5UI_KEY_PREV 11
#define T5UI_KEY_UP 17
#define T5UI_KEY_DOWN 18
#define T5UI_KEY_RIGHT 19
#define T5UI_KEY_LEFT 20
#define T5UI_KEY_ESC 27
#define T5UI_KEY_DEL 127
/* Swipe directions (T5UI_EV_GESTURE). */
#define T5UI_SWIPE_LEFT 0x1
#define T5UI_SWIPE_RIGHT 0x2
#define T5UI_SWIPE_UP 0x4
#define T5UI_SWIPE_DOWN 0x8
typedef void (*t5ui_event_cb_t)(t5ui_event_t *e);
/* cb(e) each time ev happens to obj; an object can have several. */
t5_err_t t5ui_on(t5ui_obj_t *obj, t5ui_ev_t ev, t5ui_event_cb_t cb, void *user);
/* Remove what t5ui_on(obj, ev, cb, user) added (any user if user is NULL). */
void t5ui_off(t5ui_obj_t *obj, t5ui_ev_t ev, t5ui_event_cb_t cb, void *user);
t5ui_obj_t *t5ui_event_obj(t5ui_event_t *e);           /* the object cb was added to */
t5ui_obj_t *t5ui_event_target(t5ui_event_t *e);        /* where it happened: a child, a list row */
void *t5ui_event_user(t5ui_event_t *e);
t5ui_ev_t t5ui_event_kind(t5ui_event_t *e);
int32_t t5ui_event_index(t5ui_event_t *e);             /* T5UI_EV_BIND, T5UI_EV_ITEM; else -1 */
bool t5ui_event_point(t5ui_event_t *e, int32_t *x, int32_t *y);   /* the pointer, screen pixels */
t5ui_paint_t *t5ui_event_paint(t5ui_event_t *e);       /* T5UI_EV_DRAW, T5UI_EV_DRAW_BEHIND */
uint32_t t5ui_event_key(t5ui_event_t *e);              /* T5UI_EV_KEY: T5UI_KEY_* or a code point */
uint32_t t5ui_event_dir(t5ui_event_t *e);              /* T5UI_EV_GESTURE: T5UI_SWIPE_* */
const char *t5ui_event_text(t5ui_event_t *e);          /* T5UI_EV_INSERT */
void t5ui_event_set_text(t5ui_event_t *e, const char *s);   /* T5UI_EV_INSERT: insert s instead ("" nothing) */
/* No later callbacks; for T5UI_EV_KEY, the widget doesn't see the key either. */
void t5ui_event_stop(t5ui_event_t *e);

/* ---- timers, other tasks, input --------------------------------------------------------- */
typedef void (*t5ui_timer_cb_t)(t5ui_timer_t *t, void *user);
t5ui_timer_t *t5ui_timer(uint32_t period_ms, t5ui_timer_cb_t cb, void *user);
void t5ui_timer_period(t5ui_timer_t *t, uint32_t period_ms);
void t5ui_timer_delete(t5ui_timer_t *t);
void t5ui_timer_reset(t5ui_timer_t *t);        /* a full period from now */
/* fn(arg) on the UI task soon; from any task. */
typedef void (*t5ui_fn_t)(void *arg);
t5_err_t t5ui_async(t5ui_fn_t fn, void *arg);
/* On the UI task: fn(arg) once the current event or timer is over. Cancel
 * drops every pending fn(arg). */
t5_err_t t5ui_later(t5ui_fn_t fn, void *arg);
void t5ui_later_cancel(t5ui_fn_t fn, void *arg);
/* Every finger on the screen while the app is in front: how many (0 when
 * it isn't). Screen pixels; id stays the same while the finger is down. */
typedef struct {
    int16_t x, y;
    uint8_t id;
} t5ui_touch_t;
#define T5UI_TOUCH_MAX 5
int t5ui_touches(t5ui_touch_t *out, int max);
uint32_t t5ui_ticks_ms(void);
int32_t t5ui_dp(int32_t dp);                   /* density-independent units to pixels */
void t5ui_screen_size(int32_t *w, int32_t *h);
void t5ui_text_size(const char *text, t5ui_text_t style, int32_t max_w, int32_t *w, int32_t *h);

/* ---- theme ------------------------------------------------------------------------------- */
typedef enum {
    T5UI_COLOR_PRIMARY = 0, T5UI_COLOR_PRIMARY_VARIANT = 1, T5UI_COLOR_SECONDARY = 2, T5UI_COLOR_BACKGROUND = 3,
    T5UI_COLOR_SURFACE = 4, T5UI_COLOR_ERROR = 5, T5UI_COLOR_ON_PRIMARY = 6, T5UI_COLOR_ON_SECONDARY = 7,
    T5UI_COLOR_ON_BACKGROUND = 8, T5UI_COLOR_ON_SURFACE = 9, T5UI_COLOR_ON_ERROR = 10,
} t5ui_color_role_t;
t5ui_color_t t5ui_theme_color(t5ui_color_role_t role);
bool t5ui_theme_dark(void);
typedef enum {
    T5UI_EMPHASIS_HIGH = 0, T5UI_EMPHASIS_MEDIUM = 1, T5UI_EMPHASIS_DISABLED = 2,
    T5UI_EMPHASIS_DIVIDER = 3,                 /* dividers and grid lines */
} t5ui_emphasis_t;
t5ui_opa_t t5ui_theme_emphasis(t5ui_emphasis_t e);       /* opacity of text or lines on a surface */
t5ui_color_t t5ui_mix(t5ui_color_t fg, t5ui_color_t bg, t5ui_opa_t opa);
/* A surface at a Material elevation: lighter with height in the dark theme. */
t5ui_color_t t5ui_theme_surface(int32_t elevation_dp);
/* The accent palette, the one Settings picks the theme's accent from. */
typedef enum {
    T5UI_ACCENT_PURPLE = 0, T5UI_ACCENT_TEAL = 1, T5UI_ACCENT_BLUE = 2, T5UI_ACCENT_INDIGO = 3,
    T5UI_ACCENT_CYAN = 4, T5UI_ACCENT_GREEN = 5, T5UI_ACCENT_AMBER = 6, T5UI_ACCENT_ORANGE = 7,
    T5UI_ACCENT_RED = 8, T5UI_ACCENT_PINK = 9,
} t5ui_accent_t;
#define T5UI_ACCENT_COUNT 10
typedef enum {
    T5UI_TONE_LIGHT = 0, T5UI_TONE_LIGHT_VARIANT = 1,  /* for light surfaces */
    T5UI_TONE_DARK = 2, T5UI_TONE_DARK_VARIANT = 3,    /* for dark surfaces */
    T5UI_TONE_ON_LIGHT = 4, T5UI_TONE_ON_DARK = 5,     /* text on the accent */
} t5ui_tone_t;
t5ui_color_t t5ui_accent_color(t5ui_accent_t a, t5ui_tone_t tone);
const char *t5ui_accent_name(t5ui_accent_t a);
t5ui_accent_t t5ui_theme_accent(void);         /* the theme's accent now */

/* ---- dialogs and messages ---------------------------------------------------------------- */
typedef void (*t5ui_answer_cb_t)(int choice, void *user);  /* 0 first button or item, 1 second..., -1 dismissed */
t5ui_obj_t *t5ui_dialog(const char *title, const char *text, const char *confirm, const char *dismiss,
                        t5ui_answer_cb_t cb, void *user);
void t5ui_dialog_close(t5ui_obj_t *dialog);
t5ui_obj_t *t5ui_menu(t5ui_obj_t *anchor, const char *const *items, int n, t5ui_answer_cb_t cb, void *user);
typedef void (*t5ui_action_cb_t)(void *user);
void t5ui_snackbar(const char *text, const char *action, t5ui_action_cb_t cb, void *user);

/* ---- images and drawing ------------------------------------------------------------------ */
typedef enum { T5UI_PX_RGB565 = 0, T5UI_PX_RGB888 = 1, T5UI_PX_ARGB8888 = 2, T5UI_PX_A8 = 3 } t5ui_px_t;
typedef struct {
    uint32_t size;                             /* sizeof(t5ui_pixels_t) */
    uint16_t w, h;
    uint32_t stride;                           /* bytes per row; 0: packed */
    uint8_t format;                            /* t5ui_px_t */
    const void *data;
} t5ui_pixels_t;
/* A T5UI_IMAGE shows a copy of px. */
t5_err_t t5ui_image_pixels(t5ui_obj_t *img, const t5ui_pixels_t *px);
/* A T5UI_CANVAS keeps what's drawn on it. Size it first (clear), then draw. */
t5_err_t t5ui_canvas_size(t5ui_obj_t *canvas, int32_t w, int32_t h);
void t5ui_canvas_fill(t5ui_obj_t *canvas, t5ui_color_t color, t5ui_opa_t opa);
t5ui_paint_t *t5ui_canvas_begin(t5ui_obj_t *canvas);
void t5ui_canvas_end(t5ui_obj_t *canvas, t5ui_paint_t *p);
/* On a canvas, or in T5UI_EV_DRAW; coordinates from its top left. */
void t5ui_draw_line(t5ui_paint_t *p, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t width,
                    t5ui_color_t color, t5ui_opa_t opa);
void t5ui_draw_rect(t5ui_paint_t *p, const t5ui_rect_t *r, int32_t radius, t5ui_color_t fill, t5ui_opa_t fill_opa,
                    int32_t border_w, t5ui_color_t border, t5ui_opa_t border_opa);
/* Degrees, clockwise from 3 o'clock. */
void t5ui_draw_arc(t5ui_paint_t *p, int32_t cx, int32_t cy, int32_t radius, int32_t start_deg, int32_t end_deg,
                   int32_t width, t5ui_color_t color, t5ui_opa_t opa, bool rounded);
void t5ui_draw_text(t5ui_paint_t *p, const t5ui_rect_t *r, const char *text, t5ui_text_t style, t5ui_color_t color,
                    t5ui_text_align_t align);
void t5ui_draw_pixels(t5ui_paint_t *p, int32_t x, int32_t y, const t5ui_pixels_t *px);

#ifdef __cplusplus
}
#endif
