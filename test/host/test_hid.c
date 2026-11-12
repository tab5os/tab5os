/* SPDX-License-Identifier: Apache-2.0
 * t5_hid: descriptors (parsed back to check report sizes against the pack
 * functions), report packing, keyboard state and text to keys.
 */
#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "t5_hid/hid.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- a small HID descriptor walker ---------------------------------------- */
typedef struct {
    int in_bits[8], out_bits[8];   /* by report ID (0 = none) */
    int depth, max_depth;
    bool ids_seen, bad;
} walk_t;

static void walk(const uint8_t *d, int n, walk_t *w)
{
    memset(w, 0, sizeof *w);
    int size = 0, count = 0, id = 0;
    for (int i = 0; i < n;) {
        uint8_t b = d[i];
        int len = b & 3;
        if (len == 3) len = 4;
        if (i + 1 + len > n) {
            w->bad = true;
            return;
        }
        uint32_t v = 0;
        for (int k = 0; k < len; k++) v |= (uint32_t)d[i + 1 + k] << (8 * k);
        switch (b & 0xfc) {
        case 0x74: size = (int)v; break;            /* report size */
        case 0x94: count = (int)v; break;           /* report count */
        case 0x84:                                  /* report ID */
            id = (int)v;
            w->ids_seen = true;
            if (id <= 0 || id >= 8) w->bad = true;
            break;
        case 0x80: w->in_bits[id] += size * count; break;
        case 0x90: w->out_bits[id] += size * count; break;
        case 0xa0:
            if (++w->depth > w->max_depth) w->max_depth = w->depth;
            break;
        case 0xc0:
            if (--w->depth < 0) w->bad = true;
            break;
        default: break;
        }
        i += 1 + len;
    }
    if (w->depth != 0) w->bad = true;
}

static void test_each_part_matches_its_pack_length(void)
{
    static const struct { uint32_t part; int id, len; } P[] = {
        { T5_HID_PART_KEYBOARD, T5_HID_ID_KEYBOARD, T5_HID_KEYBOARD_LEN },
        { T5_HID_PART_MOUSE, T5_HID_ID_MOUSE, T5_HID_MOUSE_LEN },
        { T5_HID_PART_CONSUMER, T5_HID_ID_CONSUMER, T5_HID_CONSUMER_LEN },
        { T5_HID_PART_GAMEPAD, T5_HID_ID_GAMEPAD, T5_HID_GAMEPAD_LEN },
        { T5_HID_PART_ABSOLUTE, T5_HID_ID_ABSOLUTE, T5_HID_ABSOLUTE_LEN },
    };
    uint8_t d[T5_HID_DESC_MAX];
    walk_t w;
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++) {
        int n = t5_hid_descriptor(P[i].part | T5_HID_NO_IDS, d, sizeof d);
        TEST_ASSERT_GREATER_THAN(0, n);
        walk(d, n, &w);
        TEST_ASSERT_FALSE(w.bad);
        TEST_ASSERT_FALSE(w.ids_seen);
        TEST_ASSERT_EQUAL(P[i].len * 8, w.in_bits[0]);
        TEST_ASSERT_EQUAL(0, t5_hid_report_id(P[i].part | T5_HID_NO_IDS, P[i].part));

        n = t5_hid_descriptor(P[i].part, d, sizeof d);
        walk(d, n, &w);
        TEST_ASSERT_FALSE(w.bad);
        TEST_ASSERT_EQUAL(P[i].len * 8, w.in_bits[P[i].id]);
        TEST_ASSERT_EQUAL(P[i].id, t5_hid_report_id(P[i].part, P[i].part));
    }
}

