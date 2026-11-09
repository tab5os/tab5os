/* SPDX-License-Identifier: Apache-2.0
 * Phase 4: rotation maths, the orientation filter, framebuffer damage
 * tracking and the MD2 tokens. Phase 6: the shell's decisions that need no
 * LVGL (shell_logic.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "md/md_icons.h"
#include "md/md_tokens.h"
#include "shell_logic.h"
#include "t5_battery/policy.h"
#include "t5_ui/fbtrack.h"
#include "t5_ui/orient.h"
#include "t5_ui/rotation.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- rotation --------------------------------------------------------------- */

static void test_rot_sizes(void)
{
    int32_t w, h;
    t5_rot_size(T5_ROT_0, &w, &h);
    TEST_ASSERT_EQUAL(720, w); TEST_ASSERT_EQUAL(1280, h);
    t5_rot_size(T5_ROT_90, &w, &h);
    TEST_ASSERT_EQUAL(1280, w); TEST_ASSERT_EQUAL(720, h);
    t5_rot_size(T5_ROT_270, &w, &h);
    TEST_ASSERT_EQUAL(1280, w); TEST_ASSERT_EQUAL(720, h);
}

static void test_rot_round_trip_and_corners(void)
{
    for (int r = 0; r < 4; r++) {
        int32_t w, h;
        t5_rot_size((t5_rotation_t)r, &w, &h);
        const int32_t pts[][2] = { {0, 0}, {w - 1, 0}, {0, h - 1}, {w - 1, h - 1}, {123, 45}, {w / 2, h / 3} };
        for (size_t i = 0; i < sizeof pts / sizeof pts[0]; i++) {
            int32_t px, py, lx, ly;
            t5_rot_to_panel((t5_rotation_t)r, pts[i][0], pts[i][1], &px, &py);
            TEST_ASSERT_TRUE(px >= 0 && px < T5_PANEL_W && py >= 0 && py < T5_PANEL_H);
            t5_rot_from_panel((t5_rotation_t)r, px, py, &lx, &ly);
            TEST_ASSERT_EQUAL(pts[i][0], lx);
            TEST_ASSERT_EQUAL(pts[i][1], ly);
        }
    }
    /* 90: the logical top-left sits at the panel's top-right corner. */
    int32_t px, py;
    t5_rot_to_panel(T5_ROT_90, 0, 0, &px, &py);
    TEST_ASSERT_EQUAL(719, px); TEST_ASSERT_EQUAL(0, py);
    t5_rot_to_panel(T5_ROT_270, 0, 0, &px, &py);
    TEST_ASSERT_EQUAL(0, px); TEST_ASSERT_EQUAL(1279, py);
}

static void test_rot_rect(void)
{
    t5_rect_t a = { 10, 20, 109, 39 };            /* 100 x 20 logical, landscape */
    t5_rect_t p = t5_rot_rect_to_panel(T5_ROT_90, a);
    TEST_ASSERT_EQUAL(20, t5_rect_w(&p));         /* rotated: 20 wide, 100 tall */
    TEST_ASSERT_EQUAL(100, t5_rect_h(&p));
    TEST_ASSERT_EQUAL(719 - 39, p.x1);
    TEST_ASSERT_EQUAL(10, p.y1);
    p = t5_rot_rect_to_panel(T5_ROT_180, (t5_rect_t){ 0, 0, 9, 9 });
    TEST_ASSERT_EQUAL(710, p.x1); TEST_ASSERT_EQUAL(1270, p.y1);
    TEST_ASSERT_EQUAL(719, p.x2); TEST_ASSERT_EQUAL(1279, p.y2);
    t5_rect_t c = { -5, -5, 10, 2000 }, b = { 0, 0, 719, 1279 };
    TEST_ASSERT_TRUE(t5_rect_clip(&c, &b));
    TEST_ASSERT_EQUAL(0, c.x1); TEST_ASSERT_EQUAL(1279, c.y2);
    t5_rect_t d = { 800, 0, 900, 10 };
    TEST_ASSERT_FALSE(t5_rect_clip(&d, &b));
}

/* ---- orientation filter ------------------------------------------------------- */

