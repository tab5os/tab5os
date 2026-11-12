/* SPDX-License-Identifier: Apache-2.0
 * t5ui's promises, checked on the host:
 *   - apps compile in nothing of LVGL: the host build has no LVGL on its
 *     include path, so if t5ui.h ever pulled in an LVGL header this file
 *     wouldn't compile;
 *   - the numbers apps compile in never change meaning (pinned below);
 *   - the core (t5ui_core.c) checks every call before a backend sees it.
 *     A fake backend here records what reaches it.
 */
#include <string.h>
#include "unity.h"
#include "t5_gui/t5ui.h"
#include "t5_gui/t5ui_icons.h"
#include "t5ui_backend.h"

#if defined(LV_VERSION_MAJOR) || defined(LVGL_VERSION_MAJOR) || defined(LV_SIZE_CONTENT)
#error "t5ui.h reaches LVGL"
#endif

/* ---- a fake backend ---------------------------------------------------------------- */
static struct {
    int sets, gets, creates, styles, anims, ons, cmds, images;
    t5ui_prop_t prop;
    uint32_t at;
    t5ui_val_t val;
    char str[64];
} be;

static t5ui_obj_t *const OBJ = (t5ui_obj_t *)0x1000;
static t5ui_style_t *const STYLE = (t5ui_style_t *)0x2000;

t5ui_obj_t *t5ui_be_create(t5ui_obj_t *parent, t5ui_kind_t kind)
{
    (void)parent;
    (void)kind;
    be.creates++;
    return OBJ;
}

t5_err_t t5ui_be_set(t5ui_obj_t *obj, t5ui_prop_t p, uint32_t at, const t5ui_val_t *v)
{
    (void)obj;
    be.sets++;
    be.prop = p;
    be.at = at;
    be.val = *v;
    if (v->type == T5UI_T_STR) {
        strncpy(be.str, v->s, sizeof be.str - 1);
        be.val.s = be.str;
    }
    return T5_OK;
}

bool t5ui_be_get(t5ui_obj_t *obj, t5ui_prop_t p, t5ui_val_t *v)
{
    (void)obj;
    be.gets++;
    if (p == T5UI_P_VALUE) v->i = 42;
    else if (p == T5UI_P_BG_COLOR) v->c = 0xff123456;   /* the core masks off the top byte */
    else if (p == T5UI_P_TEXT) v->s = "hello";
    else return false;
    return true;
}

t5_err_t t5ui_be_style_set(t5ui_style_t *s, t5ui_prop_t p, const t5ui_val_t *v)
{
    (void)s;
    (void)v;
    be.styles++;
    be.prop = p;
    return T5_OK;
}

t5_err_t t5ui_be_style_add(t5ui_obj_t *obj, t5ui_style_t *s, uint32_t at)
{
    (void)obj;
    (void)s;
    be.at = at;
    return T5_OK;
}

t5_err_t t5ui_be_animate(t5ui_obj_t *obj, t5ui_prop_t p, int32_t from, int32_t to, uint32_t ms, uint32_t delay_ms,
                         t5ui_ease_t ease)
{
    (void)obj;
    (void)from;
    (void)to;
    (void)ms;
    (void)delay_ms;
    (void)ease;
    be.anims++;
    be.prop = p;
    return T5_OK;
}

t5_err_t t5ui_be_on(t5ui_obj_t *obj, t5ui_ev_t ev, t5ui_event_cb_t cb, void *user)
{
    (void)obj;
    (void)ev;
    (void)cb;
    (void)user;
    be.ons++;
    return T5_OK;
}

int32_t t5ui_be_cmd(t5ui_obj_t *obj, t5ui_cmd_t cmd, int32_t a, int32_t b, int32_t c, const char *text)
{
    (void)obj;
    (void)cmd;
    (void)b;
    (void)c;
    (void)text;
    be.cmds++;
    return a;
}

t5_err_t t5ui_be_image_pixels(t5ui_obj_t *img, const t5ui_pixels_t *px)
{
    (void)img;
    (void)px;
    be.images++;
    return T5_OK;
}

void setUp(void)
{
    memset(&be, 0, sizeof be);
}

void tearDown(void) {}

static void cb(t5ui_event_t *e)
{
    (void)e;
}