static void test_all_parts_together(void)
{
    uint8_t d[T5_HID_DESC_MAX];
    int n = t5_hid_descriptor(T5_HID_PART_ALL, d, sizeof d);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_LESS_OR_EQUAL(T5_HID_DESC_MAX, n);
    walk_t w;
    walk(d, n, &w);
    TEST_ASSERT_FALSE(w.bad);
    TEST_ASSERT_EQUAL(0, w.in_bits[0]);            /* every report has its ID */
    TEST_ASSERT_EQUAL(T5_HID_KEYBOARD_LEN * 8, w.in_bits[T5_HID_ID_KEYBOARD]);
    TEST_ASSERT_EQUAL(T5_HID_KEYBOARD_OUT_LEN * 8, w.out_bits[T5_HID_ID_KEYBOARD]);
    TEST_ASSERT_EQUAL(T5_HID_MOUSE_LEN * 8, w.in_bits[T5_HID_ID_MOUSE]);
    TEST_ASSERT_EQUAL(T5_HID_GAMEPAD_LEN * 8, w.in_bits[T5_HID_ID_GAMEPAD]);
    /* exactly the right size is enough, one less isn't */
    TEST_ASSERT_EQUAL(n, t5_hid_descriptor(T5_HID_PART_ALL, d, (size_t)n));
    TEST_ASSERT_EQUAL(T5_ERR_NO_MEM, t5_hid_descriptor(T5_HID_PART_ALL, d, (size_t)n - 1));
}

static void test_bad_parts(void)
{
    uint8_t d[T5_HID_DESC_MAX];
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_hid_descriptor(0, d, sizeof d));
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_hid_descriptor(0x20, d, sizeof d));
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG,
                      t5_hid_descriptor(T5_HID_PART_KEYBOARD | T5_HID_PART_MOUSE | T5_HID_NO_IDS, d, sizeof d));
    TEST_ASSERT_EQUAL(-1, t5_hid_report_id(T5_HID_PART_KEYBOARD, T5_HID_PART_MOUSE));
    TEST_ASSERT_EQUAL(-1, t5_hid_report_id(T5_HID_PART_ALL, T5_HID_PART_KEYBOARD | T5_HID_PART_MOUSE));
}

static void test_packing(void)
{
    uint8_t o[16];
    t5_hid_mouse_t m = { .buttons = 0xff, .dx = -32768, .dy = 300, .wheel = -128, .pan = 5 };
    t5_hid_pack_mouse(&m, o);
    const uint8_t want_m[] = { 0x1f, 0x01, 0x80, 0x2c, 0x01, 0x81, 0x05 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want_m, o, sizeof want_m);

    t5_hid_gamepad_t g = { .buttons = 0x8001, .lx = -128, .ly = 127, .rx = 0, .ry = -1, .lt = 255, .rt = 0, .hat = 9 };
    t5_hid_pack_gamepad(&g, o);
    const uint8_t want_g[] = { 0x01, 0x80, 0x81, 0x7f, 0x00, 0xff, 0xff, 0x00, T5_HID_HAT_NONE };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want_g, o, sizeof want_g);

    t5_hid_absolute_t a = { .buttons = 0xff, .x = 40000, .y = 1 };
    t5_hid_pack_absolute(&a, o);
    const uint8_t want_a[] = { 0x07, 0xff, 0x7f, 0x01, 0x00 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want_a, o, sizeof want_a);

    t5_hid_pack_consumer(T5_HID_CC_VOL_UP, o);
    TEST_ASSERT_EQUAL_HEX8(0xe9, o[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, o[1]);
}

static void test_keyboard_state(void)
{
    t5_hid_keyboard_t k = { 0 };
    TEST_ASSERT_EQUAL(T5_OK, t5_hid_key_press(&k, T5_HID_KEY_LEFT_CTRL));
    TEST_ASSERT_EQUAL_HEX8(T5_HID_MOD_LCTRL, k.mods);
    for (uint8_t i = 0; i < 6; i++) TEST_ASSERT_EQUAL(T5_OK, t5_hid_key_press(&k, (uint8_t)(T5_HID_KEY_A + i)));
    TEST_ASSERT_EQUAL(T5_OK, t5_hid_key_press(&k, T5_HID_KEY_A));         /* already down */
    TEST_ASSERT_EQUAL(T5_ERR_NO_MEM, t5_hid_key_press(&k, T5_HID_KEY_ENTER));
    t5_hid_key_release(&k, T5_HID_KEY_A + 2);
    TEST_ASSERT_EQUAL(T5_OK, t5_hid_key_press(&k, T5_HID_KEY_ENTER));
    TEST_ASSERT_EQUAL_HEX8(T5_HID_KEY_ENTER, k.keys[2]);                  /* took the freed slot */
    t5_hid_key_release(&k, T5_HID_KEY_LEFT_CTRL);
    uint8_t o[8];
    t5_hid_pack_keyboard(&k, o);
    TEST_ASSERT_EQUAL_HEX8(0, o[0]);
    TEST_ASSERT_EQUAL_HEX8(0, o[1]);
    TEST_ASSERT_EQUAL_HEX8(T5_HID_KEY_A, o[2]);
}

