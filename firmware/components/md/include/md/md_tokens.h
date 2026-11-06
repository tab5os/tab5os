/* SPDX-License-Identifier: Apache-2.0
 * Material Design 2 tokens (plan 4.5), as plain C data with no LVGL
 * dependency, shared by the firmware, the simulator and host tests.
 *
 * Sources: Material Components Web 14 (typography, elevation, animation,
 * ripple, component sizes), see the design doc. The dark-theme overlay
 * percentages and accent 200-tones are from the MD2 dark theme guidance and
 * still need checking against the spec (design doc, "Tokens").
 *
 * Dark is the polished set (plan, "Dark first"); the light set has the same
 * token names and is not tuned yet.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- density: a fixed 2 px per dp (design "Density") ------------------- */
#define MD_PX_PER_DP 2
#define T5_DP(n) ((n) * MD_PX_PER_DP)

/* ---- grid -------------------------------------------------------------- */
#define MD_GRID_DP 8
#define MD_GRID_HALF_DP 4
#define MD_MARGIN_DP 16
#define MD_GUTTER_DP 8

/* ---- colour -------------------------------------------------------------- */
typedef uint32_t md_rgb_t;    /* 0xRRGGBB */

typedef struct {
    md_rgb_t primary, primary_variant, secondary, secondary_variant;
    md_rgb_t background, surface, error;
    md_rgb_t on_primary, on_secondary, on_background, on_surface, on_error;
    md_rgb_t scrim;           /* dialogs and drawers */
} md_colors_t;

/* Text and icon emphasis on a surface, 0..255 opacity. */
typedef struct {
    uint8_t high, medium, disabled;
    uint8_t divider;          /* lines and outlines */
} md_emphasis_t;

/* Interaction state overlays (MDC ripple/states), 0..255 opacity of the
 * "on" colour drawn over a surface. */
typedef struct {
    uint8_t hover, focus, pressed, selected, dragged;
} md_states_t;

typedef enum {
    MD_ACCENT_PURPLE,         /* MD2 baseline placeholder */
    MD_ACCENT_TEAL,
    MD_ACCENT_BLUE,
    MD_ACCENT_INDIGO,
    MD_ACCENT_CYAN,
    MD_ACCENT_GREEN,
    MD_ACCENT_AMBER,
    MD_ACCENT_ORANGE,
    MD_ACCENT_RED,
    MD_ACCENT_PINK,
    MD_ACCENT_COUNT,
} md_accent_t;

typedef struct {
    const char *name;
    md_rgb_t light, light_variant;   /* 500 / 700 tones, for light surfaces */
    md_rgb_t dark, dark_variant;     /* 200 / 700 tones, desaturated for dark */
    md_rgb_t on_light, on_dark;      /* text on the accent */
} md_accent_def_t;

typedef struct {
    bool dark;
    md_colors_t c;
    md_emphasis_t on_surface;        /* text on surface/background */
    md_emphasis_t on_primary;
    md_states_t states;
    uint8_t ripple_ink_opa;          /* 24% white ink on dark, 12% dark ink on light */
} md_scheme_t;

/* Build a colour scheme: dark or light, with an accent as primary. */
void md_scheme_build(md_scheme_t *out, bool dark, md_accent_t primary, md_accent_t secondary);
const md_accent_def_t *md_accent(md_accent_t a);

/* Dark theme elevation: a white overlay on the surface that grows with
 * elevation. Returns 0..255 opacity for an elevation in dp. */
uint8_t md_elevation_overlay_opa(int elevation_dp);
/* The resulting surface colour on a dark scheme (surface blended with white),
 * or plain surface on a light one. */
md_rgb_t md_surface_at(const md_scheme_t *s, int elevation_dp);

/* Blend fg over bg at opa (0..255), per channel, rounded. */
md_rgb_t md_mix(md_rgb_t fg, md_rgb_t bg, uint8_t opa);
/* WCAG relative-luminance contrast ratio x100 (450 = 4.5:1). */
uint32_t md_contrast_x100(md_rgb_t a, md_rgb_t b);

/* ---- type ------------------------------------------------------------------ */
typedef enum {
    MD_TYPE_HEADLINE4,
    MD_TYPE_HEADLINE5,
    MD_TYPE_HEADLINE6,
    MD_TYPE_SUBTITLE1,
    MD_TYPE_BODY1,
    MD_TYPE_BODY2,
    MD_TYPE_BUTTON,
    MD_TYPE_CAPTION,
    MD_TYPE_OVERLINE,
    MD_TYPE_COUNT,
} md_type_role_t;