/* ---- what apps compile in ------------------------------------------------------------- */
static void test_encodings(void)
{
    TEST_ASSERT_EQUAL_HEX32(0x123456, T5UI_RGB(0x12, 0x34, 0x56));
    TEST_ASSERT_EQUAL_HEX32(0x40000032, (uint32_t)T5UI_PCT(50));
    TEST_ASSERT_EQUAL_HEX32(0x400003e8, (uint32_t)T5UI_PCT(1000));
    TEST_ASSERT_TRUE(T5UI_CONTENT > T5UI_PCT(1000));
    TEST_ASSERT_EQUAL_HEX32(0x00030004, T5UI_AT(T5UI_PART_KNOB, T5UI_STATE_PRESSED));
    TEST_ASSERT_EQUAL(16, sizeof(t5ui_rect_t));
    TEST_ASSERT_EQUAL(3, strlen(T5UI_ICON_ARROW_BACK));        /* one UTF-8 glyph */
}

/* Apps compile these numbers in: they may never change. */
static void test_numbers_pinned(void)
{
    TEST_ASSERT_EQUAL(1, T5UI_BOX);
    TEST_ASSERT_EQUAL(19, T5UI_KEYBOARD);
    TEST_ASSERT_EQUAL(100, T5UI_MD_TEXT);
    TEST_ASSERT_EQUAL(126, T5UI_MD_LIST);
    TEST_ASSERT_EQUAL(3, T5UI_P_WIDTH);
    TEST_ASSERT_EQUAL(60, T5UI_P_BG_COLOR);
    TEST_ASSERT_EQUAL(100, T5UI_P_TEXT);
    TEST_ASSERT_EQUAL(177, T5UI_P_CHECKED);
    TEST_ASSERT_EQUAL(200, T5UI_P_VALUE);
    TEST_ASSERT_EQUAL(221, T5UI_P_ANIM_TIME);
    TEST_ASSERT_EQUAL(1, T5UI_EV_CLICK);
    TEST_ASSERT_EQUAL(16, T5UI_EV_CANCEL);
    TEST_ASSERT_EQUAL(13, T5UI_CMD_FOCUS);
}

/* Every property has one type, and only look properties take a selector. */
static void test_prop_table(void)
{
    int n = 0;
    for (int p = 0; p < 256; p++) {
        uint8_t info = t5ui_prop_info((t5ui_prop_t)p);
        if (!info) continue;
        n++;
        uint8_t t = info & T5UI_T_MASK;
        TEST_ASSERT_TRUE_MESSAGE(t >= T5UI_T_INT && t <= T5UI_T_OBJ, "a property without a type");
        if (info & T5UI_F_LOOK) TEST_ASSERT_TRUE(t == T5UI_T_INT || t == T5UI_T_COLOR);
    }
    TEST_ASSERT_EQUAL(0, t5ui_prop_info(0));
    TEST_ASSERT_EQUAL(0, t5ui_prop_info((t5ui_prop_t)12345));
    TEST_ASSERT_EQUAL(T5UI_T_STR, t5ui_prop_info(T5UI_P_TEXT));
    TEST_ASSERT_EQUAL(T5UI_T_COLOR | T5UI_F_LOOK, t5ui_prop_info(T5UI_P_BG_COLOR));
    TEST_ASSERT_EQUAL(T5UI_T_OBJ, t5ui_prop_info(T5UI_P_TARGET));
    TEST_ASSERT_TRUE(n > 100);
}

