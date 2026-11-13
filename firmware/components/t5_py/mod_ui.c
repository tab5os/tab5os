/* SPDX-License-Identifier: Apache-2.0
 * t5.ui (plan 8.3): a light wrapper over the MD2 widgets, drawn through t5ui.
 *
 *   s = t5.ui.screen("Air Log")          # app bar with a back arrow
 *   temp = s.label("--", role="h4")
 *   s.button("Refresh", fn=lambda b: refresh())
 *   row = s.row()
 *   row.switch(True, fn=lambda w: print(w.value()))
 *
 * Every t5ui call is made from the VM task under the UI lock. Taps and value
 * changes arrive on the UI task and are queued for the VM task with
 * micropython.schedule semantics, so callbacks run between bytecodes.
 *
 * The UI never holds a pointer into the Python heap: widgets are found through
 * a table indexed by a slot number tagged with the VM's generation, and the
 * generation changes (under the UI lock) before the heap goes away. A late
 * event finds nothing.
 *
 * Sizes are in dp (2 px each). Colours are 0xRRGGBB or a scheme name
 * ("primary", "secondary", "error", "on_surface", "surface", "background").
 */
#include <string.h>
#include "esp_timer.h"
#include "py/mphal.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "t5_app/app.h"
#include "t5_gui/t5ui.h"
#include "t5_gui/t5ui_sys.h"
#include "t5_shell/shell.h"
#include "t5_ui/t5_ui.h"
#include "bind_gen.h"
#include "py_priv.h"

enum { K_BOX, K_LABEL, K_BUTTON, K_SWITCH, K_SLIDER, K_CHECK, K_FIELD, K_ITEM, K_SECTION, K_DIVIDER, K_ICON, K_CHART,
       K_BAR };

typedef struct {
    mp_obj_base_t base;
    t5ui_obj_t *obj;            /* NULL once the UI deleted it */
    t5ui_obj_t *inner;          /* children go here (boxes), the text area (fields), the label (buttons) */
    int16_t ser;                /* charts: the series' index, or -1 */
    mp_obj_t on_click, on_change;
    uint16_t slot;
    uint8_t kind;
} py_widget_t;

volatile uint32_t py_gen;               /* changes when a VM's heap is about to go */
static uint32_t s_wcap;
MP_REGISTER_ROOT_POINTER(void *t5_py_wtab);   /* py_widget_t *[s_wcap], in the GC heap */
#define WTAB ((py_widget_t **)MP_STATE_VM(t5_py_wtab))

extern const mp_obj_type_t py_type_widget;

void py_ui_reset(void)
{
    MP_STATE_VM(t5_py_wtab) = NULL;
    s_wcap = 0;
}

static inline void *tag(uint32_t slot)
{
    return (void *)py_tag(slot);
}

/* UI task (under the lock): the widget an event's user data names, or NULL. */
static py_widget_t *untag(void *ud)
{
    int i = py_untag((uintptr_t)ud);
    if (i < 0 || (uint32_t)i >= s_wcap || !WTAB) return NULL;
    return WTAB[i];
}

/* ---- teardown ----------------------------------------------------------------------------- */
static void invalidate(void)
{
    py_gen++;
    MP_STATE_VM(t5_py_wtab) = NULL;
    s_wcap = 0;
}

void py_ui_teardown_vm(void)
{
    /* Normal end: wait for the UI task to let go of the lock. If a stop is
     * under way the stopper may be holding it; it invalidates for us. */
    py_vm->closing = true;
    while (!t5_ui_lock_timeout(20)) {
        if (py_vm->stop) return;
    }
    invalidate();
    t5_ui_unlock();
}

void py_ui_detach_all(t5_py_t *vm)
{
    (void)vm;
    invalidate();      /* the caller holds the UI lock */
}

bool py_ui_wants_events(void)
{
    return py_vm && py_vm->ui_used && !py_vm->back && !py_vm->stop;
}

/* ---- events (UI task) --------------------------------------------------------------------- */
static void ev_cb(t5ui_event_t *e)
{
    py_widget_t *w = untag(t5ui_event_user(e));
    if (!w) return;
    switch (t5ui_event_kind(e)) {
    case T5UI_EV_DELETE:
        if (t5ui_event_target(e) != w->obj) return;   /* an inner part */
        w->obj = NULL;
        w->inner = NULL;
        WTAB[w->slot] = NULL;
        break;
    case T5UI_EV_CLICK:
        if (w->on_click != mp_const_none) py_schedule(w->on_click, MP_OBJ_FROM_PTR(w));
        break;
    case T5UI_EV_VALUE:
        if (w->on_change != mp_const_none) py_schedule(w->on_change, MP_OBJ_FROM_PTR(w));
        break;
    default: break;
    }
}

/* Back: t5.app.running() turns false at once; a script still in its own
 * code half a second later (not checking it) gets a KeyboardInterrupt. */
static esp_timer_handle_t s_back_timer;

static void back_late(void *arg)
{
    (void)arg;
    t5_py_t *vm = py_vm;
    if (vm && vm->back && vm->in_main && !vm->closing) {
        mp_sched_keyboard_interrupt();
        py_wake();
    }
}

static void back_cb(t5ui_event_t *e)
{
    t5_py_t *vm = py_vm;
    if (py_untag((uintptr_t)t5ui_event_user(e)) < 0) return;
    if (vm->cfg.repl) {
        /* At the prompt: back just takes the script's UI away. */
        if (vm->ui_root) t5ui_cmd(vm->ui_root, T5UI_CMD_DELETE_LATER, 0, 0, 0, NULL);
        vm->ui_root = NULL;
        vm->ui_used = false;
        return;
    }
    vm->back = true;
    py_wake();
    if (!s_back_timer) {
        const esp_timer_create_args_t a = { .callback = back_late, .name = "py_back" };
        if (esp_timer_create(&a, &s_back_timer) != ESP_OK) return;
    }
    esp_timer_stop(s_back_timer);
    esp_timer_start_once(s_back_timer, 500 * 1000);
}

/* ---- helpers (VM task) --------------------------------------------------------------------- */
static MP_NORETURN void raise_deleted(void)
{
    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("widget was deleted"));
}

static py_widget_t *widget(mp_obj_t o)
{
    if (!mp_obj_is_type(o, &py_type_widget)) mp_raise_TypeError(MP_ERROR_TEXT("not a widget"));
    return MP_OBJ_TO_PTR(o);
}