typedef enum { MD_WEIGHT_REGULAR, MD_WEIGHT_MEDIUM } md_weight_t;

typedef struct {
    const char *name;
    uint8_t size_dp, line_dp;
    md_weight_t weight;
    bool uppercase;
    int16_t tracking_milli_em;       /* letter spacing, 1/1000 em */
} md_type_t;

const md_type_t *md_type(md_type_role_t role);
/* Letter spacing in px for a role (rounded). */
int md_type_tracking_px(md_type_role_t role);

/* ---- shape ------------------------------------------------------------------ */
#define MD_SHAPE_SMALL_DP 4          /* buttons, chips, text fields */
#define MD_SHAPE_MEDIUM_DP 4         /* cards, dialogs, menus */
#define MD_SHAPE_LARGE_DP 0          /* sheets, drawers */

/* ---- elevation (dp) per component -------------------------------------------- */
#define MD_ELEV_CARD 1
#define MD_ELEV_SWITCH_HANDLE 1
#define MD_ELEV_SLIDER_THUMB 1
#define MD_ELEV_BUTTON_RAISED 2
#define MD_ELEV_APP_BAR 4
#define MD_ELEV_FAB 6
#define MD_ELEV_SNACKBAR 6
#define MD_ELEV_MENU 8
#define MD_ELEV_DRAWER 16
#define MD_ELEV_DIALOG 24

/* ---- component sizes (dp) ------------------------------------------------------ */
#define MD_APP_BAR_DENSE_DP 48       /* landscape default */
#define MD_APP_BAR_DP 56
#define MD_APP_BAR_PROMINENT_DP 64
#define MD_BUTTON_H_DP 36
#define MD_BUTTON_MIN_W_DP 64
#define MD_BUTTON_PAD_DP 16
#define MD_TOUCH_DP 48
#define MD_FAB_DP 56
#define MD_FAB_MINI_DP 40
#define MD_LIST_ROW_1_DP 48
#define MD_LIST_ROW_2_DP 64
#define MD_LIST_ROW_3_DP 72
#define MD_TEXT_FIELD_H_DP 56
#define MD_SWITCH_TRACK_W_DP 36
#define MD_SWITCH_TRACK_H_DP 14
#define MD_SWITCH_HANDLE_DP 20
#define MD_SLIDER_TRACK_DP 4
#define MD_SLIDER_THUMB_DP 20
#define MD_CHECKBOX_DP 18
#define MD_CHIP_H_DP 32
#define MD_TABS_H_DP 48
#define MD_TABS_ICON_H_DP 72
#define MD_SNACKBAR_MIN_W_DP 344
#define MD_SNACKBAR_MAX_W_DP 672
#define MD_DRAWER_W_DP 256
#define MD_DIALOG_MIN_W_DP 280
#define MD_DIALOG_MAX_W_DP 560
#define MD_ICON_DP 24

/* ---- motion ---------------------------------------------------------------------- */
typedef struct {
    float x1, y1, x2, y2;
} md_bezier_t;

typedef enum {
    MD_CURVE_STANDARD,        /* moving on screen, elevation */
    MD_CURVE_DECELERATION,    /* entering */
    MD_CURVE_ACCELERATION,    /* leaving for good */
    MD_CURVE_SHARP,           /* leaving temporarily */
    MD_CURVE_COUNT,
} md_curve_t;

const md_bezier_t *md_curve(md_curve_t c);
/* Evaluate the curve: progress t in [0,1] to eased value in [0,1]. */
float md_curve_eval(md_curve_t c, float t);

#define MD_DUR_RIPPLE_FADE_IN_MS 75
#define MD_DUR_RIPPLE_EXPAND_MS 225
#define MD_DUR_RIPPLE_FADE_OUT_MS 150
#define MD_DUR_ELEVATION_MS 280
#define MD_DUR_DIALOG_IN_MS 150
#define MD_DUR_DIALOG_OUT_MS 75
#define MD_DUR_DRAWER_IN_MS 250
#define MD_DUR_DRAWER_OUT_MS 200
#define MD_DUR_MENU_SCALE_MS 120
#define MD_DUR_MENU_FADE_IN_MS 30
#define MD_DUR_MENU_FADE_OUT_MS 75
#define MD_DUR_SCREEN_MS 300          /* full-screen transitions (our choice) */

#ifdef __cplusplus
}
#endif
