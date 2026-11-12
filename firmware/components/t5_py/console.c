/* SPDX-License-Identifier: Apache-2.0
 * Console view: monospace program output (Python apps, the Editor's output
 * pane, the Terminal).
 *
 * Writers may be on any task. They append to a pending buffer under a lock,
 * looked up by object pointer in a small table of live consoles, so a write
 * to a console that has just been deleted is dropped rather than touching
 * freed memory. A timer on the UI task moves pending text into the label.
 * Drawn through t5ui; the public calls (t5_py/py.h) take the platform's
 * native objects and convert at the boundary.
 * ANSI escape sequences (from the REPL's line editor) are dropped; '\b'
 * removes the previous character; "\r\n" becomes "\n".
 */
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "t5_app/apps.h"
#include "t5_gui/t5ui.h"
#include "t5_gui/t5ui_sys.h"
#include "t5_py/py.h"
#include "py_priv.h"

#define CON_MAX 4
#define CON_TEXT (16 * 1024)
#define CON_TRIM (4 * 1024)
#define CON_PENDING 4096
#define CON_PERIOD_MS 50
#define CON_SCROLL_END 0x0fffffff   /* scrolling past the end stops at it */

typedef struct {
    t5ui_obj_t *obj, *label;
    char *text;             /* label text (static), UI task only */
    size_t len;
    char pending[CON_PENDING];
    size_t plen;
    bool clear;
    uint8_t esc;            /* 0 none, 1 after ESC, 2 inside CSI */
    t5ui_timer_t *timer;
} con_t;

static con_t *s_cons[CON_MAX];
static SemaphoreHandle_t s_lock;

