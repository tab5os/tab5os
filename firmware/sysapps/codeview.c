/* SPDX-License-Identifier: Apache-2.0
 * Code view: see codeview.h.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "md/md.h"
#include "t5_shell/shell.h"
#include "t5_ui/t5_ui.h"
#include "codeview.h"

#define INDENT 4
#define GUTTER 4                 /* line number columns */
#define LINE_BUF 1024            /* markup for one drawn line */
#define SENTINEL "  "            /* what the hidden text area holds between keys */

/* Colours on the dark theme. */
#define C_KEYWORD 0xc792ea
#define C_BUILTIN 0x82aaff
#define C_STRING 0xc3e88d
#define C_COMMENT 0x7f8c98
#define C_NUMBER 0xf78c6c
#define C_CONST 0xff9cac
#define C_DEF 0xffcb6b
#define C_GUTTER 0x5c6370

struct cv {
    lv_obj_t *scroll, *content, *ta;
    char **lines;
    int n, cap;
    int row, col, want_col;
    int maxlen;
    int32_t cw, lh;
    bool dirty, cursor_on, resetting;
    uint8_t *state;              /* triple-quote state at each line start: 0, '\'' or '"' */
    bool state_stale;
    lv_timer_t *blink;
    void (*changed)(cv_t *, void *);
    void *changed_arg;
};

/* ---- buffer --------------------------------------------------------------------------- */
static bool ensure_cap(cv_t *cv, int need)
{
    if (need <= cv->cap) return true;
    int cap = cv->cap ? cv->cap : 64;
    while (cap < need) cap *= 2;
    char **l = realloc(cv->lines, sizeof(char *) * (size_t)cap);
    uint8_t *st = realloc(cv->state, (size_t)cap);
    if (!l || !st) {
        if (l) cv->lines = l;
        if (st) cv->state = st;
        return false;
    }
    cv->lines = l;
    cv->state = st;
    cv->cap = cap;
    return true;
}

static void free_lines(cv_t *cv)
{
    for (int i = 0; i < cv->n; i++) free(cv->lines[i]);
    cv->n = 0;
}

static int line_len(const cv_t *cv, int r)
{
    return (int)strlen(cv->lines[r]);
}

/* Display columns up to byte offset (UTF-8 aware). */
static int disp_col(const char *s, int bytes)
{
    int c = 0;
    for (int i = 0; i < bytes && s[i]; i++) {
        if (((unsigned char)s[i] & 0xc0) != 0x80) c++;
    }
    return c;
}

/* Byte offset of display column dc. */
static int byte_col(const char *s, int dc)
{
    int c = 0, i = 0;
    for (; s[i]; i++) {
        if (((unsigned char)s[i] & 0xc0) != 0x80) {
            if (c == dc) return i;
            c++;
        }
    }
    return i;
}

static void recalc_max(cv_t *cv)
{
    int m = 0;
    for (int i = 0; i < cv->n; i++) {
        int l = disp_col(cv->lines[i], line_len(cv, i));
        if (l > m) m = l;
    }
    cv->maxlen = m;
}

/* ---- Python colouring ------------------------------------------------------------------- */
static const char *const s_keywords[] = { "and", "as", "assert", "async", "await", "break", "class", "continue",
                                          "def", "del", "elif", "else", "except", "finally", "for", "from",
                                          "global", "if", "import", "in", "is", "lambda", "nonlocal", "not", "or",
                                          "pass", "raise", "return", "try", "while", "with", "yield", NULL };
static const char *const s_builtins[] = { "print", "len", "range", "int", "str", "float", "bool", "list", "dict",
                                          "tuple", "set", "bytes", "bytearray", "open", "isinstance", "type",
                                          "super", "enumerate", "zip", "map", "filter", "sorted", "min", "max",
                                          "sum", "abs", "round", "hex", "ord", "chr", "repr", "iter", "next",
                                          "getattr", "setattr", "hasattr", "object", "Exception", "self",
                                          "OSError", "ValueError", "KeyboardInterrupt", NULL };

static bool in_list(const char *const *list, const char *w, int n)
{
    for (int i = 0; list[i]; i++) {
        if ((int)strlen(list[i]) == n && !strncmp(list[i], w, (size_t)n)) return true;
    }
    return false;
}