/* "Up" vectors in panel axes for each orientation, held upright. */
static const int32_t UP[4][3] = {
    { 0, -1000, 0 },     /* 0: portrait */
    { 1000, 0, 0 },      /* 90: panel right edge up */
    { 0, 1000, 0 },      /* 180 */
    { -1000, 0, 0 },     /* 270 */
};

static bool feed(t5_orient_t *o, const int32_t v[3], uint32_t t0, uint32_t ms, uint32_t mask, t5_rotation_t *r)
{
    bool changed = false;
    for (uint32_t t = t0; t <= t0 + ms; t += 100) {
        changed |= t5_orient_update(o, v[0], v[1], v[2], t, mask, r);
    }
    return changed;
}

static void test_orient_hold_time(void)
{
    t5_orient_t o;
    t5_orient_init(&o, NULL, T5_ROT_90);
    t5_rotation_t r;
    /* 400 ms in portrait is not enough... */
    TEST_ASSERT_FALSE(feed(&o, UP[0], 0, 400, T5_ROT_ALL, &r));
    TEST_ASSERT_EQUAL(T5_ROT_90, r);
    /* ...500 ms is. */
    TEST_ASSERT_TRUE(feed(&o, UP[0], 500, 100, T5_ROT_ALL, &r));
    TEST_ASSERT_EQUAL(T5_ROT_0, r);
    for (int k = 0; k < 4; k++) {
        feed(&o, UP[k], 10000 + (uint32_t)k * 2000, 600, T5_ROT_ALL, &r);
        TEST_ASSERT_EQUAL(k, r);
    }
}

static void test_orient_flat_ignored(void)
{
    t5_orient_t o;
    t5_orient_init(&o, NULL, T5_ROT_90);
    t5_rotation_t r;
    /* Lying on a table, slightly tilted towards portrait: no change. */
    const int32_t flat[3] = { 0, -200, 980 };
    TEST_ASSERT_FALSE(feed(&o, flat, 0, 3000, T5_ROT_ALL, &r));
    TEST_ASSERT_EQUAL(T5_ROT_90, r);
    TEST_ASSERT_TRUE(o.flat);
    /* A flat reading resets a pending change. */
    feed(&o, UP[0], 4000, 300, T5_ROT_ALL, &r);
    t5_orient_update(&o, flat[0], flat[1], flat[2], 4400, T5_ROT_ALL, &r);
    TEST_ASSERT_FALSE(feed(&o, UP[0], 4500, 300, T5_ROT_ALL, &r));
    TEST_ASSERT_EQUAL(T5_ROT_90, r);
}

static void test_orient_hysteresis(void)
{
    t5_orient_t o;
    t5_orient_init(&o, NULL, T5_ROT_0);
    t5_rotation_t r;
    /* 40 degrees towards 90 (right edge up) stays portrait: dead band. */
    const int32_t at40[3] = { 643, -766, 0 };
    TEST_ASSERT_FALSE(feed(&o, at40, 0, 3000, T5_ROT_ALL, &r));
    TEST_ASSERT_EQUAL(T5_ROT_0, r);
    /* 50 degrees is still inside the band around 45 (45 +- 15 hysteresis). */
    const int32_t at50[3] = { 766, -643, 0 };
    TEST_ASSERT_FALSE(feed(&o, at50, 4000, 3000, T5_ROT_ALL, &r));
    /* 65 degrees switches. */
    const int32_t at65[3] = { 906, -423, 0 };
    TEST_ASSERT_TRUE(feed(&o, at65, 8000, 600, T5_ROT_ALL, &r));
    TEST_ASSERT_EQUAL(T5_ROT_90, r);
    /* Back to 50 degrees: stays at 90 (no flicker around 45). */
    TEST_ASSERT_FALSE(feed(&o, at50, 9000, 3000, T5_ROT_ALL, &r));
    TEST_ASSERT_EQUAL(T5_ROT_90, r);
}