/* ---- the core's checks ------------------------------------------------------------------ */
static void test_set_types(void)
{
    TEST_ASSERT_EQUAL(T5_OK, t5ui_set_int(OBJ, T5UI_P_WIDTH, 10));
    TEST_ASSERT_EQUAL(T5UI_T_INT, be.val.type);
    TEST_ASSERT_EQUAL(10, be.val.i);
    TEST_ASSERT_EQUAL(T5_OK, t5ui_set_color(OBJ, T5UI_P_BG_COLOR, 0xaa112233));
    TEST_ASSERT_EQUAL_HEX32(0x112233, be.val.c);              /* only RGB reaches the backend */
    TEST_ASSERT_EQUAL(T5_OK, t5ui_set_str(OBJ, T5UI_P_TEXT, NULL));
    TEST_ASSERT_EQUAL_STRING("", be.val.s);                   /* never NULL */
    TEST_ASSERT_EQUAL(T5_OK, t5ui_set_strf(OBJ, T5UI_P_TEXT, "%d%%", 50));
    TEST_ASSERT_EQUAL_STRING("50%", be.val.s);
    TEST_ASSERT_EQUAL(T5_OK, t5ui_set_obj(OBJ, T5UI_P_TARGET, NULL));
    TEST_ASSERT_EQUAL(5, be.sets);

    /* The wrong type, or no such property: the backend never sees it. */
    TEST_ASSERT_EQUAL(T5_ERR_NOT_SUPPORTED, t5ui_set_int(OBJ, T5UI_P_TEXT, 1));
    TEST_ASSERT_EQUAL(T5_ERR_NOT_SUPPORTED, t5ui_set_str(OBJ, T5UI_P_WIDTH, "1"));
    TEST_ASSERT_EQUAL(T5_ERR_NOT_SUPPORTED, t5ui_set_color(OBJ, T5UI_P_OPA, 0));
    TEST_ASSERT_EQUAL(T5_ERR_NOT_SUPPORTED, t5ui_set_int(OBJ, (t5ui_prop_t)999, 1));
    TEST_ASSERT_EQUAL(T5_ERR_NOT_SUPPORTED, t5ui_set_int(OBJ, (t5ui_prop_t)-1, 1));
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5ui_set_int(NULL, T5UI_P_WIDTH, 1));
    TEST_ASSERT_EQUAL(5, be.sets);
}

static void test_set_at(void)
{
    uint32_t at = T5UI_AT(T5UI_PART_KNOB, T5UI_STATE_PRESSED);
    TEST_ASSERT_EQUAL(T5_OK, t5ui_set_color_at(OBJ, T5UI_P_BG_COLOR, at, 0x00ff00));
    TEST_ASSERT_EQUAL_HEX32(at, be.at);
    /* A widget property has no parts or states. */
    TEST_ASSERT_EQUAL(T5_ERR_NOT_SUPPORTED, t5ui_set_int_at(OBJ, T5UI_P_VALUE, at, 1));
    /* at 0 is the plain setter, for any property. */
    TEST_ASSERT_EQUAL(T5_OK, t5ui_set_int_at(OBJ, T5UI_P_VALUE, 0, 1));
    TEST_ASSERT_EQUAL(0, be.at);
    /* Unknown parts and states are refused. */
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5ui_set_int_at(OBJ, T5UI_P_OPA, T5UI_AT(99, 0), 1));
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5ui_set_int_at(OBJ, T5UI_P_OPA, T5UI_AT(0, 0x100), 1));
    TEST_ASSERT_EQUAL(2, be.sets);
}

static void test_get(void)
{
    TEST_ASSERT_EQUAL(42, t5ui_get_int(OBJ, T5UI_P_VALUE, -1));
    TEST_ASSERT_EQUAL_HEX32(0x123456, t5ui_get_color(OBJ, T5UI_P_BG_COLOR, 0));
    TEST_ASSERT_EQUAL_STRING("hello", t5ui_get_str(OBJ, T5UI_P_TEXT, "def"));
    TEST_ASSERT_EQUAL(-1, t5ui_get_int(OBJ, T5UI_P_MIN, -1));          /* the backend has none */
    TEST_ASSERT_EQUAL(4, be.gets);
    /* The wrong type, a set-only property, no object: def, backend untouched. */
    TEST_ASSERT_EQUAL_STRING("def", t5ui_get_str(OBJ, T5UI_P_VALUE, "def"));
    TEST_ASSERT_EQUAL(7, t5ui_get_int(OBJ, T5UI_P_PAD_ALL, 7));
    TEST_ASSERT_EQUAL(7, t5ui_get_int(NULL, T5UI_P_VALUE, 7));
    TEST_ASSERT_NULL(t5ui_get_obj(OBJ, T5UI_P_TEXT));
    TEST_ASSERT_EQUAL(4, be.gets);
}