/* Room for one more widget. Allocates outside the lock and swaps under it,
 * since the UI task reads the table. */
static void reserve(void)
{
    for (uint32_t i = 0; i < s_wcap; i++) {
        if (!WTAB[i]) return;
    }
    if (s_wcap >= 0xffff) mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("too many widgets"));
    uint32_t ncap = s_wcap ? s_wcap * 2 : 32;
    py_widget_t **nt = m_new0(py_widget_t *, ncap);
    py_ui_lock();
    if (WTAB) memcpy(nt, WTAB, s_wcap * sizeof *nt);
    MP_STATE_VM(t5_py_wtab) = nt;
    s_wcap = ncap;
    py_ui_unlock();
}

/* Under the lock: put w in a free slot and hook its events. */
static void attach(py_widget_t *w, t5ui_obj_t *obj, t5ui_obj_t *events_on)
{
    uint32_t i = 0;
    while (i < s_wcap && WTAB[i]) i++;
    w->slot = (uint16_t)i;
    w->obj = obj;
    WTAB[i] = w;
    void *t = tag(i);
    t5ui_on(obj, T5UI_EV_DELETE, ev_cb, t);
    t5ui_obj_t *src = events_on ? events_on : obj;
    t5ui_on(src, T5UI_EV_CLICK, ev_cb, t);
    t5ui_on(src, T5UI_EV_VALUE, ev_cb, t);
}

static py_widget_t *new_widget(uint8_t kind, mp_obj_t fn)
{
    py_widget_t *w = mp_obj_malloc(py_widget_t, &py_type_widget);
    w->obj = w->inner = NULL;
    w->ser = -1;
    w->kind = kind;
    w->on_click = mp_const_none;
    w->on_change = mp_const_none;
    if (fn != MP_OBJ_NULL && fn != mp_const_none) {
        if (kind == K_BUTTON || kind == K_ITEM || kind == K_BOX || kind == K_LABEL || kind == K_ICON) w->on_click = fn;
        else w->on_change = fn;
    }
    reserve();
    return w;
}

static t5ui_color_t color_arg(mp_obj_t c)
{
    if (mp_obj_is_int(c)) return (t5ui_color_t)mp_obj_get_int(c) & 0xffffff;
    const char *s = mp_obj_str_get_str(c);
    if (!strcmp(s, "primary")) return t5ui_theme_color(T5UI_COLOR_PRIMARY);
    if (!strcmp(s, "secondary")) return t5ui_theme_color(T5UI_COLOR_SECONDARY);
    if (!strcmp(s, "error")) return t5ui_theme_color(T5UI_COLOR_ERROR);
    if (!strcmp(s, "surface")) return t5ui_theme_color(T5UI_COLOR_SURFACE);
    if (!strcmp(s, "background")) return t5ui_theme_color(T5UI_COLOR_BACKGROUND);
    if (!strcmp(s, "on_surface")) return t5ui_theme_color(T5UI_COLOR_ON_SURFACE);
    if (!strcmp(s, "on_primary")) return t5ui_theme_color(T5UI_COLOR_ON_PRIMARY);
    mp_raise_ValueError(MP_ERROR_TEXT("colour is 0xRRGGBB or a scheme name"));
}

/* Any value as text: str(x) for non-strings. */
static const char *text_arg(mp_obj_t o)
{
    if (!mp_obj_is_str(o)) o = mp_obj_str_make_new(&mp_type_str, 1, 0, &o);
    return mp_obj_str_get_str(o);
}

/* The Material type roles, in t5ui_text_t order; "h4" is short for "headline4". */
static const char *const s_roles[] = { "headline4", "headline5", "headline6", "subtitle1", "body1",
                                       "body2",     "button",    "caption",   "overline" };

static t5ui_text_t role_arg(mp_obj_t r)
{
    const char *s = mp_obj_str_get_str(r);
    for (int i = 0; i < (int)MP_ARRAY_SIZE(s_roles); i++) {
        const char *n = s_roles[i];
        if (!strcmp(s, n) || (s[0] == 'h' && !strncmp(n, "headline", 8) && !strcmp(s + 1, n + 8))) {
            return (t5ui_text_t)i;
        }
    }
    mp_raise_ValueError(MP_ERROR_TEXT("role is h4, h5, h6, subtitle1, body1, body2, button, caption or overline"));
}

static const char *icon_arg(mp_obj_t o)
{
    if (o == mp_const_none || o == MP_OBJ_NULL) return NULL;
    const char *s = mp_obj_str_get_str(o);
    for (int i = 0; t5g_icons[i].name; i++) {
        if (!strcmp(s, t5g_icons[i].name)) return t5g_icons[i].glyph;
    }
    if ((unsigned char)s[0] >= 0x80) return s;    /* a glyph already */
    mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("no icon named %s"), s);
}

/* Size from a dp number, "fill" (100%), "content", or None (unchanged). */
static bool size_arg(mp_obj_t o, int32_t *out)
{
    if (o == mp_const_none || o == MP_OBJ_NULL) return false;
    if (mp_obj_is_int(o)) {
        *out = t5ui_dp(mp_obj_get_int(o));
        return true;
    }
    const char *s = mp_obj_str_get_str(o);
    if (!strcmp(s, "fill") || !strcmp(s, "100%")) *out = T5UI_PCT(100);
    else if (!strcmp(s, "content")) *out = T5UI_CONTENT;
    else mp_raise_ValueError(MP_ERROR_TEXT("size is dp, 'fill' or 'content'"));
    return true;
}

static void set_gap(t5ui_obj_t *o, int32_t px)
{
    t5ui_set_int(o, T5UI_P_PAD_ROW, px);
    t5ui_set_int(o, T5UI_P_PAD_COLUMN, px);
}