static void test_orient_mask_and_bad_samples(void)
{
    t5_orient_t o;
    t5_orient_init(&o, NULL, T5_ROT_90);
    t5_rotation_t r;
    /* A landscape-only app: portrait readings never rotate. */
    TEST_ASSERT_FALSE(feed(&o, UP[0], 0, 2000, T5_ROT_LANDSCAPE, &r));
    TEST_ASSERT_EQUAL(T5_ROT_90, r);
    TEST_ASSERT_TRUE(feed(&o, UP[3], 3000, 600, T5_ROT_LANDSCAPE, &r));
    TEST_ASSERT_EQUAL(T5_ROT_270, r);
    /* Shaking (2.5 g) is ignored. */
    const int32_t shake[3] = { 0, -2500, 0 };
    TEST_ASSERT_FALSE(feed(&o, shake, 5000, 2000, T5_ROT_ALL, &r));
    /* The allowed set shrinking moves the screen even while flat: to the
     * same-aspect partner first, else the first allowed rotation. */
    TEST_ASSERT_TRUE(t5_orient_update(&o, 0, 0, 1000, 9000, T5_ROT_MASK(T5_ROT_90), &r));
    TEST_ASSERT_EQUAL(T5_ROT_90, r);
    TEST_ASSERT_TRUE(t5_orient_update(&o, 0, 0, 1000, 9100, T5_ROT_PORTRAIT, &r));
    TEST_ASSERT_EQUAL(T5_ROT_0, r);
}

/* The one rule for leaving a rotation the foreground app doesn't allow
 * (ui.c's t5_ui_set_allowed_rotations and the filter both use it). */
static void test_orient_fallback(void)
{
    TEST_ASSERT_EQUAL(T5_ROT_90, t5_orient_fallback(T5_ROT_90, T5_ROT_ALL));
    TEST_ASSERT_EQUAL(T5_ROT_90, t5_orient_fallback(T5_ROT_90, 0));
    TEST_ASSERT_EQUAL(T5_ROT_270, t5_orient_fallback(T5_ROT_90, T5_ROT_MASK(T5_ROT_270)));
    TEST_ASSERT_EQUAL(T5_ROT_180, t5_orient_fallback(T5_ROT_0, T5_ROT_MASK(T5_ROT_180) | T5_ROT_MASK(T5_ROT_90)));
    /* No partner: the first allowed, from either landscape. */
    TEST_ASSERT_EQUAL(T5_ROT_0, t5_orient_fallback(T5_ROT_90, T5_ROT_PORTRAIT));
    TEST_ASSERT_EQUAL(T5_ROT_0, t5_orient_fallback(T5_ROT_270, T5_ROT_PORTRAIT));
    TEST_ASSERT_EQUAL(T5_ROT_90, t5_orient_fallback(T5_ROT_180, T5_ROT_LANDSCAPE));
}

static void test_axis_map(void)
{
    t5_axis_map_t m = { .src = { 1, 0, 2 }, .sign = { -1, 1, 1 } };
    int16_t chip[3] = { 100, 200, 300 };
    int32_t p[3];
    t5_axis_map_apply(&m, chip, p);
    TEST_ASSERT_EQUAL(-200, p[0]);
    TEST_ASSERT_EQUAL(100, p[1]);
    TEST_ASSERT_EQUAL(300, p[2]);
}

/* ---- damage tracking ------------------------------------------------------------ */

static uint32_t covered(const t5_rect_t *r, int n, int32_t x, int32_t y)
{
    uint32_t c = 0;
    for (int i = 0; i < n; i++) c += x >= r[i].x1 && x <= r[i].x2 && y >= r[i].y1 && y <= r[i].y2;
    return c;
}


static void test_fbt_initial_full(void)
{
    t5_fbt_t t;
    t5_fbt_init(&t, 1280, 720);
    for (int i = 0; i < T5_FBT_BUFS; i++) TEST_ASSERT_EQUAL_UINT32(1280u * 720u, t5_fbt_pixels(&t, i));
    t5_rect_t out[T5_FBT_RECTS];
    TEST_ASSERT_EQUAL(1, t5_fbt_take(&t, 0, out, T5_FBT_RECTS));
    TEST_ASSERT_EQUAL(0, t5_fbt_pixels(&t, 0));
    TEST_ASSERT_EQUAL(0, t5_fbt_take(&t, 0, out, T5_FBT_RECTS));
}