typedef struct {
    char *buf;
    size_t n, len;
} out_t;

static void put(out_t *o, const char *s, size_t n)
{
    if (o->len + n + 1 >= o->n) n = o->n - o->len - 1;
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = 0;
}

/* One run of text in a colour (0 = default). '#' can't appear inside a
 * recolour span, so it is written as "##" between spans. */
static void seg(out_t *o, const char *s, size_t n, uint32_t color)
{
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && s[j] != '#') j++;
        if (j > i) {
            if (color) {
                char hdr[10];
                snprintf(hdr, sizeof hdr, "#%06x ", (unsigned)color);
                put(o, hdr, 8);
                put(o, s + i, j - i);
                put(o, "#", 1);
            } else {
                put(o, s + i, j - i);
            }
        }
        if (j < n) {
            put(o, "##", 2);
            j++;
        }
        i = j;
    }
}

/* Markup for one line; returns the triple-quote state at its end. */
static uint8_t colorize(const char *s, uint8_t st, out_t *o)
{
    size_t n = strlen(s), i = 0;
    bool after_def = false;
    while (i < n) {
        if (st) {               /* inside a triple-quoted string */
            size_t j = i;
            while (j + 2 < n && !(s[j] == st && s[j + 1] == st && s[j + 2] == st)) j++;
            if (j + 2 < n) {
                seg(o, s + i, j + 3 - i, C_STRING);
                i = j + 3;
                st = 0;
            } else {
                seg(o, s + i, n - i, C_STRING);
                i = n;
            }
            continue;
        }
        char c = s[i];
        if (c == '#') {
            seg(o, s + i, n - i, C_COMMENT);
            break;
        }
        if (c == '"' || c == '\'') {
            if (i + 2 < n && s[i + 1] == c && s[i + 2] == c) {
                size_t j = i + 3;
                while (j + 2 < n && !(s[j] == c && s[j + 1] == c && s[j + 2] == c)) j++;
                if (j + 2 < n) {
                    seg(o, s + i, j + 3 - i, C_STRING);
                    i = j + 3;
                } else {
                    seg(o, s + i, n - i, C_STRING);
                    i = n;
                    st = (uint8_t)c;
                }
                continue;
            }
            size_t j = i + 1;
            while (j < n && s[j] != c) j += s[j] == '\\' && j + 1 < n ? 2 : 1;
            if (j < n) j++;
            seg(o, s + i, j - i, C_STRING);
            i = j;
            continue;
        }
        if (isdigit((unsigned char)c) && (i == 0 || !(isalnum((unsigned char)s[i - 1]) || s[i - 1] == '_'))) {
            size_t j = i;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '.' || s[j] == '_')) j++;
            seg(o, s + i, j - i, C_NUMBER);
            i = j;
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_' || c == '@') {
            size_t j = i + 1;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_' || (c == '@' && s[j] == '.'))) j++;
            int len = (int)(j - i);
            uint32_t col = 0;
            if (c == '@') col = C_DEF;
            else if (after_def) col = C_DEF;
            else if (in_list(s_keywords, s + i, len)) col = C_KEYWORD;
            else if ((len == 4 && !strncmp(s + i, "True", 4)) || (len == 5 && !strncmp(s + i, "False", 5)) ||
                     (len == 4 && !strncmp(s + i, "None", 4)))
                col = C_CONST;
            else if (in_list(s_builtins, s + i, len)) col = C_BUILTIN;
            after_def = (len == 3 && !strncmp(s + i, "def", 3)) || (len == 5 && !strncmp(s + i, "class", 5));
            seg(o, s + i, (size_t)len, col);
            i = j;
            continue;
        }
        size_t j = i + 1;
        while (j < n && !(isalnum((unsigned char)s[j]) || s[j] == '_' || s[j] == '#' || s[j] == '"' ||
                          s[j] == '\'' || s[j] == '@'))
            j++;
        seg(o, s + i, j - i, 0);
        i = j;
    }
    return st;
}

