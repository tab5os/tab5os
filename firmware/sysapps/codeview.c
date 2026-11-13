/* SPDX-License-Identifier: Apache-2.0
 * Code view: see codeview.h.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "t5_gui/t5ui_sys.h"   /* t5ui_sys_native: the shell's keyboard takes the text area */
#include "t5_shell/shell.h"
#include "codeview.h"

#define INDENT 4
#define GUTTER 4                 /* line number columns */
#define RUN_BUF 1024             /* one drawn run of text */
#define BLINK_MS 530
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
    t5ui_obj_t *scroll, *content, *ta, *cursor;
    t5ui_timer_t *blink;
    char **lines;
    int n, cap;
    int row, col, want_col;
    int maxlen;
    int32_t cw, lh;
    bool dirty, cursor_on, resetting;
    uint8_t *state;              /* triple-quote state at each line start: 0, '\'' or '"' */
    bool state_stale;
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

/* Where colorize sends each run of text in a colour (0 = default); NULL
 * when only the end state is wanted. */
typedef struct {
    void (*run)(void *ctx, const char *s, size_t n, uint32_t color);
    void *ctx;
} out_t;

static void seg(out_t *o, const char *s, size_t n, uint32_t color)
{
    if (o && o->run && n) o->run(o->ctx, s, n, color);
}

/* The runs of one line; returns the triple-quote state at its end. */
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
    for (int i = 0; i < cv->n; i++) {
        cv->state[i] = st;
        st = colorize(cv->lines[i], st, NULL);   /* only the end state matters here */
    }
    cv->state_stale = false;
}

/* ---- geometry ------------------------------------------------------------------------------ */
static void update_size(cv_t *cv)
{
    int32_t w = (GUTTER + 2 + cv->maxlen + 4) * cv->cw;
    int32_t h = cv->n * cv->lh + t5ui_dp(48);
    t5ui_rect_t sc;
    t5ui_content_coords(cv->scroll, &sc);
    int32_t sw = sc.x2 - sc.x1 + 1;
    t5ui_set_int(cv->content, T5UI_P_WIDTH, w > sw ? w : sw);
    t5ui_set_int(cv->content, T5UI_P_HEIGHT, h);
}

static int32_t text_x0(const cv_t *cv)
{
    return (GUTTER + 2) * cv->cw;
}

/* The text area and the drawn cursor go to the cursor's place. */
static void place_ta(cv_t *cv)
{
    int dc = disp_col(cv->lines[cv->row], cv->col);
    int32_t x = text_x0(cv) + dc * cv->cw, y = cv->row * cv->lh;
    t5ui_set_int(cv->ta, T5UI_P_X, x);
    t5ui_set_int(cv->ta, T5UI_P_Y, y);
    t5ui_set_int(cv->cursor, T5UI_P_X, x - 1);
    t5ui_set_int(cv->cursor, T5UI_P_Y, y + 2);
}

static void show_cursor(cv_t *cv, bool on)
{
    cv->cursor_on = on;
    t5ui_set_int(cv->cursor, T5UI_P_HIDDEN, !on);
}

static void notify(cv_t *cv)
{
    if (cv->changed) cv->changed(cv, cv->changed_arg);
}

static void cursor_moved(cv_t *cv, bool edited)
{
    show_cursor(cv, true);
    if (edited) {
        cv->dirty = true;
        cv->state_stale = true;
        recalc_max(cv);
        update_size(cv);
    }
    place_ta(cv);
    t5ui_layout_now(cv->scroll);
    t5ui_scroll_into_view(cv->ta, false);
    t5ui_invalidate(cv->content);
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
    t5ui_set_str(cv->ta, T5UI_P_TEXT, SENTINEL);
    t5ui_set_int(cv->ta, T5UI_P_CURSOR, (int32_t)strlen(SENTINEL));   /* the end */
    cv->resetting = false;
}