static void expect(t5_hid_layout_t l, uint32_t ch, uint8_t key, uint8_t mods)
{
    uint8_t k = 0, m = 0xff;
    TEST_ASSERT_EQUAL_MESSAGE(T5_OK, t5_hid_char_key(ch, l, &k, &m), "char not typable");
    TEST_ASSERT_EQUAL_HEX8(key, k);
    TEST_ASSERT_EQUAL_HEX8(mods, m);
}

static void test_layouts(void)
{
    const uint8_t S = T5_HID_MOD_LSHIFT, G = T5_HID_MOD_RALT;
    expect(T5_HID_LAYOUT_US, 'a', 0x04, 0);
    expect(T5_HID_LAYOUT_US, 'Z', 0x1d, S);
    expect(T5_HID_LAYOUT_US, '@', 0x1f, S);
    expect(T5_HID_LAYOUT_US, '\n', T5_HID_KEY_ENTER, 0);
    expect(T5_HID_LAYOUT_UK, '"', 0x1f, S);
    expect(T5_HID_LAYOUT_UK, '@', 0x34, S);
    expect(T5_HID_LAYOUT_UK, 0x00a3, 0x20, S);    /* £ */
    expect(T5_HID_LAYOUT_UK, 0x20ac, 0x21, G);    /* € */
    expect(T5_HID_LAYOUT_UK, '#', 0x32, 0);
    expect(T5_HID_LAYOUT_DE, 'z', 0x1c, 0);
    expect(T5_HID_LAYOUT_DE, 'y', 0x1d, 0);
    expect(T5_HID_LAYOUT_DE, 0x00df, 0x2d, 0);    /* ß */
    expect(T5_HID_LAYOUT_DE, 0x00c4, 0x34, S);    /* Ä */
    expect(T5_HID_LAYOUT_DE, '@', 0x14, G);
    expect(T5_HID_LAYOUT_DE, '{', 0x24, G);
    expect(T5_HID_LAYOUT_DE, '-', 0x38, 0);
    expect(T5_HID_LAYOUT_FR, 'a', 0x14, 0);
    expect(T5_HID_LAYOUT_FR, 'q', 0x04, 0);
    expect(T5_HID_LAYOUT_FR, 'm', 0x33, 0);
    expect(T5_HID_LAYOUT_FR, 'W', 0x1d, S);
    expect(T5_HID_LAYOUT_FR, '1', 0x1e, S);
    expect(T5_HID_LAYOUT_FR, 0x00e9, 0x1f, 0);    /* é */
    expect(T5_HID_LAYOUT_FR, ',', 0x10, 0);
    expect(T5_HID_LAYOUT_FR, '.', 0x36, S);
    expect(T5_HID_LAYOUT_FR, '@', 0x27, G);

    uint8_t k, m;
    TEST_ASSERT_EQUAL(T5_ERR_NOT_FOUND, t5_hid_char_key('^', T5_HID_LAYOUT_DE, &k, &m));   /* dead key */
    TEST_ASSERT_EQUAL(T5_ERR_NOT_FOUND, t5_hid_char_key(0x00e9, T5_HID_LAYOUT_US, &k, &m));
    TEST_ASSERT_EQUAL(T5_ERR_NOT_FOUND, t5_hid_char_key(1, T5_HID_LAYOUT_US, &k, &m));
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_hid_char_key('a', T5_HID_LAYOUT_COUNT, &k, &m));
}

/* Every printable ASCII character is typable on US, and no two characters
 * share a stroke on any layout (a wrong table entry usually shows as one). */