static void test_fbt_lag(void)
{
    t5_fbt_t t;
    t5_fbt_init(&t, 1280, 720);
    t5_rect_t out[T5_FBT_RECTS];
    for (int i = 0; i < 3; i++) t5_fbt_take(&t, i, out, T5_FBT_RECTS);
    /* Frame 1 damages A, written to buffer 1. Frame 2 damages B, written to
     * buffer 2, which must also catch up on A. */
    t5_rect_t A = { 0, 0, 99, 99 }, B = { 1000, 600, 1099, 699 };
    t5_fbt_add(&t, A);
    int n = t5_fbt_take(&t, 1, out, T5_FBT_RECTS);
    TEST_ASSERT_EQUAL(1, n);
    t5_fbt_add(&t, B);
    n = t5_fbt_take(&t, 2, out, T5_FBT_RECTS);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_UINT32(20000, t5_fbt_pixels(&t, 0));
    TEST_ASSERT_EQUAL_UINT32(10000, t5_fbt_pixels(&t, 1));   /* only B */
}

static void test_fbt_merge_and_overflow(void)
{
    t5_fbt_t t;
    t5_fbt_init(&t, 1280, 720);
    t5_rect_t out[T5_FBT_RECTS];
    for (int i = 0; i < 3; i++) t5_fbt_take(&t, i, out, T5_FBT_RECTS);
    /* Adjacent strips merge into one. */
    t5_fbt_add(&t, (t5_rect_t){ 0, 0, 639, 99 });
    t5_fbt_add(&t, (t5_rect_t){ 0, 100, 639, 199 });
    TEST_ASSERT_EQUAL_UINT32(640u * 200u, t5_fbt_pixels(&t, 0));
    TEST_ASSERT_EQUAL(1, t5_fbt_take(&t, 0, out, T5_FBT_RECTS));
    TEST_ASSERT_EQUAL(0, out[0].y1);
    TEST_ASSERT_EQUAL(199, out[0].y2);
    /* Many scattered rectangles: more than the list holds, and none lost. */
    t5_rect_t added[40];
    for (int i = 0; i < 40; i++) {
        int x = (i * 97) % 1200, y = (i * 53) % 650;
        added[i] = (t5_rect_t){ x, y, x + 9, y + 9 };
        t5_fbt_add(&t, added[i]);
    }
    int n = t5_fbt_take(&t, 1, out, T5_FBT_RECTS);
    TEST_ASSERT_TRUE(n >= 1 && n <= T5_FBT_RECTS);
    for (int i = 0; i < 40; i++) {
        TEST_ASSERT_TRUE(covered(out, n, added[i].x1, added[i].y1) > 0);
        TEST_ASSERT_TRUE(covered(out, n, added[i].x2, added[i].y2) > 0);
    }
    /* Out-of-bounds damage is clipped. */
    t5_fbt_init(&t, 100, 100);
    for (int i = 0; i < 3; i++) t5_fbt_take(&t, i, out, T5_FBT_RECTS);
    t5_fbt_add(&t, (t5_rect_t){ 90, 90, 200, 200 });
    TEST_ASSERT_EQUAL_UINT32(100, t5_fbt_pixels(&t, 0));
    t5_fbt_add(&t, (t5_rect_t){ 300, 300, 400, 400 });
    TEST_ASSERT_EQUAL_UINT32(100, t5_fbt_pixels(&t, 0));
    /* take() with a small cap returns one bounding box. */
    t5_fbt_add(&t, (t5_rect_t){ 0, 0, 9, 9 });
    TEST_ASSERT_EQUAL(1, t5_fbt_take(&t, 0, out, 1));
    TEST_ASSERT_EQUAL(0, out[0].x1);
    TEST_ASSERT_EQUAL(99, out[0].x2);
}

static void test_fbt_pick(void)
{
    TEST_ASSERT_EQUAL(1, t5_fbt_pick(3, 0, 0));
    TEST_ASSERT_EQUAL(2, t5_fbt_pick(3, 0, 1));
    TEST_ASSERT_EQUAL(0, t5_fbt_pick(3, 2, 1));
    TEST_ASSERT_EQUAL(-1, t5_fbt_pick(2, 0, 1));
}

/* ---- tokens ---------------------------------------------------------------------- */