static void ta_key(t5ui_event_t *e)
{
    cv_t *cv = t5ui_event_user(e);
    uint32_t k = t5ui_event_key(e);
    switch (k) {
    case T5UI_KEY_UP: move_v(cv, -1); break;
    case T5UI_KEY_DOWN: move_v(cv, 1); break;
    case T5UI_KEY_LEFT: move_h(cv, -1); break;
    case T5UI_KEY_RIGHT: move_h(cv, 1); break;
    case T5UI_KEY_HOME: {
        int ind = indent_of(cv->lines[cv->row]);
        cv->col = cv->col == ind ? 0 : ind;
        cv->want_col = -1;
        cursor_moved(cv, false);
        break;
    }
    case T5UI_KEY_END:
        cv->col = line_len(cv, cv->row);
        cv->want_col = -1;
        cursor_moved(cv, false);
        break;
    case T5UI_KEY_ENTER: insert_text(cv, "\n"); break;
    case T5UI_KEY_BACKSPACE: backspace(cv); break;
    case T5UI_KEY_DEL: delete_fwd(cv); break;
    case T5UI_KEY_NEXT: insert_text(cv, "\t"); break;
    case T5UI_KEY_PREV: dedent(cv); break;
    case T5UI_KEY_ESC: t5_shell_keyboard_hide(); break;
    default:
        if (k >= 0x20 && k < 0x7f) {
            char s[2] = { (char)k, 0 };
            insert_text(cv, s);
        } else {
            return;     /* let the text area have it */
        }
    }
    t5ui_event_stop(e);
}

/* The on-screen keyboard types by inserting into the text area: take the
 * text and cancel the insert. */
static void ta_insert(t5ui_event_t *e)
{
    cv_t *cv = t5ui_event_user(e);
    if (cv->resetting) return;
    const char *txt = t5ui_event_text(e);
    /* Its backspace comes through here too, as T5UI_KEY_DEL. */
    if (txt && (unsigned char)txt[0] == T5UI_KEY_DEL && !txt[1]) backspace(cv);
    else if (txt && txt[0]) insert_text(cv, txt);
    t5ui_event_set_text(e, "");
}

/* Its backspace deletes from the sentinel: notice and put it back. */
static void ta_changed(t5ui_event_t *e)
{
    cv_t *cv = t5ui_event_user(e);
    if (cv->resetting) return;
    size_t len = strlen(t5ui_get_str(cv->ta, T5UI_P_TEXT, ""));
    if (len < strlen(SENTINEL)) backspace(cv);
    reset_ta(cv);
}

static void ta_ready(t5ui_event_t *e)
{
    insert_text(t5ui_event_user(e), "\n");
}

static void content_pressed(t5ui_event_t *e)
{
    cv_t *cv = t5ui_event_user(e);
    int32_t px, py;
    if (!t5ui_event_point(e, &px, &py)) return;
    t5ui_rect_t a;
    t5ui_coords(cv->content, &a);
    int r = (int)((py - a.y1) / cv->lh);
    if (r < 0) r = 0;
    if (r >= cv->n) r = cv->n - 1;
    int dc = (int)((px - a.x1 - text_x0(cv) + cv->cw / 2) / cv->cw);
    if (dc < 0) dc = 0;
    cv->row = r;
    cv->col = byte_col(cv->lines[r], dc);
    cv->want_col = -1;
    cursor_moved(cv, false);
    cv_focus(cv);
}

/* ---- drawing ----------------------------------------------------------------------------------------- */
/* A line's runs, drawn as they come; runs of one colour are drawn together. */
typedef struct {
    t5ui_paint_t *paint;
    const cv_t *cv;
    int32_t y, x2;               /* the line's top, the content's right edge */
    int col;                     /* display column the pending run starts at */
    const char *start;           /* the pending run */
    size_t n;
    uint32_t color;
    t5ui_color_t text;           /* the default colour */
} line_draw_t;

