/* SPDX-License-Identifier: Apache-2.0
 * Console view: monospace program output (Python apps, the Editor's output
 * pane, the Terminal).
 *
 * Writers may be on any task. They append to a pending buffer under a lock,
 * looked up by object pointer in a small table of live consoles, so a write
 * to a console that has just been deleted is dropped rather than touching
 * freed memory. A timer on the UI task moves pending text into the label.
 * ANSI escape sequences (from the REPL's line editor) are dropped; '\b'
 * removes the previous character; "\r\n" becomes "\n".
 */
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "md/md.h"
#include "t5_py/py.h"

#define CON_MAX 4
#define CON_TEXT (16 * 1024)
#define CON_TRIM (4 * 1024)
#define CON_PENDING 4096
#define CON_PERIOD_MS 50

typedef struct {
    lv_obj_t *obj, *label;
    char *text;             /* label text (static), UI task only */
    size_t len;
    char pending[CON_PENDING];
    size_t plen;
    bool clear;
    uint8_t esc;            /* 0 none, 1 after ESC, 2 inside CSI */
    lv_timer_t *timer;
} con_t;

static con_t *s_cons[CON_MAX];
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void lock(void)
{
    if (!s_lock) {
        SemaphoreHandle_t l = xSemaphoreCreateMutex();
        portENTER_CRITICAL(&s_mux);
        if (!s_lock) {
            s_lock = l;
            l = NULL;
        }
        portEXIT_CRITICAL(&s_mux);
        if (l) vSemaphoreDelete(l);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static con_t *find(lv_obj_t *obj)
{
    for (int i = 0; i < CON_MAX; i++) {
        if (s_cons[i] && s_cons[i]->obj == obj) return s_cons[i];
    }
    return NULL;
}

/* UI task. */
static void flush(lv_timer_t *t)
{
    con_t *c = lv_timer_get_user_data(t);
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
    bool at_bottom = lv_obj_get_scroll_bottom(c->obj) <= 4;
    lv_label_set_text_static(c->label, c->text);
    if (at_bottom || clear) {
        lv_obj_update_layout(c->obj);
        lv_obj_scroll_to_y(c->obj, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

static void deleted(lv_event_t *e)
{
    con_t *c = lv_event_get_user_data(e);
    lock();
    for (int i = 0; i < CON_MAX; i++) {
        if (s_cons[i] == c) s_cons[i] = NULL;
    }
    unlock();
    if (c->timer) lv_timer_delete(c->timer);
    heap_caps_free(c->text);
    free(c);
}

lv_obj_t *t5_py_console_create(lv_obj_t *parent)
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
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, md_color(md_scheme()->c.background), 0);
    lv_obj_set_style_pad_all(o, T5_DP(8), 0);
    lv_obj_set_scroll_dir(o, LV_DIR_VER);
    lv_obj_t *l = lv_label_create(o);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(l, md_font_mono(), 0);
    lv_obj_set_style_text_color(l, md_color(md_scheme()->c.on_surface), 0);
    lv_obj_set_style_text_opa(l, md_scheme()->on_surface.high, 0);
    lv_label_set_text_static(l, c->text);
    c->obj = o;
    c->label = l;
    /* The timer belongs to the console, not to the app that made it. */
    c->timer = lv_timer_create(flush, CON_PERIOD_MS, c);
    lv_obj_add_event_cb(o, deleted, LV_EVENT_DELETE, c);
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
        lv_obj_delete(o);
        return NULL;
    }
    return o;
}

void t5_py_console_write(lv_obj_t *con, const char *s, size_t n)
{
    if (!con || !s) return;
    lock();
    con_t *c = find(con);
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

void t5_py_console_clear(lv_obj_t *con)
{
    lock();
    con_t *c = find(con);
    if (c) {
        c->plen = 0;
        c->clear = true;
    }
    unlock();
}

void t5_py_console_sink(const char *s, size_t n, void *con)
{
    t5_py_console_write(con, s, n);
}