static void lock(void)
{
    xSemaphoreTake(py_mutex_once(&s_lock), portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static con_t *find(t5ui_obj_t *obj)
{
    for (int i = 0; i < CON_MAX; i++) {
        if (s_cons[i] && s_cons[i]->obj == obj) return s_cons[i];
    }
    return NULL;
}

static void flush(t5ui_timer_t *t, void *user)
{
    (void)t;
    con_t *c = user;
    char tmp[CON_PENDING];
    size_t n;
    bool clear;
    lock();
    n = c->plen;
    memcpy(tmp, c->pending, n);
    c->plen = 0;
    clear = c->clear;
    c->clear = false;
    unlock();
    if (!n && !clear) return;
    if (clear) c->len = 0;
    for (size_t i = 0; i < n; i++) {
        char ch = tmp[i];
        if (c->esc == 1) {
            c->esc = ch == '[' ? 2 : 0;
            continue;
        }
        if (c->esc == 2) {
            if ((ch >= '@' && ch <= '~')) c->esc = 0;
            continue;
        }
        if (ch == 0x1b) {
            c->esc = 1;
            continue;
        }
        if (ch == '\r') continue;
        if (ch == '\b') {
            if (c->len && c->text[c->len - 1] != '\n') c->len--;
            continue;
        }
        if (ch == 0x04 || ch == 0x07) continue;
        if (c->len >= CON_TEXT - 1) {
            /* Drop the oldest part, at a line start. */
            size_t cut = CON_TRIM;
            while (cut < c->len && c->text[cut - 1] != '\n') cut++;
            memmove(c->text, c->text + cut, c->len - cut);
            c->len -= cut;
        }
        c->text[c->len++] = ch;
    }
    c->text[c->len] = 0;
    /* What's left to scroll below the view: the text's end past the content area. */
    t5ui_rect_t text, view;
    t5ui_coords(c->label, &text);
    t5ui_content_coords(c->obj, &view);
    bool at_bottom = text.y2 - view.y2 <= 4;
    t5ui_sys_set_text_static(c->label, c->text);   /* no copy of up to 16 KB */
    if (at_bottom || clear) {
        t5ui_layout_now(c->obj);
        t5ui_set_int(c->obj, T5UI_P_SCROLL_Y, CON_SCROLL_END);
    }
}

static void deleted(t5ui_event_t *e)
{
    con_t *c = t5ui_event_user(e);
    lock();
    for (int i = 0; i < CON_MAX; i++) {
        if (s_cons[i] == c) s_cons[i] = NULL;
    }
    unlock();
    if (c->timer) t5ui_timer_delete(c->timer);
    heap_caps_free(c->text);
    free(c);
}

struct _lv_obj_t *t5_py_console_create(struct _lv_obj_t *parent)
{
    con_t *c = calloc(1, sizeof *c);
    char *text = heap_caps_malloc(CON_TEXT, MALLOC_CAP_SPIRAM);
    if (!c || !text) {
        free(c);
        heap_caps_free(text);
        return NULL;
    }
    text[0] = 0;
    c->text = text;
    t5ui_obj_t *o = t5ui_create(t5ui_sys_obj(parent), T5UI_BOX);
    if (!o) {
        free(c);
        heap_caps_free(text);
        return NULL;
    }
    t5ui_set_int(o, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(o, T5UI_P_HEIGHT, T5UI_PCT(100));
    t5ui_set_int(o, T5UI_P_BG_OPA, T5UI_OPA_COVER);
    t5ui_set_color(o, T5UI_P_BG_COLOR, t5ui_theme_color(T5UI_COLOR_BACKGROUND));
    t5ui_set_int(o, T5UI_P_PAD_ALL, t5ui_dp(8));
    t5ui_set_int(o, T5UI_P_SCROLL_DIR, T5UI_DIR_VER);
    t5ui_obj_t *l = t5ui_create(o, T5UI_LABEL);
    t5ui_set_int(l, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(l, T5UI_P_LONG_MODE, T5UI_LONG_WRAP);
    t5ui_set_int(l, T5UI_P_TEXT_STYLE, T5UI_TEXT_MONO);
    t5ui_set_color(l, T5UI_P_TEXT_COLOR, t5ui_theme_color(T5UI_COLOR_ON_SURFACE));
    t5ui_set_int(l, T5UI_P_TEXT_OPA, t5ui_theme_emphasis(T5UI_EMPHASIS_HIGH));
    t5ui_sys_set_text_static(l, c->text);
    c->obj = o;
    c->label = l;
    /* The timer belongs to the console, not to the app that made it: made
     * as the system, so the app's exit doesn't take it (the console's
     * DELETE does). */
    void *saved = t5_sys_enter();
    c->timer = t5ui_timer(CON_PERIOD_MS, flush, c);
    t5_sys_leave(saved);
    t5ui_on(o, T5UI_EV_DELETE, deleted, c);
    lock();
    bool placed = false;
    for (int i = 0; i < CON_MAX && !placed; i++) {
        if (!s_cons[i]) {
            s_cons[i] = c;
            placed = true;
        }
    }
    unlock();
    if (!placed) {
        t5ui_delete(o);
        return NULL;
    }
    return t5ui_sys_native(o);
}

void t5_py_console_write(struct _lv_obj_t *con, const char *s, size_t n)
{
    if (!con || !s) return;
    lock();
    con_t *c = find(t5ui_sys_obj(con));
    if (c) {
        if (n > CON_PENDING - c->plen) {
            /* The UI isn't keeping up: keep the newest output. */
            if (n >= CON_PENDING) {
                s += n - CON_PENDING;
                n = CON_PENDING;
                c->plen = 0;
            } else {
                size_t drop = n - (CON_PENDING - c->plen);
                memmove(c->pending, c->pending + drop, c->plen - drop);
                c->plen -= drop;
            }
        }
        memcpy(c->pending + c->plen, s, n);
        c->plen += n;
    }
    unlock();
}

void t5_py_console_clear(struct _lv_obj_t *con)
{
    lock();
    con_t *c = find(t5ui_sys_obj(con));
    if (c) {
        c->plen = 0;
        c->clear = true;
    }
    unlock();
}