static void test_tokens_density_and_type(void)
{
    TEST_ASSERT_EQUAL(34, md_type(MD_TYPE_HEADLINE4)->size_dp);
    /* Sentence case throughout: no role shouts. */
    for (int r = 0; r < MD_TYPE_COUNT; r++) TEST_ASSERT_FALSE(md_type((md_type_role_t)r)->uppercase);
    TEST_ASSERT_EQUAL(MD_WEIGHT_MEDIUM, md_type(MD_TYPE_HEADLINE6)->weight);
    /* Button tracking 0.018 em at 14 dp (28 px) = 0.5 px -> 1. */
    TEST_ASSERT_EQUAL(1, md_type_tracking_px(MD_TYPE_BUTTON));
    TEST_ASSERT_EQUAL(0, md_type_tracking_px(MD_TYPE_HEADLINE5));
}

static void test_tokens_dark_scheme(void)
{
    md_scheme_t s;
    md_scheme_build(&s, true, MD_ACCENT_PURPLE, MD_ACCENT_TEAL);
    TEST_ASSERT_EQUAL_HEX32(0x121212, s.c.surface);
    TEST_ASSERT_EQUAL_HEX32(0xBB86FC, s.c.primary);
    /* Overlays: 5 / 9 / 12 / 16 % at 1 / 4 / 8 / 24 dp. */
    TEST_ASSERT_EQUAL(13, md_elevation_overlay_opa(1));
    TEST_ASSERT_EQUAL(23, md_elevation_overlay_opa(4));
    TEST_ASSERT_EQUAL(31, md_elevation_overlay_opa(8));
    TEST_ASSERT_EQUAL(41, md_elevation_overlay_opa(24));
    TEST_ASSERT_EQUAL(41, md_elevation_overlay_opa(48));
    TEST_ASSERT_EQUAL(0, md_elevation_overlay_opa(0));
    /* Higher surfaces are lighter, monotonically. */
    md_rgb_t prev = md_surface_at(&s, 0);
    for (int dp = 1; dp <= 24; dp++) {
        md_rgb_t c = md_surface_at(&s, dp);
        TEST_ASSERT_TRUE((c & 0xFF) >= (prev & 0xFF));
        prev = c;
    }
    TEST_ASSERT_EQUAL_HEX32(0x1E1E1E, md_surface_at(&s, 1));   /* MD2: 1 dp ~ #1E1E1E */
    /* Text must stay readable on the highest surface and on the primary. */
    TEST_ASSERT_TRUE(md_contrast_x100(s.c.on_surface, md_surface_at(&s, 24)) >= 700);
    TEST_ASSERT_TRUE(md_contrast_x100(s.c.on_primary, s.c.primary) >= 450);
    for (int a = 0; a < MD_ACCENT_COUNT; a++) {
        md_scheme_build(&s, true, (md_accent_t)a, MD_ACCENT_TEAL);
        /* Dark accents are 200-tones: AA contrast against the dark surface. */
        TEST_ASSERT_TRUE_MESSAGE(md_contrast_x100(s.c.primary, 0x121212) >= 450, md_accent((md_accent_t)a)->name);
        TEST_ASSERT_TRUE_MESSAGE(md_contrast_x100(s.c.on_primary, s.c.primary) >= 450, md_accent((md_accent_t)a)->name);
    }
}

static void test_tokens_light_names_exist(void)
{
    md_scheme_t s;
    md_scheme_build(&s, false, MD_ACCENT_PURPLE, MD_ACCENT_TEAL);
    TEST_ASSERT_FALSE(s.dark);
    TEST_ASSERT_EQUAL_HEX32(0x6200EE, s.c.primary);
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFF, md_surface_at(&s, 24));
    TEST_ASSERT_EQUAL_HEX32(0xB00020, s.c.error);
}

static void test_tokens_curves(void)
{
    for (int c = 0; c < MD_CURVE_COUNT; c++) {
        TEST_ASSERT_EQUAL_FLOAT(0.0f, md_curve_eval((md_curve_t)c, 0));
        TEST_ASSERT_EQUAL_FLOAT(1.0f, md_curve_eval((md_curve_t)c, 1));
        float prev = 0;
        for (int i = 1; i <= 20; i++) {
            float v = md_curve_eval((md_curve_t)c, (float)i / 20);
            TEST_ASSERT_TRUE(v >= prev - 1e-4f);
            prev = v;
        }
    }
    /* Deceleration starts fast, acceleration starts slow. */
    TEST_ASSERT_TRUE(md_curve_eval(MD_CURVE_DECELERATION, 0.25f) > 0.5f);
    TEST_ASSERT_TRUE(md_curve_eval(MD_CURVE_ACCELERATION, 0.25f) < 0.15f);
    TEST_ASSERT_EQUAL_HEX32(0x808080, md_mix(0xFFFFFF, 0x000000, 128) & 0xFEFEFE);
}