static void refresh_state(cv_t *cv)
{
    if (!cv->state_stale) return;
    uint8_t st = 0;
    char scratch[16];
    for (int i = 0; i < cv->n; i++) {
        cv->state[i] = st;
        /* Only the end state matters here: colourise into a tiny buffer. */
        out_t o = { scratch, sizeof scratch, 0 };
        st = colorize(cv->lines[i], st, &o);
    }
    cv->state_stale = false;
}

/* ---- geometry ------------------------------------------------------------------------------ */
static void update_size(cv_t *cv)
{
    int32_t w = (GUTTER + 2 + cv->maxlen + 4) * cv->cw;
    int32_t h = cv->n * cv->lh + T5_DP(48);
    lv_obj_set_size(cv->content, LV_MAX(w, lv_obj_get_content_width(cv->scroll)), h);
}

static int32_t text_x0(const cv_t *cv)
{
    return (GUTTER + 2) * cv->cw;
}

static void place_ta(cv_t *cv)
{
    int dc = disp_col(cv->lines[cv->row], cv->col);
    lv_obj_set_pos(cv->ta, text_x0(cv) + dc * cv->cw, cv->row * cv->lh);
}

static void notify(cv_t *cv)
{
    if (cv->changed) cv->changed(cv, cv->changed_arg);
}

static void cursor_moved(cv_t *cv, bool edited)
{
    cv->cursor_on = true;
    if (edited) {
        cv->dirty = true;
        cv->state_stale = true;
        recalc_max(cv);
        update_size(cv);
    }
    place_ta(cv);
    lv_obj_update_layout(cv->scroll);
    lv_obj_scroll_to_view_recursive(cv->ta, LV_ANIM_OFF);
    lv_obj_invalidate(cv->content);
    notify(cv);
}

/* ---- edits ------------------------------------------------------------------------------------ */
static bool insert_line(cv_t *cv, int at, char *text)
{
    if (!ensure_cap(cv, cv->n + 1)) return false;
    memmove(&cv->lines[at + 1], &cv->lines[at], sizeof(char *) * (size_t)(cv->n - at));
    cv->lines[at] = text;
    cv->n++;
    return true;
}

static void insert_bytes(cv_t *cv, const char *s, size_t k)
{
    char *l = cv->lines[cv->row];
    size_t len = strlen(l);
    char *nl = malloc(len + k + 1);
    if (!nl) return;
    memcpy(nl, l, (size_t)cv->col);
    memcpy(nl + cv->col, s, k);
    memcpy(nl + cv->col + k, l + cv->col, len - (size_t)cv->col + 1);
    free(l);
    cv->lines[cv->row] = nl;
    cv->col += (int)k;
}

static int indent_of(const char *l)
{
    int i = 0;
    while (l[i] == ' ') i++;
    return i;
}

static void newline(cv_t *cv)
{
    char *l = cv->lines[cv->row];
    int ind = indent_of(l);
    /* One level deeper after a line ending in ':' (ignoring trailing space). */
    int e = cv->col;
    while (e > 0 && l[e - 1] == ' ') e--;
    if (e > 0 && l[e - 1] == ':') ind += INDENT;
    if (ind > cv->col && cv->col <= indent_of(l)) ind = cv->col;
    size_t rest = strlen(l + cv->col);
    char *nl = malloc((size_t)ind + rest + 1);
    if (!nl) return;
    memset(nl, ' ', (size_t)ind);
    memcpy(nl + ind, l + cv->col, rest + 1);
    l[cv->col] = 0;
    /* Strip what the new line's indent replaces. */
    if (!insert_line(cv, cv->row + 1, nl)) {
        free(nl);
        return;
    }
    cv->row++;
    cv->col = ind;
}

static void insert_text(cv_t *cv, const char *s)
{
    while (*s) {
        if (*s == '\n') {
            newline(cv);
            s++;
        } else if (*s == '\t') {
            int pad = INDENT - (disp_col(cv->lines[cv->row], cv->col) % INDENT);
            insert_bytes(cv, "    ", (size_t)pad);
            s++;
        } else if (*s == '\r') {
            s++;
        } else {
            const char *e = s;
            while (*e && *e != '\n' && *e != '\t' && *e != '\r') e++;
            insert_bytes(cv, s, (size_t)(e - s));
            s = e;
        }
    }
    cv->want_col = -1;
    cursor_moved(cv, true);
}