static void flush_run(line_draw_t *d)
{
    if (!d->n) return;
    static char buf[RUN_BUF];
    size_t n = d->n < sizeof buf - 1 ? d->n : sizeof buf - 1;
    memcpy(buf, d->start, n);
    buf[n] = 0;
    int32_t x = text_x0(d->cv) + d->col * d->cv->cw;
    t5ui_rect_t r = { x, d->y, d->x2, d->y + d->cv->lh - 1 };
    t5ui_draw_text(d->paint, &r, buf, T5UI_TEXT_MONO, d->color ? d->color : d->text, T5UI_TEXT_LEFT);
    d->col += disp_col(d->start, (int)d->n);
    d->n = 0;
}

static void draw_run(void *ctx, const char *s, size_t n, uint32_t color)
{
    line_draw_t *d = ctx;
    if (d->n && color == d->color && d->start + d->n == s) {
        d->n += n;
        return;
    }
    flush_run(d);
    d->start = s;
    d->n = n;
    d->color = color;
}

/* Under the content's children (the cursor): the visible lines. */
static void draw(t5ui_event_t *e)
{
    cv_t *cv = t5ui_event_user(e);
    t5ui_paint_t *paint = t5ui_event_paint(e);
    if (!paint) return;
    refresh_state(cv);
    t5ui_rect_t ca, sa;
    t5ui_coords(cv->content, &ca);
    t5ui_coords(cv->scroll, &sa);
    int first = (int)((sa.y1 - ca.y1) / cv->lh);
    int last = (int)((sa.y2 - ca.y1) / cv->lh) + 1;
    if (first < 0) first = 0;
    if (last >= cv->n) last = cv->n - 1;
    /* Paint coordinates are the content's own. */
    line_draw_t d = { .paint = paint, .cv = cv, .x2 = ca.x2 - ca.x1,
                      .text = t5ui_theme_color(T5UI_COLOR_ON_SURFACE) };
    out_t o = { draw_run, &d };
    for (int r = first; r <= last; r++) {
        int32_t y = r * cv->lh;
        char num[12];
        snprintf(num, sizeof num, "%d", r + 1);
        t5ui_rect_t ga = { 0, y, GUTTER * cv->cw - 1, y + cv->lh - 1 };
        t5ui_draw_text(paint, &ga, num, T5UI_TEXT_MONO, C_GUTTER, T5UI_TEXT_RIGHT);
        d.y = y;
        d.col = 0;
        d.n = 0;
        colorize(cv->lines[r], cv->state[r], &o);
        flush_run(&d);
    }
}

/* The cursor blinks while the text area has the keyboard, and stays on
 * otherwise. */
static void blink(t5ui_timer_t *t, void *user)
{
    (void)t;
    cv_t *cv = user;
    bool focused = t5ui_get_int(cv->ta, T5UI_P_STATE, 0) & T5UI_STATE_FOCUSED;
    bool on = focused ? !cv->cursor_on : true;
    if (on != cv->cursor_on) show_cursor(cv, on);
}

static void deleted(t5ui_event_t *e)
{
    cv_t *cv = t5ui_event_user(e);
    t5ui_timer_delete(cv->blink);   /* harmless if the app's exit already took it */
    free_lines(cv);
    free(cv->lines);
    free(cv->state);
    free(cv);
}

