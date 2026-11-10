/* SPDX-License-Identifier: Apache-2.0 */
#include <math.h>
#include "md/md_tokens.h"

#define OPA(pct) ((uint8_t)((pct) * 255.0 / 100 + 0.5))

static const md_accent_def_t s_accents[MD_ACCENT_COUNT] = {
    /*                      light 500  light 700  dark 200   dark 700   on light   on dark */
    [MD_ACCENT_PURPLE] = { "purple", 0x6200EE, 0x3700B3, 0xBB86FC, 0x3700B3, 0xFFFFFF, 0x000000 },
    [MD_ACCENT_TEAL]   = { "teal",   0x018786, 0x005457, 0x03DAC6, 0x018786, 0xFFFFFF, 0x000000 },
    [MD_ACCENT_BLUE]   = { "blue",   0x2196F3, 0x1976D2, 0x90CAF9, 0x1976D2, 0xFFFFFF, 0x000000 },
    [MD_ACCENT_INDIGO] = { "indigo", 0x3F51B5, 0x303F9F, 0x9FA8DA, 0x303F9F, 0xFFFFFF, 0x000000 },
    [MD_ACCENT_CYAN]   = { "cyan",   0x00BCD4, 0x0097A7, 0x80DEEA, 0x0097A7, 0x000000, 0x000000 },
    [MD_ACCENT_GREEN]  = { "green",  0x4CAF50, 0x388E3C, 0xA5D6A7, 0x388E3C, 0xFFFFFF, 0x000000 },
    [MD_ACCENT_AMBER]  = { "amber",  0xFFC107, 0xFFA000, 0xFFE082, 0xFFA000, 0x000000, 0x000000 },
    [MD_ACCENT_ORANGE] = { "orange", 0xFF9800, 0xF57C00, 0xFFCC80, 0xF57C00, 0x000000, 0x000000 },
    [MD_ACCENT_RED]    = { "red",    0xF44336, 0xD32F2F, 0xEF9A9A, 0xD32F2F, 0xFFFFFF, 0x000000 },
    [MD_ACCENT_PINK]   = { "pink",   0xE91E63, 0xC2185B, 0xF48FB1, 0xC2185B, 0xFFFFFF, 0x000000 },
};

const md_accent_def_t *md_accent(md_accent_t a)
{
    return &s_accents[(unsigned)a < MD_ACCENT_COUNT ? a : MD_ACCENT_PURPLE];
}

void md_scheme_build(md_scheme_t *s, bool dark, md_accent_t primary, md_accent_t secondary)
{
    const md_accent_def_t *p = md_accent(primary), *q = md_accent(secondary);
    s->dark = dark;
    if (dark) {
        s->c = (md_colors_t){
            .primary = p->dark, .primary_variant = p->dark_variant,
            .secondary = q->dark, .secondary_variant = q->dark,
            .background = 0x121212, .surface = 0x121212, .error = 0xCF6679,
            .on_primary = p->on_dark, .on_secondary = q->on_dark,
            .on_background = 0xFFFFFF, .on_surface = 0xFFFFFF, .on_error = 0x000000,
            .scrim = 0x000000,
        };
        /* Text on dark, as set in MDC: 100% / 70% / 50%. */
        s->on_surface = (md_emphasis_t){ OPA(100), OPA(70), OPA(50), OPA(12) };
        s->states = (md_states_t){ OPA(4), OPA(12), OPA(24), OPA(8), OPA(16) };
        s->ripple_ink_opa = OPA(24);
    } else {
        s->c = (md_colors_t){
            .primary = p->light, .primary_variant = p->light_variant,
            .secondary = q->light, .secondary_variant = q->light_variant,
            .background = 0xFFFFFF, .surface = 0xFFFFFF, .error = 0xB00020,
            .on_primary = p->on_light, .on_secondary = q->on_light,
            .on_background = 0x000000, .on_surface = 0x000000, .on_error = 0xFFFFFF,
            .scrim = 0x000000,
        };
        /* Text on light: 87% / 54% / 38%. */
        s->on_surface = (md_emphasis_t){ OPA(87), OPA(54), OPA(38), OPA(12) };
        s->states = (md_states_t){ OPA(4), OPA(12), OPA(12), OPA(8), OPA(16) };
        s->ripple_ink_opa = OPA(12);
    }
    s->on_primary = (md_emphasis_t){ OPA(100), OPA(74), OPA(38), OPA(12) };
}

/* MD2 dark theme: overlay opacity by elevation. */
static const struct { uint8_t dp, pct; } s_overlay[] = {
    { 0, 0 }, { 1, 5 }, { 2, 7 }, { 3, 8 }, { 4, 9 }, { 6, 11 }, { 8, 12 }, { 12, 14 }, { 16, 15 }, { 24, 16 },
};