static void test_no_two_chars_share_a_stroke(void)
{
    for (int l = 0; l < T5_HID_LAYOUT_COUNT; l++) {
        uint32_t seen[256][4] = { { 0 } };     /* by key: plain, shift, altgr */
        static const uint32_t extra[] = { 0xa3, 0xa7, 0xb0, 0xb2, 0xb3, 0xb5, 0xa4, 0xac, 0xc4, 0xd6, 0xdc,
                                          0xdf, 0xe0, 0xe4, 0xe7, 0xe8, 0xe9, 0xf6, 0xf9, 0xfc, 0x20ac };
        for (uint32_t ch = 0x20; ch < 0x7f + sizeof extra / sizeof extra[0]; ch++) {
            uint32_t c = ch < 0x7f ? ch : extra[ch - 0x7f];
            uint8_t k, m;
            if (t5_hid_char_key(c, (t5_hid_layout_t)l, &k, &m) != T5_OK) {
                if (l == T5_HID_LAYOUT_US && c < 0x7f) TEST_FAIL_MESSAGE("US can't type printable ASCII");
                continue;
            }
            int slot = m == 0 ? 0 : m == T5_HID_MOD_LSHIFT ? 1 : m == T5_HID_MOD_RALT ? 2 : 3;
            TEST_ASSERT_LESS_THAN(3, slot);
            if (seen[k][slot]) {
                char msg[64];
                snprintf(msg, sizeof msg, "layout %d: U+%04X and U+%04X share a stroke", l, (unsigned)seen[k][slot],
                         (unsigned)c);
                TEST_FAIL_MESSAGE(msg);
            }
            seen[k][slot] = c;
        }
    }
}

typedef struct {
    uint8_t keys[32], mods[32];
    int n, stop_at;
} strokes_t;

static int collect(uint8_t key, uint8_t mods, void *arg)
{
    strokes_t *s = arg;
    if (s->n == s->stop_at) return -1;
    s->keys[s->n] = key;
    s->mods[s->n++] = mods;
    return 0;
}

static void test_text(void)
{
    strokes_t s = { .stop_at = -1 };
    TEST_ASSERT_EQUAL(4, t5_hid_text("Hi!\n", T5_HID_LAYOUT_US, collect, &s));
    TEST_ASSERT_EQUAL(4, s.n);
    TEST_ASSERT_EQUAL_HEX8(0x0b, s.keys[0]);
    TEST_ASSERT_EQUAL_HEX8(T5_HID_MOD_LSHIFT, s.mods[0]);
    TEST_ASSERT_EQUAL_HEX8(0x1e, s.keys[2]);
    TEST_ASSERT_EQUAL_HEX8(T5_HID_KEY_ENTER, s.keys[3]);

    /* "Grüße" on DE: ü and ß are two bytes each */
    s = (strokes_t){ .stop_at = -1 };
    const char *gr = "Gr\xc3\xbc\xc3\x9f" "e";
    TEST_ASSERT_EQUAL((int)strlen(gr), t5_hid_text(gr, T5_HID_LAYOUT_DE, collect, &s));
    TEST_ASSERT_EQUAL(5, s.n);
    TEST_ASSERT_EQUAL_HEX8(0x2f, s.keys[2]);

    /* stops at what US can't type, and says where */
    s = (strokes_t){ .stop_at = -1 };
    TEST_ASSERT_EQUAL(2, t5_hid_text("ab\xc3\xa9" "c", T5_HID_LAYOUT_US, collect, &s));
    TEST_ASSERT_EQUAL(2, s.n);
    /* ...at invalid UTF-8 */
    s = (strokes_t){ .stop_at = -1 };
    TEST_ASSERT_EQUAL(1, t5_hid_text("a\xc3", T5_HID_LAYOUT_US, collect, &s));
    TEST_ASSERT_EQUAL(1, t5_hid_text("a\xc0\xaf", T5_HID_LAYOUT_US, collect, &s));   /* overlong */
    /* ...and when the callback says so */
    s = (strokes_t){ .stop_at = 1 };
    TEST_ASSERT_EQUAL(1, t5_hid_text("abc", T5_HID_LAYOUT_US, collect, &s));
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_hid_text(NULL, T5_HID_LAYOUT_US, collect, &s));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_each_part_matches_its_pack_length);
    RUN_TEST(test_all_parts_together);
    RUN_TEST(test_bad_parts);
    RUN_TEST(test_packing);
    RUN_TEST(test_keyboard_state);
    RUN_TEST(test_layouts);
    RUN_TEST(test_no_two_chars_share_a_stroke);
    RUN_TEST(test_text);
    return UNITY_END();
}