/* ---- API ------------------------------------------------------------------------------------------- */
t5ui_obj_t *cv_create(t5ui_obj_t *parent)
{
    cv_t *cv = calloc(1, sizeof *cv);
    if (!cv) return NULL;
    t5ui_obj_t *s = t5ui_create(parent, T5UI_BOX);
    t5ui_set_int(s, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(s, T5UI_P_HEIGHT, T5UI_PCT(100));
    t5ui_set_int(s, T5UI_P_BG_OPA, T5UI_OPA_COVER);
    t5ui_set_color(s, T5UI_P_BG_COLOR, t5ui_theme_color(T5UI_COLOR_BACKGROUND));
    t5ui_set_int(s, T5UI_P_PAD_ALL, t5ui_dp(4));
    t5ui_set_int(s, T5UI_P_SCROLL_DIR, T5UI_DIR_ALL);
    t5ui_set_user(s, cv);
    t5ui_obj_t *c = t5ui_create(s, T5UI_BOX);
    t5ui_set_int(c, T5UI_P_CLICKABLE, true);
    t5ui_set_int(c, T5UI_P_SCROLLABLE, false);
    cv->scroll = s;
    cv->content = c;
    int32_t w, h;
    t5ui_text_size("M", T5UI_TEXT_MONO, 0, &w, &h);
    cv->cw = w;
    cv->lh = h + t5ui_dp(2);
    cv->ta = t5ui_create(c, T5UI_TEXTAREA);
    t5ui_set_int(cv->ta, T5UI_P_ONE_LINE, true);
    t5ui_style_clear(cv->ta);
    t5ui_set_int(cv->ta, T5UI_P_WIDTH, 1);   /* px */
    t5ui_set_int(cv->ta, T5UI_P_HEIGHT, cv->lh);
    /* Invisible (the theme's look is gone, the cursor part too): the view
     * draws the text and its own cursor. */
    t5ui_set_int(cv->ta, T5UI_P_OPA, T5UI_OPA_TRANSP);
    t5ui_set_int_at(cv->ta, T5UI_P_OPA, T5UI_AT(T5UI_PART_CURSOR, T5UI_STATE_DEFAULT), T5UI_OPA_TRANSP);
    t5ui_set_int(cv->ta, T5UI_P_OUTLINE_WIDTH, 0);
    t5ui_on(cv->ta, T5UI_EV_KEY, ta_key, cv);
    t5ui_on(cv->ta, T5UI_EV_INSERT, ta_insert, cv);
    t5ui_on(cv->ta, T5UI_EV_VALUE, ta_changed, cv);
    t5ui_on(cv->ta, T5UI_EV_READY, ta_ready, cv);
    /* The cursor: a bar over the text, shown and hidden to blink. */
    cv->cursor = t5ui_create(c, T5UI_BOX);
    t5ui_set_int(cv->cursor, T5UI_P_WIDTH, 3);   /* px */
    t5ui_set_int(cv->cursor, T5UI_P_HEIGHT, cv->lh - 4);
    t5ui_set_int(cv->cursor, T5UI_P_BG_OPA, T5UI_OPA_COVER);
    t5ui_set_color(cv->cursor, T5UI_P_BG_COLOR, t5ui_theme_color(T5UI_COLOR_SECONDARY));
    t5ui_set_int(cv->cursor, T5UI_P_CLICKABLE, false);
    t5ui_set_int(cv->cursor, T5UI_P_HIDDEN, true);
    t5ui_on(c, T5UI_EV_DRAW_BEHIND, draw, cv);
    t5ui_on(c, T5UI_EV_SHORT_CLICK, content_pressed, cv);
    t5ui_on(s, T5UI_EV_DELETE, deleted, cv);
    cv->blink = t5ui_timer(BLINK_MS, blink, cv);
    cv->want_col = -1;
    cv_set_text(cv, "", 0);
    reset_ta(cv);
    return s;
}

cv_t *cv_get(t5ui_obj_t *view)
{
    return view ? t5ui_user(view) : NULL;
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
    t5ui_cmd(cv->scroll, T5UI_CMD_SCROLL_TO, 0, 0, false, NULL);
    t5ui_invalidate(cv->content);
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
    t5ui_cmd(cv->ta, T5UI_CMD_FOCUS, 1, 0, 0, NULL);   /* 1: Tab and Shift-Tab come to us */
    t5ui_set_int(cv->ta, T5UI_P_STATE, t5ui_get_int(cv->ta, T5UI_P_STATE, 0) | T5UI_STATE_FOCUSED);
    t5_shell_keyboard_show(t5ui_sys_native(cv->ta));
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