static void backspace(cv_t *cv)
{
    char *l = cv->lines[cv->row];
    if (cv->col > 0) {
        int del = 1;
        if (cv->col <= indent_of(l) && cv->col % INDENT == 0) del = INDENT;   /* a whole level */
        else if (cv->col <= indent_of(l)) del = cv->col % INDENT;
        else {
            while (cv->col - del > 0 && ((unsigned char)l[cv->col - del] & 0xc0) == 0x80) del++;
        }
        memmove(l + cv->col - del, l + cv->col, strlen(l + cv->col) + 1);
        cv->col -= del;
    } else if (cv->row > 0) {
        char *prev = cv->lines[cv->row - 1];
        size_t pl = strlen(prev), ll = strlen(l);
        char *nl = malloc(pl + ll + 1);
        if (!nl) return;
        memcpy(nl, prev, pl);
        memcpy(nl + pl, l, ll + 1);
        free(prev);
        free(l);
        cv->lines[cv->row - 1] = nl;
        memmove(&cv->lines[cv->row], &cv->lines[cv->row + 1], sizeof(char *) * (size_t)(cv->n - cv->row - 1));
        cv->n--;
        cv->row--;
        cv->col = (int)pl;
    } else {
        return;
    }
    cv->want_col = -1;
    cursor_moved(cv, true);
}

static void delete_fwd(cv_t *cv)
{
    char *l = cv->lines[cv->row];
    if (l[cv->col]) {
        int del = 1;
        while (l[cv->col + del] && ((unsigned char)l[cv->col + del] & 0xc0) == 0x80) del++;
        memmove(l + cv->col, l + cv->col + del, strlen(l + cv->col + del) + 1);
    } else if (cv->row + 1 < cv->n) {
        cv->row++;
        cv->col = 0;
        backspace(cv);
        return;
    } else {
        return;
    }
    cursor_moved(cv, true);
}

static void dedent(cv_t *cv)
{
    char *l = cv->lines[cv->row];
    int ind = indent_of(l);
    int del = ind % INDENT ? ind % INDENT : (ind >= INDENT ? INDENT : 0);
    if (!del) return;
    memmove(l, l + del, strlen(l + del) + 1);
    cv->col = cv->col > del ? cv->col - del : 0;
    cursor_moved(cv, true);
}

static void move_v(cv_t *cv, int d)
{
    int nr = cv->row + d;
    if (nr < 0) nr = 0;
    if (nr >= cv->n) nr = cv->n - 1;
    if (cv->want_col < 0) cv->want_col = disp_col(cv->lines[cv->row], cv->col);
    cv->row = nr;
    cv->col = byte_col(cv->lines[nr], cv->want_col);
    cursor_moved(cv, false);
}

static void move_h(cv_t *cv, int d)
{
    char *l = cv->lines[cv->row];
    if (d < 0) {
        if (cv->col > 0) {
            cv->col--;
            while (cv->col > 0 && ((unsigned char)l[cv->col] & 0xc0) == 0x80) cv->col--;
        } else if (cv->row > 0) {
            cv->row--;
            cv->col = line_len(cv, cv->row);
        }
    } else {
        if (l[cv->col]) {
            cv->col++;
            while (l[cv->col] && ((unsigned char)l[cv->col] & 0xc0) == 0x80) cv->col++;
        } else if (cv->row + 1 < cv->n) {
            cv->row++;
            cv->col = 0;
        }
    }
    cv->want_col = -1;
    cursor_moved(cv, false);
}

/* ---- input ---------------------------------------------------------------------------------------- */
static void reset_ta(cv_t *cv)
{
    cv->resetting = true;
    lv_textarea_set_text(cv->ta, SENTINEL);
    lv_textarea_set_cursor_pos(cv->ta, LV_TEXTAREA_CURSOR_LAST);
    cv->resetting = false;
}