uint8_t md_elevation_overlay_opa(int dp)
{
    int n = (int)(sizeof s_overlay / sizeof s_overlay[0]);
    if (dp <= 0) return 0;
    for (int i = 1; i < n; i++) {
        if (dp <= s_overlay[i].dp) {
            /* Linear between table points. */
            int d0 = s_overlay[i - 1].dp, d1 = s_overlay[i].dp;
            float pct = s_overlay[i - 1].pct + (float)(s_overlay[i].pct - s_overlay[i - 1].pct) * (dp - d0) / (d1 - d0);
            return (uint8_t)(pct * 255 / 100 + 0.5f);
        }
    }
    return OPA(16);
}

md_rgb_t md_mix(md_rgb_t fg, md_rgb_t bg, uint8_t opa)
{
    md_rgb_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        uint32_t f = (fg >> sh) & 0xFF, b = (bg >> sh) & 0xFF;
        uint32_t v = (f * opa + b * (255u - opa) + 127u) / 255u;
        out |= v << sh;
    }
    return out;
}

md_rgb_t md_surface_at(const md_scheme_t *s, int dp)
{
    return s->dark ? md_mix(0xFFFFFF, s->c.surface, md_elevation_overlay_opa(dp)) : s->c.surface;
}

static double lin(uint32_t c8)
{
    double c = c8 / 255.0;
    return c <= 0.03928 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double lum(md_rgb_t c)
{
    return 0.2126 * lin((c >> 16) & 0xFF) + 0.7152 * lin((c >> 8) & 0xFF) + 0.0722 * lin(c & 0xFF);
}

uint32_t md_contrast_x100(md_rgb_t a, md_rgb_t b)
{
    double la = lum(a), lb = lum(b);
    if (la < lb) { double t = la; la = lb; lb = t; }
    return (uint32_t)((la + 0.05) / (lb + 0.05) * 100.0 + 0.5);
}

/* MDC 14 typography: size/line in sp (= dp here), tracking in em.
 * One deliberate departure: button and overline are sentence case with
 * light tracking. All-caps labels read as shouting on a 5" panel and cost
 * width that long translated labels need. */
static const md_type_t s_type[MD_TYPE_COUNT] = {
    [MD_TYPE_HEADLINE4] = { "headline4", 34, 40, MD_WEIGHT_REGULAR, false, 7 },
    [MD_TYPE_HEADLINE5] = { "headline5", 24, 32, MD_WEIGHT_REGULAR, false, 0 },
    [MD_TYPE_HEADLINE6] = { "headline6", 20, 32, MD_WEIGHT_MEDIUM, false, 13 },
    [MD_TYPE_SUBTITLE1] = { "subtitle1", 16, 28, MD_WEIGHT_REGULAR, false, 9 },
    [MD_TYPE_BODY1]     = { "body1", 16, 24, MD_WEIGHT_REGULAR, false, 31 },
    [MD_TYPE_BODY2]     = { "body2", 14, 20, MD_WEIGHT_REGULAR, false, 18 },
    [MD_TYPE_BUTTON]    = { "button", 14, 36, MD_WEIGHT_MEDIUM, false, 18 },
    [MD_TYPE_CAPTION]   = { "caption", 12, 20, MD_WEIGHT_REGULAR, false, 33 },
    [MD_TYPE_OVERLINE]  = { "overline", 12, 32, MD_WEIGHT_MEDIUM, false, 42 },
};

const md_type_t *md_type(md_type_role_t r)
{
    return &s_type[(unsigned)r < MD_TYPE_COUNT ? r : MD_TYPE_BODY1];
}

int md_type_tracking_px(md_type_role_t r)
{
    const md_type_t *t = md_type(r);
    return (int)lroundf((float)T5_DP(t->size_dp) * t->tracking_milli_em / 1000.0f);
}

static const md_bezier_t s_curves[MD_CURVE_COUNT] = {
    [MD_CURVE_STANDARD] = { 0.4f, 0.0f, 0.2f, 1.0f },
    [MD_CURVE_DECELERATION] = { 0.0f, 0.0f, 0.2f, 1.0f },
    [MD_CURVE_ACCELERATION] = { 0.4f, 0.0f, 1.0f, 1.0f },
    [MD_CURVE_SHARP] = { 0.4f, 0.0f, 0.6f, 1.0f },
};

const md_bezier_t *md_curve(md_curve_t c)
{
    return &s_curves[(unsigned)c < MD_CURVE_COUNT ? c : MD_CURVE_STANDARD];
}

static float bez(float a, float b, float t)
{
    /* One coordinate of a cubic bezier with P0 = 0 and P3 = 1. */
    float u = 1.0f - t;
    return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t;
}

float md_curve_eval(md_curve_t c, float t)
{
    const md_bezier_t *b = md_curve(c);
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    /* Solve x(s) = t by bisection, then return y(s). */
    float lo = 0, hi = 1, s = t;
    for (int i = 0; i < 30; i++) {
        s = (lo + hi) / 2;
        if (bez(b->x1, b->x2, s) < t) lo = s; else hi = s;
    }
    return bez(b->y1, b->y2, s);
}