/* A bare box (T5UI_BOX): no look of its own. */
static void box_style(t5ui_obj_t *o, bool row, int gap_dp)
{
    t5ui_set_int(o, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(o, T5UI_P_HEIGHT, T5UI_CONTENT);
    t5ui_set_int(o, T5UI_P_FLOW, row ? T5UI_ROW_WRAP : T5UI_COLUMN);
    if (row) {
        t5ui_set_int(o, T5UI_P_PLACE_MAIN, T5UI_PLACE_START);
        t5ui_set_int(o, T5UI_P_PLACE_CROSS, T5UI_PLACE_CENTER);
        t5ui_set_int(o, T5UI_P_PLACE_TRACK, T5UI_PLACE_CENTER);
    }
    set_gap(o, t5ui_dp(gap_dp));
    t5ui_set_int(o, T5UI_P_SCROLLABLE, false);
}

/* A list row that reacts to taps: clickable, with Material ink. */
static void row_clickable(t5ui_obj_t *r)
{
    t5ui_set_int(r, T5UI_P_CLICKABLE, true);
    t5ui_set_int(r, T5UI_P_RIPPLE, true);
}

/* A text field: t5ui sends its taps, value changes, focus and disabled
 * state to its text area, so the field itself is the widget's inner part. */
static t5ui_obj_t *field_ta(t5ui_obj_t *f)
{
    return f;
}

/* ---- screen --------------------------------------------------------------------------------- */
static mp_obj_t ui_screen(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
    enum { A_title, A_scroll };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_title, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_scroll, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    t5_py_t *vm = py_vm;
    if (!vm || !vm->cfg.ui_parent) mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("no display for this script"));
    const char *title = a[A_title].u_obj == mp_const_none ? NULL : mp_obj_str_get_str(a[A_title].u_obj);
    py_widget_t *w = new_widget(K_BOX, MP_OBJ_NULL);
    py_ui_lock();
    if (vm->ui_root) t5ui_delete(vm->ui_root);
    t5ui_obj_t *root = t5ui_create(t5ui_sys_obj(vm->cfg.ui_parent), T5UI_BOX);
    t5ui_set_int(root, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(root, T5UI_P_HEIGHT, T5UI_PCT(100));
    t5ui_set_int(root, T5UI_P_BG_OPA, T5UI_OPA_COVER);
    t5ui_set_color(root, T5UI_P_BG_COLOR, t5ui_theme_color(T5UI_COLOR_BACKGROUND));
    t5ui_obj_t *body = root;
    if (title) {
        t5ui_set_int(root, T5UI_P_SCROLLABLE, false);
        t5ui_obj_t *page = t5ui_create(root, T5UI_MD_PAGE);
        t5ui_set_str(page, T5UI_P_TITLE, title);
        t5ui_on(page, T5UI_EV_BACK, back_cb, tag(0xffff));
        body = t5ui_get_obj(page, T5UI_P_CONTENT);
    }
    t5ui_set_int(body, T5UI_P_FLOW, T5UI_COLUMN);
    t5ui_set_int(body, T5UI_P_PAD_ALL, t5ui_dp(16));
    set_gap(body, t5ui_dp(8));
    t5ui_set_int(body, T5UI_P_SCROLLABLE, a[A_scroll].u_bool);
    vm->ui_root = root;
    vm->ui_used = true;
    attach(w, root, NULL);
    w->inner = body;
    py_ui_unlock();
    return MP_OBJ_FROM_PTR(w);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(ui_screen_obj, 0, ui_screen);

/* ---- children (container methods) ------------------------------------------------------------ */
static t5ui_obj_t *parent_of(py_widget_t *p)
{
    t5ui_obj_t *o = p->inner ? p->inner : p->obj;
    return o;
}

#define LOCK_PARENT(p, par)                  \
    py_ui_lock();                            \
    t5ui_obj_t *par = (p)->obj ? parent_of(p) : NULL; \
    if (!par) {                              \
        py_ui_unlock();                      \
        raise_deleted();                     \
    }

static mp_obj_t c_label(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
    enum { A_self, A_text, A_role, A_color, A_fn };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_text, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_QSTR(MP_QSTR_) } },
        { MP_QSTR_role, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_QSTR(MP_QSTR_body1) } },
        { MP_QSTR_color, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_fn, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    py_widget_t *p = widget(a[A_self].u_obj);
    const char *text = text_arg(a[A_text].u_obj);
    t5ui_text_t role = role_arg(a[A_role].u_obj);
    bool has_color = a[A_color].u_obj != mp_const_none;
    t5ui_color_t color = has_color ? color_arg(a[A_color].u_obj) : 0;
    py_widget_t *w = new_widget(K_LABEL, a[A_fn].u_obj);
    LOCK_PARENT(p, par);
    t5ui_obj_t *l = t5ui_create(par, T5UI_MD_TEXT);
    t5ui_set_int(l, T5UI_P_TEXT_STYLE, role);
    t5ui_set_str(l, T5UI_P_TEXT, text);
    t5ui_set_int(l, T5UI_P_LONG_MODE, T5UI_LONG_WRAP);
    t5ui_set_int(l, T5UI_P_MAX_WIDTH, T5UI_PCT(100));
    if (has_color) t5ui_set_color(l, T5UI_P_TEXT_COLOR, color);
    if (w->on_click != mp_const_none) t5ui_set_int(l, T5UI_P_CLICKABLE, true);
    attach(w, l, NULL);
    py_ui_unlock();
    return MP_OBJ_FROM_PTR(w);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(c_label_obj, 1, c_label);

static mp_obj_t c_button(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
    enum { A_self, A_text, A_variant, A_fn };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_text, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_variant, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_QSTR(MP_QSTR_contained) } },
        { MP_QSTR_fn, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    py_widget_t *p = widget(a[A_self].u_obj);
    const char *text = mp_obj_str_get_str(a[A_text].u_obj);
    const char *v = mp_obj_str_get_str(a[A_variant].u_obj);
    t5ui_kind_t var = !strcmp(v, "text") ? T5UI_MD_BUTTON_TEXT : !strcmp(v, "outlined") ? T5UI_MD_BUTTON_OUTLINED
                                                                                       : T5UI_MD_BUTTON_CONTAINED;
    py_widget_t *w = new_widget(K_BUTTON, a[A_fn].u_obj);
    LOCK_PARENT(p, par);
    t5ui_obj_t *b = t5ui_create(par, var);
    t5ui_set_str(b, T5UI_P_TEXT, text);
    attach(w, b, NULL);
    w->inner = t5ui_child(b, 0);       /* its label */
    py_ui_unlock();
    return MP_OBJ_FROM_PTR(w);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(c_button_obj, 2, c_button);

static mp_obj_t c_switch(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_on, MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_fn, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    py_widget_t *p = widget(a[0].u_obj);
    py_widget_t *w = new_widget(K_SWITCH, a[2].u_obj);
    LOCK_PARENT(p, par);
    t5ui_obj_t *s = t5ui_create(par, T5UI_MD_SWITCH);
    if (a[1].u_bool) t5ui_set_int(s, T5UI_P_CHECKED, true);
    attach(w, s, NULL);
    py_ui_unlock();
    return MP_OBJ_FROM_PTR(w);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(c_switch_obj, 1, c_switch);

static mp_obj_t c_slider(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_min, MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_max, MP_ARG_INT, { .u_int = 100 } },
        { MP_QSTR_value, MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_fn, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    py_widget_t *p = widget(a[0].u_obj);
    py_widget_t *w = new_widget(K_SLIDER, a[4].u_obj);
    LOCK_PARENT(p, par);
    int32_t lo = (int32_t)a[1].u_int, hi = (int32_t)a[2].u_int;
    t5ui_obj_t *s = t5ui_create(par, T5UI_MD_SLIDER);   /* 0..100 */
    /* The range one end at a time, never crossing the other end. */
    if (lo > 100) {
        t5ui_set_int(s, T5UI_P_MAX, hi);
        t5ui_set_int(s, T5UI_P_MIN, lo);
    } else {
        t5ui_set_int(s, T5UI_P_MIN, lo);
        t5ui_set_int(s, T5UI_P_MAX, hi);
    }
    t5ui_set_int(s, T5UI_P_VALUE, (int32_t)a[3].u_int);
    t5ui_set_int(s, T5UI_P_WIDTH, T5UI_PCT(100));
    attach(w, s, NULL);
    py_ui_unlock();
    return MP_OBJ_FROM_PTR(w);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(c_slider_obj, 1, c_slider);

static mp_obj_t c_checkbox(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_text, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_checked, MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_fn, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    py_widget_t *p = widget(a[0].u_obj);
    const char *text = mp_obj_str_get_str(a[1].u_obj);
    py_widget_t *w = new_widget(K_CHECK, a[3].u_obj);
    LOCK_PARENT(p, par);
    t5ui_obj_t *c = t5ui_create(par, T5UI_MD_CHECKBOX);
    t5ui_set_str(c, T5UI_P_TEXT, text);
    if (a[2].u_bool) t5ui_set_int(c, T5UI_P_CHECKED, true);
    attach(w, c, NULL);
    py_ui_unlock();
    return MP_OBJ_FROM_PTR(w);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(c_checkbox_obj, 2, c_checkbox);

static mp_obj_t c_textfield(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_label, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_QSTR(MP_QSTR_) } },
        { MP_QSTR_text, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_QSTR(MP_QSTR_) } },
        { MP_QSTR_password, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_fn, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    py_widget_t *p = widget(a[0].u_obj);
    const char *label = mp_obj_str_get_str(a[1].u_obj);
    const char *text = mp_obj_str_get_str(a[2].u_obj);
    py_widget_t *w = new_widget(K_FIELD, a[4].u_obj);
    LOCK_PARENT(p, par);
    t5ui_obj_t *f = t5ui_create(par, a[3].u_bool ? T5UI_MD_PASSWORD : T5UI_MD_TEXTFIELD);
    t5ui_set_str(f, T5UI_P_PLACEHOLDER, label);
    t5ui_set_int(f, T5UI_P_WIDTH, T5UI_PCT(100));
    if (text[0]) t5ui_set_str(f, T5UI_P_TEXT, text);
    t5ui_obj_t *ta = field_ta(f);
    attach(w, f, ta);
    w->inner = ta;
    py_ui_unlock();
    return MP_OBJ_FROM_PTR(w);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(c_textfield_obj, 1, c_textfield);

static mp_obj_t c_item(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_text, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_secondary, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_icon, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_fn, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    py_widget_t *p = widget(a[0].u_obj);
    const char *t1 = mp_obj_str_get_str(a[1].u_obj);
    const char *t2 = a[2].u_obj == mp_const_none ? NULL : mp_obj_str_get_str(a[2].u_obj);
    const char *icon = icon_arg(a[3].u_obj);
    py_widget_t *w = new_widget(K_ITEM, a[4].u_obj);
    LOCK_PARENT(p, par);
    t5ui_obj_t *r = t5ui_create(par, T5UI_MD_ROW);
    if (icon) t5ui_set_str(r, T5UI_P_ICON, icon);
    t5ui_set_str(r, T5UI_P_TEXT, t1);
    if (t2) {
        t5ui_set_str(r, T5UI_P_SECONDARY, t2);
        /* Two lines, growing when the second wraps (as made with both). */
        t5ui_set_int(r, T5UI_P_HEIGHT, T5UI_CONTENT);
        t5ui_set_int(r, T5UI_P_MIN_HEIGHT, t5ui_dp(64));
    }
    if (w->on_click != mp_const_none) row_clickable(r);
    attach(w, r, NULL);
    py_ui_unlock();
    return MP_OBJ_FROM_PTR(w);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(c_item_obj, 2, c_item);

static mp_obj_t c_section(mp_obj_t self, mp_obj_t title)
{
    py_widget_t *p = widget(self);
    const char *t = mp_obj_str_get_str(title);
    py_widget_t *w = new_widget(K_SECTION, MP_OBJ_NULL);
    LOCK_PARENT(p, par);
    t5ui_obj_t *s = t5ui_create(par, T5UI_MD_SECTION);
    t5ui_set_str(s, T5UI_P_TEXT, t);
    attach(w, s, NULL);
    py_ui_unlock();
    return MP_OBJ_FROM_PTR(w);
}
static MP_DEFINE_CONST_FUN_OBJ_2(c_section_obj, c_section);

static mp_obj_t c_divider(mp_obj_t self)
{
    py_widget_t *p = widget(self);
    py_widget_t *w = new_widget(K_DIVIDER, MP_OBJ_NULL);
    LOCK_PARENT(p, par);
    attach(w, t5ui_create(par, T5UI_MD_DIVIDER), NULL);
    py_ui_unlock();
    return MP_OBJ_FROM_PTR(w);
}
static MP_DEFINE_CONST_FUN_OBJ_1(c_divider_obj, c_divider);

static mp_obj_t c_icon(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_name, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_size, MP_ARG_INT, { .u_int = 24 } },
        { MP_QSTR_color, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_fn, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    py_widget_t *p = widget(a[0].u_obj);
    const char *g = icon_arg(a[1].u_obj);
    bool has_color = a[3].u_obj != mp_const_none;
    t5ui_color_t color = has_color ? color_arg(a[3].u_obj) : 0;
    py_widget_t *w = new_widget(K_ICON, a[4].u_obj);
    LOCK_PARENT(p, par);
    t5ui_obj_t *l = t5ui_create(par, T5UI_MD_ICON);
    t5ui_set_int(l, T5UI_P_ICON_SIZE, (int32_t)a[2].u_int);
    t5ui_set_str(l, T5UI_P_ICON, g);
    if (has_color) t5ui_set_color(l, T5UI_P_TEXT_COLOR, color);
    if (w->on_click != mp_const_none) t5ui_set_int(l, T5UI_P_CLICKABLE, true);
    attach(w, l, NULL);
    py_ui_unlock();
    return MP_OBJ_FROM_PTR(w);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(c_icon_obj, 2, c_icon);

static mp_obj_t make_box(mp_obj_t self, bool row, bool card, int gap, mp_obj_t fn)
{
    py_widget_t *p = widget(self);
    py_widget_t *w = new_widget(K_BOX, fn);
    LOCK_PARENT(p, par);
    t5ui_obj_t *o;
    if (card) {
        o = t5ui_create(par, T5UI_MD_CARD);
        t5ui_set_int(o, T5UI_P_WIDTH, T5UI_PCT(100));
        t5ui_set_int(o, T5UI_P_HEIGHT, T5UI_CONTENT);
        t5ui_set_int(o, T5UI_P_FLOW, T5UI_COLUMN);
        t5ui_set_int(o, T5UI_P_PAD_ALL, t5ui_dp(16));
        set_gap(o, t5ui_dp(gap));
        t5ui_set_int(o, T5UI_P_SCROLLABLE, false);
    } else {
        o = t5ui_create(par, T5UI_BOX);
        box_style(o, row, gap);
    }
    if (w->on_click != mp_const_none) t5ui_set_int(o, T5UI_P_CLICKABLE, true);
    attach(w, o, NULL);
    py_ui_unlock();
    return MP_OBJ_FROM_PTR(w);
}

static mp_obj_t c_row(size_t n, const mp_obj_t *args)
{
    return make_box(args[0], true, false, n > 1 ? (int)mp_obj_get_int(args[1]) : 8, MP_OBJ_NULL);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(c_row_obj, 1, 2, c_row);

static mp_obj_t c_column(size_t n, const mp_obj_t *args)
{
    return make_box(args[0], false, false, n > 1 ? (int)mp_obj_get_int(args[1]) : 8, MP_OBJ_NULL);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(c_column_obj, 1, 2, c_column);

static mp_obj_t c_card(size_t n, const mp_obj_t *args)
{
    return make_box(args[0], false, true, 8, n > 1 ? args[1] : MP_OBJ_NULL);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(c_card_obj, 1, 2, c_card);

static mp_obj_t c_chart(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_points, MP_ARG_INT, { .u_int = 60 } },
        { MP_QSTR_min, MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_max, MP_ARG_INT, { .u_int = 100 } },
        { MP_QSTR_height, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 140 } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    py_widget_t *p = widget(a[0].u_obj);
    mp_int_t points = a[1].u_int;
    if (points < 2 || points > 1000) mp_raise_ValueError(MP_ERROR_TEXT("points is 2..1000"));
    py_widget_t *w = new_widget(K_CHART, MP_OBJ_NULL);
    LOCK_PARENT(p, par);
    t5ui_obj_t *c = t5ui_create(par, T5UI_CHART);   /* shifting, no dots on the line */
    t5ui_set_int(c, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(c, T5UI_P_HEIGHT, t5ui_dp((int32_t)a[4].u_int));
    t5ui_set_int(c, T5UI_P_BG_OPA, T5UI_OPA_TRANSP);
    t5ui_set_int(c, T5UI_P_BORDER_WIDTH, 0);
    t5ui_set_color(c, T5UI_P_LINE_COLOR, t5ui_theme_color(T5UI_COLOR_ON_SURFACE));
    t5ui_set_int(c, T5UI_P_LINE_OPA, t5ui_theme_emphasis(T5UI_EMPHASIS_DIVIDER));
    t5ui_set_int_at(c, T5UI_P_LINE_WIDTH, T5UI_AT(T5UI_PART_ITEMS, T5UI_STATE_DEFAULT), t5ui_dp(1));
    t5ui_set_int(c, T5UI_P_CHART_TYPE, T5UI_CHART_LINE);
    t5ui_set_int(c, T5UI_P_COUNT, (int32_t)points);
    t5ui_set_int(c, T5UI_P_MIN, (int32_t)a[2].u_int);
    t5ui_set_int(c, T5UI_P_MAX, (int32_t)a[3].u_int);
    t5ui_set_int(c, T5UI_P_CHART_GRID_ROWS, 5);
    t5ui_set_int(c, T5UI_P_CHART_GRID_COLUMNS, 0);
    int32_t ser = t5ui_cmd(c, T5UI_CMD_CHART_ADD_SERIES, (int32_t)t5ui_theme_color(T5UI_COLOR_SECONDARY), 0, 0, NULL);
    w->ser = (int16_t)(ser >= 0 ? ser : -1);
    if (ser >= 0) t5ui_cmd(c, T5UI_CMD_CHART_SET_ALL, ser, T5UI_CHART_NONE, 0, NULL);
    attach(w, c, NULL);
    py_ui_unlock();
    return MP_OBJ_FROM_PTR(w);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(c_chart_obj, 1, c_chart);

static mp_obj_t c_progress(size_t n, const mp_obj_t *args)
{
    py_widget_t *p = widget(args[0]);
    int32_t v = n > 1 ? (int32_t)mp_obj_get_int(args[1]) : 0;
    py_widget_t *w = new_widget(K_BAR, MP_OBJ_NULL);
    LOCK_PARENT(p, par);
    t5ui_obj_t *b = t5ui_create(par, T5UI_BAR);   /* the indicator in the theme's primary */
    t5ui_set_int(b, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(b, T5UI_P_HEIGHT, t5ui_dp(4));
    t5ui_set_int(b, T5UI_P_MIN, 0);
    t5ui_set_int(b, T5UI_P_MAX, 100);
    t5ui_set_int(b, T5UI_P_VALUE, v);
    attach(w, b, NULL);
    py_ui_unlock();
    return MP_OBJ_FROM_PTR(w);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(c_progress_obj, 1, 2, c_progress);

/* ---- widget methods ------------------------------------------------------------------------------ */
#define LOCK_W(w)          \
    py_ui_lock();          \
    if (!(w)->obj) {       \
        py_ui_unlock();    \
        raise_deleted();   \
    }

static mp_obj_t w_set_text(mp_obj_t self, mp_obj_t text)
{
    py_widget_t *w = widget(self);
    const char *t = text_arg(text);
    LOCK_W(w);
    switch (w->kind) {
    case K_LABEL:
    case K_FIELD:
    case K_ITEM:
    case K_CHECK: t5ui_set_str(w->obj, T5UI_P_TEXT, t); break;
    case K_BUTTON: if (w->inner) t5ui_set_str(w->inner, T5UI_P_TEXT, t); break;
    default: break;
    }
    py_ui_unlock();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(w_set_text_obj, w_set_text);

static mp_obj_t w_text(mp_obj_t self)
{
    py_widget_t *w = widget(self);
    char buf[512];
    buf[0] = 0;
    LOCK_W(w);
    const char *t = NULL;
    switch (w->kind) {
    case K_LABEL:
    case K_FIELD:
    case K_CHECK: t = t5ui_get_str(w->obj, T5UI_P_TEXT, NULL); break;
    case K_BUTTON: t = w->inner ? t5ui_get_str(w->inner, T5UI_P_TEXT, NULL) : NULL; break;
    default: break;
    }
    if (t) snprintf(buf, sizeof buf, "%s", t);
    py_ui_unlock();
    return mp_obj_new_str_from_cstr(buf);
}
static MP_DEFINE_CONST_FUN_OBJ_1(w_text_obj, w_text);

static mp_obj_t w_set_secondary(mp_obj_t self, mp_obj_t text)
{
    py_widget_t *w = widget(self);
    const char *t = mp_obj_str_get_str(text);
    LOCK_W(w);
    if (w->kind == K_ITEM) t5ui_set_str(w->obj, T5UI_P_SECONDARY, t);
    py_ui_unlock();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(w_set_secondary_obj, w_set_secondary);

static mp_obj_t w_value(mp_obj_t self)
{
    py_widget_t *w = widget(self);
    if (w->kind == K_FIELD) return w_text(self);
    int32_t v = 0;
    bool is_bool = false;
    LOCK_W(w);
    switch (w->kind) {
    case K_SWITCH:
    case K_CHECK: v = t5ui_get_int(w->obj, T5UI_P_CHECKED, 0); is_bool = true; break;
    case K_SLIDER:
    case K_BAR: v = t5ui_get_int(w->obj, T5UI_P_VALUE, 0); break;
    default: break;
    }
    py_ui_unlock();
    return is_bool ? mp_obj_new_bool(v) : mp_obj_new_int(v);
}
static MP_DEFINE_CONST_FUN_OBJ_1(w_value_obj, w_value);

static mp_obj_t w_set_value(mp_obj_t self, mp_obj_t val)
{
    py_widget_t *w = widget(self);
    if (w->kind == K_FIELD) return w_set_text(self, val);
    if (w->kind == K_CHART) mp_raise_TypeError(MP_ERROR_TEXT("use push() or set() on a chart"));
    mp_int_t v = mp_obj_get_int(val);      /* True/False are 1/0 */
    LOCK_W(w);
    switch (w->kind) {
    case K_SWITCH:
    case K_CHECK: t5ui_set_int(w->obj, T5UI_P_CHECKED, v != 0); break;
    case K_SLIDER:
    case K_BAR: t5ui_set_int(w->obj, T5UI_P_VALUE, (int32_t)v); break;
    default: break;
    }
    py_ui_unlock();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(w_set_value_obj, w_set_value);

static mp_obj_t w_push(mp_obj_t self, mp_obj_t val)
{
    py_widget_t *w = widget(self);
    if (w->kind != K_CHART) mp_raise_TypeError(MP_ERROR_TEXT("push() is for charts"));
    int32_t v = val == mp_const_none ? T5UI_CHART_NONE : (int32_t)mp_obj_get_float(val);
    LOCK_W(w);
    if (w->ser >= 0) t5ui_cmd(w->obj, T5UI_CMD_CHART_PUSH, w->ser, v, 0, NULL);
    py_ui_unlock();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(w_push_obj, w_push);

static mp_obj_t w_set_points(mp_obj_t self, mp_obj_t seq)
{
    py_widget_t *w = widget(self);
    if (w->kind != K_CHART) mp_raise_TypeError(MP_ERROR_TEXT("set() is for charts"));
    size_t n;
    mp_obj_t *items;
    mp_obj_get_array(seq, &n, &items);
    int32_t *vals = m_new(int32_t, n ? n : 1);
    for (size_t i = 0; i < n; i++) vals[i] = items[i] == mp_const_none ? T5UI_CHART_NONE : (int32_t)mp_obj_get_float(items[i]);
    LOCK_W(w);
    if (w->ser >= 0) {
        /* The newest values at the right end; older points stay empty. */
        uint32_t cnt = (uint32_t)t5ui_get_int(w->obj, T5UI_P_COUNT, 0);
        t5ui_cmd(w->obj, T5UI_CMD_CHART_SET_ALL, w->ser, T5UI_CHART_NONE, 0, NULL);
        uint32_t off = n < cnt ? cnt - (uint32_t)n : 0;
        for (uint32_t i = 0; i + off < cnt && i < n; i++) {
            t5ui_cmd(w->obj, T5UI_CMD_CHART_SET, w->ser, (int32_t)(off + i), vals[n > cnt ? n - cnt + i : i], NULL);
        }
        t5ui_cmd(w->obj, T5UI_CMD_CHART_X_START, w->ser, 0, 0, NULL);
    }
    py_ui_unlock();
    m_del(int32_t, vals, n ? n : 1);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(w_set_points_obj, w_set_points);

static mp_obj_t w_on_click(mp_obj_t self, mp_obj_t fn)
{
    py_widget_t *w = widget(self);
    w->on_click = fn;
    LOCK_W(w);
    if (fn != mp_const_none) {
        t5ui_set_int(w->obj, T5UI_P_CLICKABLE, true);
        if (w->kind == K_ITEM) row_clickable(w->obj);
    }
    py_ui_unlock();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(w_on_click_obj, w_on_click);

static mp_obj_t w_on_change(mp_obj_t self, mp_obj_t fn)
{
    widget(self)->on_change = fn;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(w_on_change_obj, w_on_change);

static mp_obj_t w_set_hidden(mp_obj_t self, mp_obj_t hide)
{
    py_widget_t *w = widget(self);
    bool h = mp_obj_is_true(hide);
    LOCK_W(w);
    t5ui_set_int(w->obj, T5UI_P_HIDDEN, h);
    py_ui_unlock();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(w_set_hidden_obj, w_set_hidden);

static mp_obj_t w_enable(mp_obj_t self, mp_obj_t on)
{
    py_widget_t *w = widget(self);
    bool e = mp_obj_is_true(on);
    LOCK_W(w);
    t5ui_obj_t *o = w->kind == K_FIELD && w->inner ? w->inner : w->obj;
    t5ui_set_int(o, T5UI_P_DISABLED, !e);
    py_ui_unlock();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(w_enable_obj, w_enable);

static mp_obj_t w_delete(mp_obj_t self)
{
    py_widget_t *w = widget(self);
    py_ui_lock();
    if (w->obj) {
        if (py_vm && w->obj == py_vm->ui_root) py_vm->ui_root = NULL;
        t5ui_delete(w->obj);         /* the DELETE event clears w->obj and the slot */
    }
    py_ui_unlock();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(w_delete_obj, w_delete);

static mp_obj_t w_clear(mp_obj_t self)
{
    py_widget_t *w = widget(self);
    LOCK_W(w);
    t5ui_clean(parent_of(w));
    py_ui_unlock();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(w_clear_obj, w_clear);

static mp_obj_t w_size(size_t n, const mp_obj_t *args)
{
    py_widget_t *w = widget(args[0]);
    int32_t ww = 0, hh = 0;
    bool sw = n > 1 && size_arg(args[1], &ww);
    bool sh = n > 2 && size_arg(args[2], &hh);
    LOCK_W(w);
    if (sw) t5ui_set_int(w->obj, T5UI_P_WIDTH, ww);
    if (sh) t5ui_set_int(w->obj, T5UI_P_HEIGHT, hh);
    py_ui_unlock();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(w_size_obj, 2, 3, w_size);

static mp_obj_t w_grow(size_t n, const mp_obj_t *args)
{
    py_widget_t *w = widget(args[0]);
    uint8_t g = n > 1 ? (uint8_t)mp_obj_get_int(args[1]) : 1;
    LOCK_W(w);
    t5ui_set_int(w->obj, T5UI_P_GROW, g);
    py_ui_unlock();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(w_grow_obj, 1, 2, w_grow);

static mp_obj_t w_color(mp_obj_t self, mp_obj_t c)
{
    py_widget_t *w = widget(self);
    t5ui_color_t rgb = color_arg(c);
    LOCK_W(w);
    if (w->kind == K_CHART && w->ser >= 0) t5ui_cmd(w->obj, T5UI_CMD_CHART_COLOR, w->ser, (int32_t)rgb, 0, NULL);
    else if (w->kind == K_BAR) t5ui_set_color_at(w->obj, T5UI_P_BG_COLOR, T5UI_AT(T5UI_PART_INDICATOR, T5UI_STATE_DEFAULT), rgb);
    else t5ui_set_color(w->obj, T5UI_P_TEXT_COLOR, rgb);
    py_ui_unlock();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(w_color_obj, w_color);

static mp_obj_t w_bg(mp_obj_t self, mp_obj_t c)
{
    py_widget_t *w = widget(self);
    t5ui_color_t rgb = color_arg(c);
    LOCK_W(w);
    t5ui_set_int(w->obj, T5UI_P_BG_OPA, T5UI_OPA_COVER);
    t5ui_set_color(w->obj, T5UI_P_BG_COLOR, rgb);
    py_ui_unlock();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(w_bg_obj, w_bg);

static mp_obj_t w_focus(mp_obj_t self)
{
    py_widget_t *w = widget(self);
    LOCK_W(w);
    if (w->kind == K_FIELD && w->inner) {
        t5ui_cmd(w->inner, T5UI_CMD_FOCUS, 0, 0, 0, NULL);
        t5ui_set_int(w->inner, T5UI_P_STATE, t5ui_get_int(w->inner, T5UI_P_STATE, 0) | T5UI_STATE_FOCUSED);
        t5_shell_keyboard_show(t5ui_sys_native_input(w->inner));
    }
    py_ui_unlock();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(w_focus_obj, w_focus);

static mp_obj_t w_alive(mp_obj_t self)
{
    return mp_obj_new_bool(widget(self)->obj != NULL);
}
static MP_DEFINE_CONST_FUN_OBJ_1(w_alive_obj, w_alive);

static void w_print(const mp_print_t *print, mp_obj_t o, mp_print_kind_t kind)
{
    (void)kind;
    static const char *const names[] = { "box", "label", "button", "switch", "slider", "checkbox", "textfield", "item",
                                         "section", "divider", "icon", "chart", "progress" };
    py_widget_t *w = MP_OBJ_TO_PTR(o);
    mp_printf(print, "<%s%s>", w->kind < MP_ARRAY_SIZE(names) ? names[w->kind] : "widget", w->obj ? "" : " deleted");
}

static const mp_rom_map_elem_t widget_locals_table[] = {
    /* containers */
    { MP_ROM_QSTR(MP_QSTR_label), MP_ROM_PTR(&c_label_obj) },
    { MP_ROM_QSTR(MP_QSTR_button), MP_ROM_PTR(&c_button_obj) },
    { MP_ROM_QSTR(MP_QSTR_switch), MP_ROM_PTR(&c_switch_obj) },
    { MP_ROM_QSTR(MP_QSTR_slider), MP_ROM_PTR(&c_slider_obj) },
    { MP_ROM_QSTR(MP_QSTR_checkbox), MP_ROM_PTR(&c_checkbox_obj) },
    { MP_ROM_QSTR(MP_QSTR_textfield), MP_ROM_PTR(&c_textfield_obj) },
    { MP_ROM_QSTR(MP_QSTR_item), MP_ROM_PTR(&c_item_obj) },
    { MP_ROM_QSTR(MP_QSTR_section), MP_ROM_PTR(&c_section_obj) },
    { MP_ROM_QSTR(MP_QSTR_divider), MP_ROM_PTR(&c_divider_obj) },
    { MP_ROM_QSTR(MP_QSTR_icon), MP_ROM_PTR(&c_icon_obj) },
    { MP_ROM_QSTR(MP_QSTR_row), MP_ROM_PTR(&c_row_obj) },
    { MP_ROM_QSTR(MP_QSTR_column), MP_ROM_PTR(&c_column_obj) },
    { MP_ROM_QSTR(MP_QSTR_card), MP_ROM_PTR(&c_card_obj) },
    { MP_ROM_QSTR(MP_QSTR_chart), MP_ROM_PTR(&c_chart_obj) },
    { MP_ROM_QSTR(MP_QSTR_progress), MP_ROM_PTR(&c_progress_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&w_clear_obj) },
    /* any widget */
    { MP_ROM_QSTR(MP_QSTR_set_text), MP_ROM_PTR(&w_set_text_obj) },
    { MP_ROM_QSTR(MP_QSTR_text), MP_ROM_PTR(&w_text_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_secondary), MP_ROM_PTR(&w_set_secondary_obj) },
    { MP_ROM_QSTR(MP_QSTR_value), MP_ROM_PTR(&w_value_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_value), MP_ROM_PTR(&w_set_value_obj) },
    { MP_ROM_QSTR(MP_QSTR_push), MP_ROM_PTR(&w_push_obj) },
    { MP_ROM_QSTR(MP_QSTR_set), MP_ROM_PTR(&w_set_points_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_click), MP_ROM_PTR(&w_on_click_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_change), MP_ROM_PTR(&w_on_change_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_hidden), MP_ROM_PTR(&w_set_hidden_obj) },
    { MP_ROM_QSTR(MP_QSTR_enable), MP_ROM_PTR(&w_enable_obj) },
    { MP_ROM_QSTR(MP_QSTR_delete), MP_ROM_PTR(&w_delete_obj) },
    { MP_ROM_QSTR(MP_QSTR_size), MP_ROM_PTR(&w_size_obj) },
    { MP_ROM_QSTR(MP_QSTR_grow), MP_ROM_PTR(&w_grow_obj) },
    { MP_ROM_QSTR(MP_QSTR_color), MP_ROM_PTR(&w_color_obj) },
    { MP_ROM_QSTR(MP_QSTR_bg), MP_ROM_PTR(&w_bg_obj) },
    { MP_ROM_QSTR(MP_QSTR_focus), MP_ROM_PTR(&w_focus_obj) },
    { MP_ROM_QSTR(MP_QSTR_alive), MP_ROM_PTR(&w_alive_obj) },
};
static MP_DEFINE_CONST_DICT(widget_locals, widget_locals_table);
MP_DEFINE_CONST_OBJ_TYPE(py_type_widget, MP_QSTR_Widget, MP_TYPE_FLAG_NONE, print, w_print, locals_dict,
                         &widget_locals);

/* ---- module functions --------------------------------------------------------------------------- */
static void snack_cb(void *ud)
{
    mp_obj_t fn = py_slot_take((uintptr_t)ud);
    if (fn != MP_OBJ_NULL) py_schedule(fn, mp_const_none);
}

static mp_obj_t ui_snackbar(size_t n, const mp_obj_t *args)
{
    const char *text = mp_obj_str_get_str(args[0]);
    const char *action = n > 1 && args[1] != mp_const_none ? mp_obj_str_get_str(args[1]) : NULL;
    uintptr_t h = 0;
    bool cb = action && n > 2 && args[2] != mp_const_none;
    if (cb) h = py_slot_put(args[2]);
    py_ui_lock();
    t5ui_snackbar(text, action, cb ? snack_cb : NULL, (void *)h);
    py_ui_unlock();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ui_snackbar_obj, 1, 3, ui_snackbar);

static void dialog_cb(int button, void *ud)
{
    mp_obj_t fn = py_slot_take((uintptr_t)ud);
    if (fn != MP_OBJ_NULL) py_schedule(fn, mp_obj_new_bool(button == 0));
}

static mp_obj_t ui_dialog(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
    static const mp_arg_t allowed[] = {
        { MP_QSTR_title, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_text, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_QSTR(MP_QSTR_) } },
        { MP_QSTR_ok, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_QSTR(MP_QSTR_OK) } },
        { MP_QSTR_cancel, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_fn, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    const char *title = mp_obj_str_get_str(a[0].u_obj);
    const char *text = mp_obj_str_get_str(a[1].u_obj);
    const char *ok = mp_obj_str_get_str(a[2].u_obj);
    const char *cancel = a[3].u_obj == mp_const_none ? NULL : mp_obj_str_get_str(a[3].u_obj);
    uintptr_t h = a[4].u_obj == mp_const_none ? 0 : py_slot_put(a[4].u_obj);
    py_ui_lock();
    t5ui_dialog(title, text, ok, cancel, h ? dialog_cb : NULL, (void *)h);
    py_ui_unlock();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(ui_dialog_obj, 1, ui_dialog);

static mp_obj_t ui_dp(mp_obj_t n)
{
    return mp_obj_new_int(t5ui_dp(mp_obj_get_int(n)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ui_dp_obj, ui_dp);

static mp_obj_t ui_size(void)
{
    py_ui_lock();
    int32_t w = 0, h = 0;
    t5ui_screen_size(&w, &h);
    py_ui_unlock();
    mp_obj_t t[2] = { MP_OBJ_NEW_SMALL_INT(w / t5ui_dp(1)), MP_OBJ_NEW_SMALL_INT(h / t5ui_dp(1)) };
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_0(ui_size_obj, ui_size);

static mp_obj_t ui_icon(mp_obj_t name)
{
    return mp_obj_new_str_from_cstr(icon_arg(name));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ui_icon_obj, ui_icon);

static const mp_rom_map_elem_t ui_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_ui) },
    T5_GEN_UI_GLOBALS
    { MP_ROM_QSTR(MP_QSTR_screen), MP_ROM_PTR(&ui_screen_obj) },
    { MP_ROM_QSTR(MP_QSTR_snackbar), MP_ROM_PTR(&ui_snackbar_obj) },
    { MP_ROM_QSTR(MP_QSTR_dialog), MP_ROM_PTR(&ui_dialog_obj) },
    { MP_ROM_QSTR(MP_QSTR_dp), MP_ROM_PTR(&ui_dp_obj) },
    { MP_ROM_QSTR(MP_QSTR_size), MP_ROM_PTR(&ui_size_obj) },
    { MP_ROM_QSTR(MP_QSTR_icon), MP_ROM_PTR(&ui_icon_obj) },
};
static MP_DEFINE_CONST_DICT(ui_globals, ui_globals_table);
const mp_obj_module_t py_mod_ui = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&ui_globals };