static void test_fbt_subtract(void)
{
    t5_rect_t a[2] = { { 0, 0, 99, 99 }, { 200, 0, 209, 9 } };
    t5_rect_t cut[2] = { { 20, 30, 59, 69 }, { 0, 90, 300, 300 } };
    t5_rect_t out[32];
    int n = t5_fbt_subtract(a, 2, cut, 2, out, 32);
    /* Every pixel: in the answer exactly when it is in a and in no cut, once. */
    for (int32_t y = 0; y < 120; y++) {
        for (int32_t x = 0; x < 220; x++) {
            bool in_a = covered(a, 2, x, y) > 0, in_cut = covered(cut, 2, x, y) > 0;
            TEST_ASSERT_EQUAL_UINT32((in_a && !in_cut) ? 1 : 0, covered(out, n, x, y));
        }
    }
    /* A full-screen cut leaves nothing (the scrolling case). */
    t5_rect_t full = { 0, 0, 1279, 719 };
    TEST_ASSERT_EQUAL(0, t5_fbt_subtract(a, 2, &full, 1, out, 32));
    /* No room: the uncut input comes back (a superset). */
    TEST_ASSERT_EQUAL(1, t5_fbt_subtract(a, 1, cut, 1, out, 1));
}

/* ---- the shell's logic ------------------------------------------------------------ */

static void test_shell_recents(void)
{
    shell_recents_t r;
    memset(&r, 0, sizeof r);
    char ids[SHELL_RECENTS][T5_APP_ID_LEN];
    TEST_ASSERT_EQUAL(0, shell_recents_list(&r, ids, SHELL_RECENTS));
    shell_recents_note(&r, "a.a");
    shell_recents_note(&r, "b.b");
    shell_recents_note(&r, "c.c");
    TEST_ASSERT_EQUAL(3, shell_recents_list(&r, ids, SHELL_RECENTS));
    TEST_ASSERT_EQUAL_STRING("c.c", ids[0]);
    TEST_ASSERT_EQUAL_STRING("a.a", ids[2]);
    /* Launching a listed app moves it to the front, without a duplicate. */
    shell_recents_note(&r, "a.a");
    TEST_ASSERT_EQUAL(3, shell_recents_list(&r, ids, SHELL_RECENTS));
    TEST_ASSERT_EQUAL_STRING("a.a", ids[0]);
    TEST_ASSERT_EQUAL_STRING("c.c", ids[1]);
    TEST_ASSERT_EQUAL_STRING("b.b", ids[2]);
    /* Full: the oldest drops off. */
    char id[8];
    for (int i = 0; i < SHELL_RECENTS; i++) {
        snprintf(id, sizeof id, "x.%d", i);
        shell_recents_note(&r, id);
    }
    TEST_ASSERT_EQUAL(SHELL_RECENTS, shell_recents_list(&r, ids, SHELL_RECENTS));
    TEST_ASSERT_EQUAL_STRING("x.5", ids[0]);
    TEST_ASSERT_EQUAL_STRING("x.0", ids[SHELL_RECENTS - 1]);
    TEST_ASSERT_EQUAL(2, shell_recents_list(&r, ids, 2));
    /* A full-length id survives intact. */
    char longid[T5_APP_ID_LEN];
    memset(longid, 'z', sizeof longid - 1);
    longid[sizeof longid - 1] = 0;
    shell_recents_note(&r, longid);
    shell_recents_list(&r, ids, 1);
    TEST_ASSERT_EQUAL_STRING(longid, ids[0]);
}

