/* SPDX-License-Identifier: Apache-2.0
 * Keyboard state and text to key strokes (t5_hid/hid.h). Layouts follow the
 * Windows and Linux versions; macOS puts some AltGr (Option) characters on
 * other keys for German and French. Dead keys (´ ` ^ ¨ ~ where they are
 * dead) aren't used, so those characters can't be typed on that layout.
 */
#include <string.h>
#include "t5_hid/hid.h"

t5_err_t t5_hid_key_press(t5_hid_keyboard_t *k, uint8_t key)
{
    if (key >= 0xe0 && key <= 0xe7) {
        k->mods |= (uint8_t)(1u << (key - 0xe0));
        return T5_OK;
    }
    if (!key) return T5_ERR_INVALID_ARG;
    int free_slot = -1;
    for (int i = 0; i < 6; i++) {
        if (k->keys[i] == key) return T5_OK;
        if (!k->keys[i] && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return T5_ERR_NO_MEM;
    k->keys[free_slot] = key;
    return T5_OK;
}

void t5_hid_key_release(t5_hid_keyboard_t *k, uint8_t key)
{
    if (key >= 0xe0 && key <= 0xe7) {
        k->mods &= (uint8_t)~(1u << (key - 0xe0));
        return;
    }
    for (int i = 0; i < 6; i++) {
        if (k->keys[i] == key) k->keys[i] = 0;
    }
}

/* ---- layouts ---------------------------------------------------------------- */
#define NONE u'\x01'
#define S T5_HID_MOD_LSHIFT
#define G T5_HID_MOD_RALT

typedef struct {
    uint_least16_t ch;
    uint8_t key, mods;
} extra_t;

typedef struct {
    const uint8_t *keys;      /* the non-letter keys, with what they type below */
    const uint_least16_t *plain, *shift, *altgr;   /* one char per key, NONE for nothing */
    uint8_t n;
    uint8_t letters[26];      /* the key for a..z */
    const extra_t *extras;    /* AltGr on letter keys */
    uint8_t n_extras;
} layout_t;

#define DIGIT_KEYS 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27
#define QWERTY { 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, \
                 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d }

static const uint8_t US_KEYS[] = { DIGIT_KEYS, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38 };
static const uint_least16_t US_PLAIN[] = u"1234567890-=[]\\;'`,./";
static const uint_least16_t US_SHIFT[] = u"!@#$%^&*()_+{}|:\"~<>?";

static const uint8_t UK_KEYS[] = { DIGIT_KEYS, 0x2d, 0x2e, 0x2f, 0x30, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x64 };
static const uint_least16_t UK_PLAIN[] = u"1234567890-=[]#;'`,./\\";
static const uint_least16_t UK_SHIFT[] = u"!\"£$%^&*()_+{}~:@¬<>?|";
static const uint_least16_t UK_ALTGR[] = { NONE, NONE, NONE, u'€', NONE, NONE, NONE, NONE, NONE, NONE, NONE,
                                     NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, 0 };

static const uint8_t DE_KEYS[] = { DIGIT_KEYS, 0x2d, 0x2e, 0x2f, 0x30, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x64 };
static const uint_least16_t DE_PLAIN[] = { u'1', u'2', u'3', u'4', u'5', u'6', u'7', u'8', u'9', u'0',
                                     u'ß', NONE, u'ü', u'+', u'#', u'ö', u'ä', NONE, u',', u'.', u'-', u'<', 0 };
static const uint_least16_t DE_SHIFT[] = { u'!', u'"', u'§', u'$', u'%', u'&', u'/', u'(', u')', u'=',
                                     u'?', NONE, u'Ü', u'*', u'\'', u'Ö', u'Ä', u'°', u';', u':', u'_', u'>', 0 };
static const uint_least16_t DE_ALTGR[] = { NONE, u'²', u'³', NONE, NONE, NONE, u'{', u'[', u']', u'}',
                                     u'\\', NONE, NONE, u'~', NONE, NONE, NONE, NONE, NONE, NONE, NONE, u'|', 0 };
static const extra_t DE_EXTRAS[] = { { u'@', 0x14, G }, { u'€', 0x08, G }, { u'µ', 0x10, G } };

/* AZERTY: the number row types symbols, digits need shift; M is right of L,
 * and the M position (0x10) types , and ?. */
static const uint8_t FR_KEYS[] = { DIGIT_KEYS, 0x2d, 0x2e, 0x2f, 0x30, 0x32, 0x34, 0x35, 0x10, 0x36, 0x37, 0x38, 0x64 };
static const uint_least16_t FR_PLAIN[] = { u'&', u'é', u'"', u'\'', u'(', u'-', u'è', u'_', u'ç', u'à',
                                     u')', u'=', NONE, u'$', u'*', u'ù', u'²', u',', u';', u':', u'!', u'<', 0 };
static const uint_least16_t FR_SHIFT[] = { u'1', u'2', u'3', u'4', u'5', u'6', u'7', u'8', u'9', u'0',
                                     u'°', u'+', NONE, u'£', u'µ', u'%', NONE, u'?', u'.', u'/', u'§', u'>', 0 };
static const uint_least16_t FR_ALTGR[] = { NONE, NONE, u'#', u'{', u'[', u'|', NONE, u'\\', NONE, u'@',
                                     u']', u'}', NONE, u'¤', NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE, 0 };
static const extra_t FR_EXTRAS[] = { { u'€', 0x08, G } };

#define N(a) (uint8_t)(sizeof a / sizeof a[0])
#define CHECK(k, s) _Static_assert(sizeof k == sizeof s / sizeof s[0] - 1, #s " matches " #k)
CHECK(US_KEYS, US_PLAIN);
CHECK(US_KEYS, US_SHIFT);
CHECK(UK_KEYS, UK_PLAIN);
CHECK(UK_KEYS, UK_SHIFT);
CHECK(UK_KEYS, UK_ALTGR);
CHECK(DE_KEYS, DE_PLAIN);
CHECK(DE_KEYS, DE_SHIFT);
CHECK(DE_KEYS, DE_ALTGR);
CHECK(FR_KEYS, FR_PLAIN);
CHECK(FR_KEYS, FR_SHIFT);
CHECK(FR_KEYS, FR_ALTGR);

static const layout_t LAYOUTS[T5_HID_LAYOUT_COUNT] = {
    [T5_HID_LAYOUT_US] = { US_KEYS, US_PLAIN, US_SHIFT, NULL, N(US_KEYS), QWERTY, NULL, 0 },
    [T5_HID_LAYOUT_UK] = { UK_KEYS, UK_PLAIN, UK_SHIFT, UK_ALTGR, N(UK_KEYS), QWERTY, NULL, 0 },
    [T5_HID_LAYOUT_DE] = { DE_KEYS, DE_PLAIN, DE_SHIFT, DE_ALTGR, N(DE_KEYS),
                           { 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
                             0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1d, 0x1c },
                           DE_EXTRAS, N(DE_EXTRAS) },
    [T5_HID_LAYOUT_FR] = { FR_KEYS, FR_PLAIN, FR_SHIFT, FR_ALTGR, N(FR_KEYS),
                           /* a=Q-pos, m=;-pos, q=A-pos, w=Z-pos, z=W-pos */
                           { 0x14, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x33,
                             0x11, 0x12, 0x13, 0x04, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1d, 0x1b, 0x1c, 0x1a },
                           FR_EXTRAS, N(FR_EXTRAS) },
};

static bool find_in(const uint_least16_t *row, const uint8_t *keys, uint8_t n, uint32_t ch, uint8_t *key)
{
    if (!row) return false;
    for (uint8_t i = 0; i < n; i++) {
        if (row[i] != NONE && row[i] == ch) {
            *key = keys[i];
            return true;
        }
    }
    return false;
}

t5_err_t t5_hid_char_key(uint32_t ch, t5_hid_layout_t layout, uint8_t *key, uint8_t *mods)
{
    if ((unsigned)layout >= T5_HID_LAYOUT_COUNT || !key || !mods) return T5_ERR_INVALID_ARG;
    const layout_t *l = &LAYOUTS[layout];
    *mods = 0;
    switch (ch) {
    case '\n': *key = T5_HID_KEY_ENTER; return T5_OK;
    case '\t': *key = T5_HID_KEY_TAB; return T5_OK;
    case '\b': *key = T5_HID_KEY_BACKSPACE; return T5_OK;
    case ' ': *key = T5_HID_KEY_SPACE; return T5_OK;
    default: break;
    }
    if (ch >= 'a' && ch <= 'z') {
        *key = l->letters[ch - 'a'];
        return T5_OK;
    }
    if (ch >= 'A' && ch <= 'Z') {
        *key = l->letters[ch - 'A'];
        *mods = S;
        return T5_OK;
    }
    if (ch == NONE || ch > 0xffff) return T5_ERR_NOT_FOUND;
    if (find_in(l->plain, l->keys, l->n, ch, key)) return T5_OK;
    if (find_in(l->shift, l->keys, l->n, ch, key)) {
        *mods = S;
        return T5_OK;
    }
    if (find_in(l->altgr, l->keys, l->n, ch, key)) {
        *mods = G;
        return T5_OK;
    }
    for (uint8_t i = 0; i < l->n_extras; i++) {
        if (l->extras[i].ch == ch) {
            *key = l->extras[i].key;
            *mods = l->extras[i].mods;
            return T5_OK;
        }
    }
    return T5_ERR_NOT_FOUND;
}

/* One UTF-8 character: its length, 0 for an invalid sequence. */
static int utf8_next(const unsigned char *p, uint32_t *ch)
{
    if (p[0] < 0x80) {
        *ch = p[0];
        return 1;
    }
    int n = (p[0] & 0xe0) == 0xc0 ? 2 : (p[0] & 0xf0) == 0xe0 ? 3 : (p[0] & 0xf8) == 0xf0 ? 4 : 0;
    if (!n) return 0;
    uint32_t c = p[0] & (0x7f >> n);
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xc0) != 0x80) return 0;
        c = c << 6 | (p[i] & 0x3f);
    }
    static const uint32_t MIN[5] = { 0, 0, 0x80, 0x800, 0x10000 };
    if (c < MIN[n] || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return 0;
    *ch = c;
    return n;
}

int t5_hid_text(const char *text, t5_hid_layout_t layout, t5_hid_stroke_cb_t cb, void *arg)
{
    if (!text || !cb || (unsigned)layout >= T5_HID_LAYOUT_COUNT) return T5_ERR_INVALID_ARG;
    const unsigned char *p = (const unsigned char *)text;
    int done = 0;
    while (p[done]) {
        uint32_t ch;
        int n = utf8_next(p + done, &ch);
        uint8_t key, mods;
        if (!n || t5_hid_char_key(ch, layout, &key, &mods) != T5_OK) break;
        if (cb(key, mods, arg) < 0) break;
        done += n;
    }
    return done;
}