static void test_create(void)
{
    TEST_ASSERT_EQUAL_PTR(OBJ, t5ui_create(OBJ, T5UI_BOX));
    TEST_ASSERT_EQUAL_PTR(OBJ, t5ui_create(OBJ, T5UI_MD_LIST));
    TEST_ASSERT_NULL(t5ui_create(OBJ, (t5ui_kind_t)0));
    TEST_ASSERT_NULL(t5ui_create(OBJ, (t5ui_kind_t)20));
    TEST_ASSERT_NULL(t5ui_create(OBJ, (t5ui_kind_t)99));
    TEST_ASSERT_NULL(t5ui_create(OBJ, (t5ui_kind_t)127));
    TEST_ASSERT_NULL(t5ui_create(NULL, T5UI_BOX));
    TEST_ASSERT_EQUAL(2, be.creates);
}

static void test_styles_anims_events(void)
{
    TEST_ASSERT_EQUAL(T5_OK, t5ui_style_set_int(STYLE, T5UI_P_RADIUS, 4));
    TEST_ASSERT_EQUAL(T5_ERR_NOT_SUPPORTED, t5ui_style_set_int(STYLE, T5UI_P_VALUE, 4));   /* not a look */
    TEST_ASSERT_EQUAL(T5_ERR_NOT_SUPPORTED, t5ui_style_set_color(STYLE, T5UI_P_RADIUS, 4));
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5ui_style_set_int(NULL, T5UI_P_RADIUS, 4));
    TEST_ASSERT_EQUAL(1, be.styles);
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5ui_style_add(OBJ, STYLE, T5UI_AT(42, 0)));

    TEST_ASSERT_EQUAL(T5_OK, t5ui_animate(OBJ, T5UI_P_X, 0, 100, 200, 0, T5UI_EASE_OUT));
    TEST_ASSERT_EQUAL(T5_ERR_NOT_SUPPORTED, t5ui_animate(OBJ, T5UI_P_BG_COLOR, 0, 1, 200, 0, T5UI_EASE_OUT));
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5ui_animate(OBJ, T5UI_P_X, 0, 1, 200, 0, (t5ui_ease_t)9));
    TEST_ASSERT_EQUAL(1, be.anims);

    TEST_ASSERT_EQUAL(T5_OK, t5ui_on(OBJ, T5UI_EV_CLICK, cb, NULL));
    TEST_ASSERT_EQUAL(T5_ERR_NOT_SUPPORTED, t5ui_on(OBJ, (t5ui_ev_t)17, cb, NULL));
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5ui_on(OBJ, T5UI_EV_CLICK, NULL, NULL));
    TEST_ASSERT_EQUAL(1, be.ons);

    TEST_ASSERT_EQUAL(7, t5ui_cmd(OBJ, T5UI_CMD_SCROLL_TO, 7, 0, 0, NULL));
    TEST_ASSERT_EQUAL(T5_ERR_NOT_SUPPORTED, t5ui_cmd(OBJ, (t5ui_cmd_t)0, 0, 0, 0, NULL));
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5ui_cmd(NULL, T5UI_CMD_FOCUS, 0, 0, 0, NULL));
    TEST_ASSERT_EQUAL(1, be.cmds);
}

static void test_image_pixels(void)
{
    static const uint16_t px[4] = { 0 };
    t5ui_pixels_t p = { .size = sizeof p, .w = 2, .h = 2, .format = T5UI_PX_RGB565, .data = px };
    TEST_ASSERT_EQUAL(T5_OK, t5ui_image_pixels(OBJ, &p));
    t5ui_pixels_t bad = p;
    bad.size = 4;                              /* an app built against something older */
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5ui_image_pixels(OBJ, &bad));
    bad = p;
    bad.format = 9;
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5ui_image_pixels(OBJ, &bad));
    bad = p;
    bad.data = NULL;
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5ui_image_pixels(OBJ, &bad));
    TEST_ASSERT_EQUAL(1, be.images);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_encodings);
    RUN_TEST(test_numbers_pinned);
    RUN_TEST(test_prop_table);
    RUN_TEST(test_set_types);
    RUN_TEST(test_set_at);
    RUN_TEST(test_get);
    RUN_TEST(test_create);
    RUN_TEST(test_styles_anims_events);
    RUN_TEST(test_image_pixels);
    return UNITY_END();
}