static void ta_key(lv_event_t *e)
{
    cv_t *cv = lv_event_get_user_data(e);
    uint32_t k = lv_event_get_key(e);
    switch (k) {
    case LV_KEY_UP: move_v(cv, -1); break;
    case LV_KEY_DOWN: move_v(cv, 1); break;
    case LV_KEY_LEFT: move_h(cv, -1); break;
    case LV_KEY_RIGHT: move_h(cv, 1); break;
    case LV_KEY_HOME: {
        int ind = indent_of(cv->lines[cv->row]);
        cv->col = cv->col == ind ? 0 : ind;
        cv->want_col = -1;
        cursor_moved(cv, false);
        break;
    }
    case LV_KEY_END:
        cv->col = line_len(cv, cv->row);
        cv->want_col = -1;
        cursor_moved(cv, false);
        break;
    case LV_KEY_ENTER: insert_text(cv, "\n"); break;
    case LV_KEY_BACKSPACE: backspace(cv); break;
    case LV_KEY_DEL: delete_fwd(cv); break;
    case LV_KEY_NEXT: insert_text(cv, "\t"); break;
    case LV_KEY_PREV: dedent(cv); break;
    case LV_KEY_ESC: t5_shell_keyboard_hide(); break;
    default:
        if (k >= 0x20 && k < 0x7f) {
            char s[2] = { (char)k, 0 };
            insert_text(cv, s);
        } else {
            return;     /* let LVGL have it */
        }
    }
    lv_event_stop_processing(e);
}

/* The on-screen keyboard types with lv_textarea_add_text: take the text
 * and cancel the insert. */
static void ta_insert(lv_event_t *e)
{
    cv_t *cv = lv_event_get_user_data(e);
    if (cv->resetting) return;
    const char *txt = lv_event_get_param(e);
    /* Its backspace comes through here too, as LV_KEY_DEL. */
    if (txt && (unsigned char)txt[0] == LV_KEY_DEL && !txt[1]) backspace(cv);
    else if (txt && txt[0]) insert_text(cv, txt);
    lv_textarea_set_insert_replace(cv->ta, "");
}

/* Its backspace deletes from the sentinel: notice and put it back. */
static void ta_changed(lv_event_t *e)
{
    cv_t *cv = lv_event_get_user_data(e);
    if (cv->resetting) return;
    size_t len = strlen(lv_textarea_get_text(cv->ta));
    if (len < strlen(SENTINEL)) backspace(cv);
    reset_ta(cv);
}

static void ta_ready(lv_event_t *e)
{
    insert_text(lv_event_get_user_data(e), "\n");
}