static void test_shell_battery_icon(void)
{
    shell_status_t st = { .battery_pct = 50 };
    TEST_ASSERT_EQUAL_STRING(MD_ICON_BATTERY_STD, shell_battery_icon(&st));
    st.battery_pct = T5_DIS_WARN_PCT + 1;
    TEST_ASSERT_EQUAL_STRING(MD_ICON_BATTERY_STD, shell_battery_icon(&st));
    st.battery_pct = T5_DIS_WARN_PCT;            /* where the battery service warns */
    TEST_ASSERT_EQUAL_STRING(MD_ICON_BATTERY_ALERT, shell_battery_icon(&st));
    st.battery_pct = 95;
    TEST_ASSERT_EQUAL_STRING(MD_ICON_BATTERY_FULL, shell_battery_icon(&st));
    st.battery_pct = -1;                         /* not measured yet: no alarm */
    TEST_ASSERT_EQUAL_STRING(MD_ICON_BATTERY_STD, shell_battery_icon(&st));
    st = (shell_status_t){ .battery_pct = 10, .low_power = true };
    TEST_ASSERT_EQUAL_STRING(MD_ICON_BATTERY_SAVER, shell_battery_icon(&st));
    st.charging = true;                          /* charging wins over saver */
    TEST_ASSERT_EQUAL_STRING(MD_ICON_BATTERY_CHARGING_FULL, shell_battery_icon(&st));
    /* Full on USB power without charging (the charger stopped) counts as charged. */
    st = (shell_status_t){ .battery_pct = 99, .usb_power = true };
    TEST_ASSERT_EQUAL_STRING(MD_ICON_BATTERY_CHARGING_FULL, shell_battery_icon(&st));
    st.battery_pct = 98;
    TEST_ASSERT_EQUAL_STRING(MD_ICON_BATTERY_FULL, shell_battery_icon(&st));
}

static void test_shell_app_order_and_removal(void)
{
    shell_app_t a[] = {
        { .id = "com.x.zeta", .name = "zeta" },
        { .id = "com.x.alpha", .name = "" },        /* no name: sorted and shown by id */
        { .id = "com.x.b", .name = "Beta" },
        { .id = "com.x.a", .name = "ALPHA" },
    };
    qsort(a, 4, sizeof a[0], shell_app_name_cmp);
    TEST_ASSERT_EQUAL_STRING("ALPHA", shell_app_name(&a[0]));
    TEST_ASSERT_EQUAL_STRING("Beta", shell_app_name(&a[1]));
    TEST_ASSERT_EQUAL_STRING("com.x.alpha", shell_app_name(&a[2]));
    TEST_ASSERT_EQUAL_STRING("zeta", shell_app_name(&a[3]));
    shell_app_t installed = { .id = "com.x.y" }, builtin = { .id = "com.t5.files", .builtin = true };
    TEST_ASSERT_TRUE(shell_app_removable(&installed));
    TEST_ASSERT_FALSE(shell_app_removable(&builtin));
    installed.running = true;
    TEST_ASSERT_FALSE(shell_app_removable(&installed));
}

static void test_shell_time_text(void)
{
    struct tm tm = { .tm_hour = 9, .tm_min = 5, .tm_wday = 4, .tm_mday = 5, .tm_mon = 10 };
    char buf[48];
    shell_fmt_clock(&tm, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("09:05", buf);
    shell_fmt_date(&tm, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("Thursday, 5 November", buf);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_rot_sizes);
    RUN_TEST(test_rot_round_trip_and_corners);
    RUN_TEST(test_rot_rect);
    RUN_TEST(test_orient_hold_time);
    RUN_TEST(test_orient_flat_ignored);
    RUN_TEST(test_orient_hysteresis);
    RUN_TEST(test_orient_mask_and_bad_samples);
    RUN_TEST(test_orient_fallback);
    RUN_TEST(test_axis_map);
    RUN_TEST(test_fbt_initial_full);
    RUN_TEST(test_fbt_lag);
    RUN_TEST(test_fbt_merge_and_overflow);
    RUN_TEST(test_fbt_pick);
    RUN_TEST(test_fbt_subtract);
    RUN_TEST(test_tokens_density_and_type);
    RUN_TEST(test_tokens_dark_scheme);
    RUN_TEST(test_tokens_light_names_exist);
    RUN_TEST(test_tokens_curves);
    RUN_TEST(test_shell_recents);
    RUN_TEST(test_shell_battery_icon);
    RUN_TEST(test_shell_app_order_and_removal);
    RUN_TEST(test_shell_time_text);
    return UNITY_END();
}