static void content_pressed(lv_event_t *e)
{
    cv_t *cv = lv_event_get_user_data(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a;
    lv_obj_get_coords(cv->content, &a);
    int r = (int)((p.y - a.y1) / cv->lh);
    if (r < 0) r = 0;
    if (r >= cv->n) r = cv->n - 1;
    int dc = (int)((p.x - a.x1 - text_x0(cv) + cv->cw / 2) / cv->cw);
    if (dc < 0) dc = 0;
    cv->row = r;
    cv->col = byte_col(cv->lines[r], dc);
    cv->want_col = -1;
    cursor_moved(cv, false);
    cv_focus(cv);
}

/* ---- drawing ----------------------------------------------------------------------------------------- */
static void draw(lv_event_t *e)
{
    cv_t *cv = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    refresh_state(cv);
    lv_area_t ca, sa;
    lv_obj_get_coords(cv->content, &ca);
    lv_obj_get_coords(cv->scroll, &sa);
    int first = (int)((sa.y1 - ca.y1) / cv->lh);
    int last = (int)((sa.y2 - ca.y1) / cv->lh) + 1;
    if (first < 0) first = 0;
    if (last >= cv->n) last = cv->n - 1;
    const md_scheme_t *sc = md_scheme();
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = md_font_mono();
    d.color = md_color(sc->c.on_surface);
    d.flag |= LV_TEXT_FLAG_RECOLOR;
    d.text_local = 1;
    lv_draw_label_dsc_t g;
    lv_draw_label_dsc_init(&g);
    g.font = d.font;
    g.color = lv_color_hex(C_GUTTER);
    g.align = LV_TEXT_ALIGN_RIGHT;
    g.text_local = 1;
    static char buf[LINE_BUF];
    for (int r = first; r <= last; r++) {
        int32_t y = ca.y1 + r * cv->lh;
        char num[12];
        snprintf(num, sizeof num, "%d", r + 1);
        g.text = num;
        lv_area_t ga = { ca.x1, y, ca.x1 + GUTTER * cv->cw - 1, y + cv->lh - 1 };
        lv_draw_label(layer, &g, &ga);
        out_t o = { buf, sizeof buf, 0 };
        buf[0] = 0;
        colorize(cv->lines[r], cv->state[r], &o);
        if (!buf[0]) continue;
        d.text = buf;
        lv_area_t ta = { ca.x1 + text_x0(cv), y, ca.x2, y + cv->lh - 1 };
        lv_draw_label(layer, &d, &ta);
    }
    /* The cursor. */
    if (cv->cursor_on && cv->row >= first && cv->row <= last) {
        lv_draw_rect_dsc_t rd;
        lv_draw_rect_dsc_init(&rd);
        rd.bg_color = md_color(sc->c.secondary);
        rd.bg_opa = LV_OPA_COVER;
        int32_t x = ca.x1 + text_x0(cv) + disp_col(cv->lines[cv->row], cv->col) * cv->cw;
        int32_t y = ca.y1 + cv->row * cv->lh;
        lv_area_t cr = { x - 1, y + 2, x + 1, y + cv->lh - 3 };
        lv_draw_rect(layer, &rd, &cr);
    }
}

static void blink(lv_timer_t *t)
{
    cv_t *cv = lv_timer_get_user_data(t);
    bool focused = lv_obj_has_state(cv->ta, LV_STATE_FOCUSED);
    bool on = focused ? !cv->cursor_on : true;
    if (on == cv->cursor_on) return;
    cv->cursor_on = on;
    lv_area_t ca;
    lv_obj_get_coords(cv->content, &ca);
    int32_t x = ca.x1 + text_x0(cv) + disp_col(cv->lines[cv->row], cv->col) * cv->cw;
    int32_t y = ca.y1 + cv->row * cv->lh;
    lv_area_t cr = { x - 2, y, x + 2, y + cv->lh };
    lv_obj_invalidate_area(cv->content, &cr);
}

static void deleted(lv_event_t *e)
{
    cv_t *cv = lv_event_get_user_data(e);
    if (cv->blink) lv_timer_delete(cv->blink);
    free_lines(cv);
    free(cv->lines);
    free(cv->state);
    free(cv);
}

/* ---- API ------------------------------------------------------------------------------------------- */
lv_obj_t *cv_create(lv_obj_t *parent)
{
    cv_t *cv = calloc(1, sizeof *cv);
    if (!cv) return NULL;
    lv_obj_t *s = lv_obj_create(parent);
    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s, md_color(md_scheme()->c.background), 0);
    lv_obj_set_style_pad_all(s, T5_DP(4), 0);
    lv_obj_set_scroll_dir(s, LV_DIR_ALL);
    lv_obj_set_user_data(s, cv);
    lv_obj_t *c = lv_obj_create(s);
    lv_obj_remove_style_all(c);
    lv_obj_set_clickable(c, true);
    lv_obj_set_scrollable(c, false);
    cv->scroll = s;
    cv->content = c;
    const lv_font_t *f = md_font_mono();
    cv->cw = lv_font_get_glyph_width(f, 'M', 0);
    cv->lh = lv_font_get_line_height(f) + T5_DP(2);
    cv->ta = lv_textarea_create(c);
    lv_textarea_set_one_line(cv->ta, true);
    lv_obj_remove_style_all(cv->ta);
    lv_obj_set_size(cv->ta, 1, cv->lh);
    /* Invisible in every state (the theme outlines a focused text area). */
    lv_obj_set_style_opa(cv->ta, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_style_opa(cv->ta, LV_OPA_TRANSP, LV_PART_CURSOR | LV_STATE_ANY);
    lv_obj_set_style_outline_width(cv->ta, 0, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_add_event_cb(cv->ta, ta_key, LV_EVENT_KEY | LV_EVENT_PREPROCESS, cv);
    lv_obj_add_event_cb(cv->ta, ta_insert, LV_EVENT_INSERT, cv);
    lv_obj_add_event_cb(cv->ta, ta_changed, LV_EVENT_VALUE_CHANGED, cv);
    lv_obj_add_event_cb(cv->ta, ta_ready, LV_EVENT_READY, cv);
    lv_obj_add_event_cb(c, draw, LV_EVENT_DRAW_MAIN, cv);
    lv_obj_add_event_cb(c, content_pressed, LV_EVENT_SHORT_CLICKED, cv);
    lv_obj_add_event_cb(s, deleted, LV_EVENT_DELETE, cv);
    cv->blink = lv_timer_create(blink, 530, cv);
    cv->want_col = -1;
    cv_set_text(cv, "", 0);
    reset_ta(cv);
    return s;
}

cv_t *cv_get(lv_obj_t *view)
{
    return view ? lv_obj_get_user_data(view) : NULL;
}

bool cv_set_text(cv_t *cv, const char *text, size_t len)
{
    free_lines(cv);
    size_t i = 0;
    do {
        size_t j = i;
        while (j < len && text[j] != '\n') j++;
        size_t e = j;
        if (e > i && text[e - 1] == '\r') e--;
        char *l = malloc(e - i + 1);
        if (!l || !ensure_cap(cv, cv->n + 1)) {
            free(l);
            return false;
        }
        /* Tabs become spaces: the view works in spaces. */
        size_t k = 0;
        for (size_t x = i; x < e; x++) l[k++] = text[x] == '\t' ? ' ' : text[x];
        l[k] = 0;
        cv->lines[cv->n++] = l;
        i = j + 1;
    } while (i <= len && !(i == len && len > 0 && text[len - 1] == '\n'));
    if (!cv->n) {
        ensure_cap(cv, 1);
        cv->lines[cv->n++] = strdup("");
    }
    cv->row = cv->col = 0;
    cv->state_stale = true;
    recalc_max(cv);
    update_size(cv);
    cv->dirty = false;
    place_ta(cv);
    lv_obj_scroll_to(cv->scroll, 0, 0, LV_ANIM_OFF);
    lv_obj_invalidate(cv->content);
    return true;
}

char *cv_text(cv_t *cv, size_t *len)
{
    size_t total = 0;
    for (int i = 0; i < cv->n; i++) total += strlen(cv->lines[i]) + 1;
    char *out = malloc(total + 1);
    if (!out) return NULL;
    size_t k = 0;
    for (int i = 0; i < cv->n; i++) {
        /* Trailing spaces go (they come from auto-indent on blank lines). */
        size_t l = strlen(cv->lines[i]);
        while (l > 0 && cv->lines[i][l - 1] == ' ') l--;
        memcpy(out + k, cv->lines[i], l);
        k += l;
        out[k++] = '\n';
    }
    out[k] = 0;
    if (len) *len = k;
    return out;
}

bool cv_dirty(const cv_t *cv)
{
    return cv->dirty;
}

void cv_set_clean(cv_t *cv)
{
    cv->dirty = false;
    notify(cv);
}

void cv_set_changed_cb(cv_t *cv, void (*cb)(cv_t *, void *), void *arg)
{
    cv->changed = cb;
    cv->changed_arg = arg;
}

void cv_focus(cv_t *cv)
{
    lv_group_t *g = lv_group_get_default();
    if (g) {
        if (lv_obj_get_group(cv->ta) != g) lv_group_add_obj(g, cv->ta);
        lv_group_focus_obj(cv->ta);
        lv_group_set_editing(g, true);     /* Tab and Shift-Tab come to us */
    }
    lv_obj_add_state(cv->ta, LV_STATE_FOCUSED);
    t5_shell_keyboard_show(cv->ta);
    reset_ta(cv);
}

void cv_cursor(const cv_t *cv, int *line, int *col)
{
    if (line) *line = cv->row + 1;
    if (col) *col = disp_col(cv->lines[cv->row], cv->col) + 1;
}

void cv_goto_line(cv_t *cv, int line)
{
    if (line < 1) line = 1;
    if (line > cv->n) line = cv->n;
    cv->row = line - 1;
    cv->col = indent_of(cv->lines[cv->row]);
    cursor_moved(cv, false);
}

int cv_line_count(const cv_t *cv)
{
    return cv->n;
}
