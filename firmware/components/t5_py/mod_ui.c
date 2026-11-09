/* SPDX-License-Identifier: Apache-2.0
 * t5.ui (plan 8.3): a light wrapper over the MD2 widgets.
 *
 *   s = t5.ui.screen("Air Log")          # app bar with a back arrow
 *   temp = s.label("--", role="h4")
 *   s.button("Refresh", fn=lambda b: refresh())
 *   row = s.row()
 *   row.switch(True, fn=lambda w: print(w.value()))
 *
 * Every LVGL call is made from the VM task under the UI lock. Taps and value
 * changes arrive on the UI task and are queued for the VM task with
 * micropython.schedule semantics, so callbacks run between bytecodes.
 *
 * LVGL never holds a pointer into the Python heap: widgets are found through
 * a table indexed by a slot number tagged with the VM's generation, and the
 * generation changes (under the UI lock) before the heap goes away. A late
 * event finds nothing.
 *
 * Sizes are in dp (2 px each). Colours are 0xRRGGBB or a scheme name
 * ("primary", "secondary", "error", "on_surface", "surface", "background").
 */
#include <string.h>
#include "esp_timer.h"
#include "lvgl.h"
#include "md/md.h"
#include "md/md_tokens.h"
#include "py/mphal.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "t5_app/app.h"
#include "t5_shell/shell.h"
#include "t5_ui/t5_ui.h"
#include "bind_gen.h"
#include "py_priv.h"

enum { K_BOX, K_LABEL, K_BUTTON, K_SWITCH, K_SLIDER, K_CHECK, K_FIELD, K_ITEM, K_SECTION, K_DIVIDER, K_ICON, K_CHART,
       K_BAR };

typedef struct {
    mp_obj_base_t base;
    lv_obj_t *obj;              /* NULL once LVGL deleted it */
    lv_obj_t *inner;            /* children go here (boxes), the text area (fields), the label (buttons) */
    lv_chart_series_t *ser;
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
static void ev_cb(lv_event_t *e)
{
    py_widget_t *w = untag(lv_event_get_user_data(e));
    if (!w) return;
    switch (lv_event_get_code(e)) {
    case LV_EVENT_DELETE:
        if (lv_event_get_target_obj(e) != w->obj) return;   /* an inner part */
        w->obj = NULL;
        w->inner = NULL;
        WTAB[w->slot] = NULL;
        break;
    case LV_EVENT_CLICKED:
        if (w->on_click != mp_const_none) py_schedule(w->on_click, MP_OBJ_FROM_PTR(w));
        break;
    case LV_EVENT_VALUE_CHANGED:
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

static void back_cb(lv_event_t *e)
{
    t5_py_t *vm = py_vm;
    if (py_untag((uintptr_t)lv_event_get_user_data(e)) < 0) return;
    if (vm->cfg.repl) {
        /* At the prompt: back just takes the script's UI away. */
        if (vm->ui_root) lv_obj_delete_async(vm->ui_root);
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
static void attach(py_widget_t *w, lv_obj_t *obj, lv_obj_t *events_on)
{
    uint32_t i = 0;
    while (i < s_wcap && WTAB[i]) i++;
    w->slot = (uint16_t)i;
    w->obj = obj;
    WTAB[i] = w;
    void *t = tag(i);
    lv_obj_add_event_cb(obj, ev_cb, LV_EVENT_DELETE, t);
    lv_obj_t *src = events_on ? events_on : obj;
    lv_obj_add_event_cb(src, ev_cb, LV_EVENT_CLICKED, t);
    lv_obj_add_event_cb(src, ev_cb, LV_EVENT_VALUE_CHANGED, t);
}

static py_widget_t *new_widget(uint8_t kind, mp_obj_t fn)
{
    py_widget_t *w = mp_obj_malloc(py_widget_t, &py_type_widget);
    w->obj = w->inner = NULL;
    w->ser = NULL;
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

static md_rgb_t color_arg(mp_obj_t c)
{
    if (mp_obj_is_int(c)) return (md_rgb_t)mp_obj_get_int(c) & 0xffffff;
    const char *s = mp_obj_str_get_str(c);
    const md_scheme_t *sc = md_scheme();
    if (!strcmp(s, "primary")) return sc->c.primary;
    if (!strcmp(s, "secondary")) return sc->c.secondary;
    if (!strcmp(s, "error")) return sc->c.error;
    if (!strcmp(s, "surface")) return sc->c.surface;
    if (!strcmp(s, "background")) return sc->c.background;
    if (!strcmp(s, "on_surface")) return sc->c.on_surface;
    if (!strcmp(s, "on_primary")) return sc->c.on_primary;
    mp_raise_ValueError(MP_ERROR_TEXT("colour is 0xRRGGBB or a scheme name"));
}

/* Any value as text: str(x) for non-strings. */
static const char *text_arg(mp_obj_t o)
{
    if (!mp_obj_is_str(o)) o = mp_obj_str_make_new(&mp_type_str, 1, 0, &o);
    return mp_obj_str_get_str(o);
}

/* md's role names; "h4" is short for "headline4". */
static md_type_role_t role_arg(mp_obj_t r)
{
    const char *s = mp_obj_str_get_str(r);
    for (int i = 0; i < MD_TYPE_COUNT; i++) {
        const char *n = md_type((md_type_role_t)i)->name;
        if (!strcmp(s, n) || (s[0] == 'h' && !strncmp(n, "headline", 8) && !strcmp(s + 1, n + 8))) {
            return (md_type_role_t)i;
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
        *out = T5_DP(mp_obj_get_int(o));
        return true;
    }
    const char *s = mp_obj_str_get_str(o);
    if (!strcmp(s, "fill") || !strcmp(s, "100%")) *out = LV_PCT(100);
    else if (!strcmp(s, "content")) *out = LV_SIZE_CONTENT;
    else mp_raise_ValueError(MP_ERROR_TEXT("size is dp, 'fill' or 'content'"));
    return true;
}

static void box_style(lv_obj_t *o, bool row, int gap_dp)
{
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, row ? LV_FLEX_FLOW_ROW_WRAP : LV_FLEX_FLOW_COLUMN);
    if (row) lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(o, T5_DP(gap_dp), 0);
    lv_obj_set_scrollable(o, false);
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
    if (vm->ui_root) lv_obj_delete(vm->ui_root);
    lv_obj_t *root = lv_obj_create(vm->cfg.ui_parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, md_color(md_scheme()->c.background), 0);
    lv_obj_t *body = root;
    if (title) {
        lv_obj_set_scrollable(root, false);
        lv_obj_t *page = md_page_create(root, title, back_cb, tag(0xffff), NULL);
        body = md_page_body(page);
    }
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, T5_DP(16), 0);
    lv_obj_set_style_pad_gap(body, T5_DP(8), 0);
    lv_obj_set_scrollable(body, a[A_scroll].u_bool);
    vm->ui_root = root;
    vm->ui_used = true;
    attach(w, root, NULL);
    w->inner = body;
    py_ui_unlock();
    return MP_OBJ_FROM_PTR(w);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(ui_screen_obj, 0, ui_screen);

/* ---- children (container methods) ------------------------------------------------------------ */
static lv_obj_t *parent_of(py_widget_t *p)
{
    lv_obj_t *o = p->inner ? p->inner : p->obj;
    return o;
}

#define LOCK_PARENT(p, par)                  \
    py_ui_lock();                            \
    lv_obj_t *par = (p)->obj ? parent_of(p) : NULL; \
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
    md_type_role_t role = role_arg(a[A_role].u_obj);
    bool has_color = a[A_color].u_obj != mp_const_none;
    md_rgb_t color = has_color ? color_arg(a[A_color].u_obj) : 0;
    py_widget_t *w = new_widget(K_LABEL, a[A_fn].u_obj);
    LOCK_PARENT(p, par);
    lv_obj_t *l = md_label_create(par, text, role);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_max_width(l, LV_PCT(100), 0);
    if (has_color) lv_obj_set_style_text_color(l, md_color(color), 0);
    if (w->on_click != mp_const_none) lv_obj_set_clickable(l, true);
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
    md_button_variant_t var = !strcmp(v, "text") ? MD_BUTTON_TEXT : !strcmp(v, "outlined") ? MD_BUTTON_OUTLINED
                                                                                           : MD_BUTTON_CONTAINED;
    py_widget_t *w = new_widget(K_BUTTON, a[A_fn].u_obj);
    LOCK_PARENT(p, par);
    lv_obj_t *b = md_button_create(par, text, var);
    attach(w, b, NULL);
    w->inner = lv_obj_get_child(b, 0);
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
    attach(w, md_switch_create(par, a[1].u_bool), NULL);
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
    lv_obj_t *s = md_slider_create(par, (int32_t)a[1].u_int, (int32_t)a[2].u_int, (int32_t)a[3].u_int);
    lv_obj_set_width(s, LV_PCT(100));
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
    attach(w, md_checkbox_create(par, text, a[2].u_bool), NULL);
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
    lv_obj_t *f = md_textfield_create(par, label, a[3].u_bool);
    lv_obj_set_width(f, LV_PCT(100));
    if (text[0]) md_textfield_set_text(f, text);
    lv_obj_t *ta = md_textfield_ta(f);
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
    lv_obj_t *r = md_row_create(par, icon, t1, t2);
    if (w->on_click != mp_const_none) md_row_set_clickable(r, NULL, NULL);
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
    attach(w, md_section_create(par, t), NULL);
    py_ui_unlock();
    return MP_OBJ_FROM_PTR(w);
}
static MP_DEFINE_CONST_FUN_OBJ_2(c_section_obj, c_section);

static mp_obj_t c_divider(mp_obj_t self)
{
    py_widget_t *p = widget(self);
    py_widget_t *w = new_widget(K_DIVIDER, MP_OBJ_NULL);
    LOCK_PARENT(p, par);
    attach(w, md_divider_create(par), NULL);
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
    md_rgb_t color = has_color ? color_arg(a[3].u_obj) : 0;
    py_widget_t *w = new_widget(K_ICON, a[4].u_obj);
    LOCK_PARENT(p, par);
    lv_obj_t *l = md_icon_create(par, g, (int)a[2].u_int);
    if (has_color) lv_obj_set_style_text_color(l, md_color(color), 0);
    if (w->on_click != mp_const_none) lv_obj_set_clickable(l, true);
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
    lv_obj_t *o;
    if (card) {
        o = md_card_create(par);
        lv_obj_set_size(o, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(o, T5_DP(16), 0);
        lv_obj_set_style_pad_gap(o, T5_DP(gap), 0);
        lv_obj_set_scrollable(o, false);
    } else {
        o = lv_obj_create(par);
        box_style(o, row, gap);
    }
    if (w->on_click != mp_const_none) lv_obj_set_clickable(o, true);
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
    lv_obj_t *c = lv_chart_create(par);
    lv_obj_set_size(c, LV_PCT(100), T5_DP(a[4].u_int));
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_line_color(c, md_color(md_scheme()->c.on_surface), LV_PART_MAIN);
    lv_obj_set_style_line_opa(c, md_scheme()->on_surface.divider, LV_PART_MAIN);
    lv_obj_set_style_size(c, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(c, T5_DP(1), LV_PART_ITEMS);
    lv_chart_set_type(c, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(c, (uint32_t)points);
    lv_chart_set_axis_range(c, LV_CHART_AXIS_PRIMARY_Y, (int32_t)a[2].u_int, (int32_t)a[3].u_int);
    lv_chart_set_div_line_count(c, 5, 0);
    lv_chart_set_update_mode(c, LV_CHART_UPDATE_MODE_SHIFT);
    w->ser = lv_chart_add_series(c, md_color(md_scheme()->c.secondary), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(c, w->ser, LV_CHART_POINT_NONE);
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
    lv_obj_t *b = lv_bar_create(par);
    lv_obj_set_size(b, LV_PCT(100), T5_DP(4));
    lv_bar_set_range(b, 0, 100);
    lv_bar_set_value(b, v, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(b, md_color(md_scheme()->c.primary), LV_PART_INDICATOR);
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
    case K_LABEL: lv_label_set_text(w->obj, t); break;
    case K_BUTTON: if (w->inner) lv_label_set_text(w->inner, t); break;
    case K_FIELD: md_textfield_set_text(w->obj, t); break;
    case K_ITEM: md_row_set_text(w->obj, t); break;
    case K_CHECK: lv_checkbox_set_text(w->obj, t); break;
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
    case K_LABEL: t = lv_label_get_text(w->obj); break;
    case K_BUTTON: t = w->inner ? lv_label_get_text(w->inner) : NULL; break;
    case K_FIELD: t = md_textfield_text(w->obj); break;
    case K_CHECK: t = lv_checkbox_get_text(w->obj); break;
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
    if (w->kind == K_ITEM) md_row_set_secondary(w->obj, t);
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
    case K_CHECK: v = lv_obj_has_state(w->obj, LV_STATE_CHECKED); is_bool = true; break;
    case K_SLIDER: v = lv_slider_get_value(w->obj); break;
    case K_BAR: v = lv_bar_get_value(w->obj); break;
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
    case K_CHECK:
        if (v) lv_obj_add_state(w->obj, LV_STATE_CHECKED);
        else lv_obj_remove_state(w->obj, LV_STATE_CHECKED);
        break;
    case K_SLIDER: lv_slider_set_value(w->obj, (int32_t)v, LV_ANIM_OFF); break;
    case K_BAR: lv_bar_set_value(w->obj, (int32_t)v, LV_ANIM_OFF); break;
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
    int32_t v = val == mp_const_none ? LV_CHART_POINT_NONE : (int32_t)mp_obj_get_float(val);
    LOCK_W(w);
    lv_chart_set_next_value(w->obj, w->ser, v);
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
    for (size_t i = 0; i < n; i++) vals[i] = items[i] == mp_const_none ? LV_CHART_POINT_NONE : (int32_t)mp_obj_get_float(items[i]);
    LOCK_W(w);
    uint32_t cnt = lv_chart_get_point_count(w->obj);
    lv_chart_set_all_values(w->obj, w->ser, LV_CHART_POINT_NONE);
    int32_t *y = lv_chart_get_series_y_array(w->obj, w->ser);
    uint32_t off = n < cnt ? cnt - (uint32_t)n : 0;
    for (uint32_t i = 0; i + off < cnt && i < n; i++) y[off + i] = vals[n > cnt ? n - cnt + i : i];
    lv_chart_set_x_start_point(w->obj, w->ser, 0);
    lv_chart_refresh(w->obj);
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
        lv_obj_set_clickable(w->obj, true);
        if (w->kind == K_ITEM) md_row_set_clickable(w->obj, NULL, NULL);
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
    lv_obj_set_hidden(w->obj, h);
    py_ui_unlock();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(w_set_hidden_obj, w_set_hidden);

static mp_obj_t w_enable(mp_obj_t self, mp_obj_t on)
{
    py_widget_t *w = widget(self);
    bool e = mp_obj_is_true(on);
    LOCK_W(w);
    lv_obj_t *o = w->kind == K_FIELD && w->inner ? w->inner : w->obj;
    if (e) lv_obj_remove_state(o, LV_STATE_DISABLED);
    else lv_obj_add_state(o, LV_STATE_DISABLED);
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
        lv_obj_delete(w->obj);       /* the DELETE event clears w->obj and the slot */
    }
    py_ui_unlock();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(w_delete_obj, w_delete);

static mp_obj_t w_clear(mp_obj_t self)
{
    py_widget_t *w = widget(self);
    LOCK_W(w);
    lv_obj_clean(parent_of(w));
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
    if (sw) lv_obj_set_width(w->obj, ww);
    if (sh) lv_obj_set_height(w->obj, hh);
    py_ui_unlock();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(w_size_obj, 2, 3, w_size);

static mp_obj_t w_grow(size_t n, const mp_obj_t *args)
{
    py_widget_t *w = widget(args[0]);
    uint8_t g = n > 1 ? (uint8_t)mp_obj_get_int(args[1]) : 1;
    LOCK_W(w);
    lv_obj_set_flex_grow(w->obj, g);
    py_ui_unlock();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(w_grow_obj, 1, 2, w_grow);

static mp_obj_t w_color(mp_obj_t self, mp_obj_t c)
{
    py_widget_t *w = widget(self);
    md_rgb_t rgb = color_arg(c);
    LOCK_W(w);
    if (w->kind == K_CHART && w->ser) lv_chart_set_series_color(w->obj, w->ser, md_color(rgb));
    else if (w->kind == K_BAR) lv_obj_set_style_bg_color(w->obj, md_color(rgb), LV_PART_INDICATOR);
    else lv_obj_set_style_text_color(w->obj, md_color(rgb), 0);
    py_ui_unlock();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(w_color_obj, w_color);

static mp_obj_t w_bg(mp_obj_t self, mp_obj_t c)
{
    py_widget_t *w = widget(self);
    md_rgb_t rgb = color_arg(c);
    LOCK_W(w);
    lv_obj_set_style_bg_opa(w->obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(w->obj, md_color(rgb), 0);
    py_ui_unlock();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(w_bg_obj, w_bg);

static mp_obj_t w_focus(mp_obj_t self)
{
    py_widget_t *w = widget(self);
    LOCK_W(w);
    if (w->kind == K_FIELD && w->inner) {
        lv_group_t *g = lv_group_get_default();
        if (g) lv_group_focus_obj(w->inner);
        lv_obj_add_state(w->inner, LV_STATE_FOCUSED);
        t5_shell_keyboard_show(w->inner);
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
    md_snackbar_show(text, action, cb ? snack_cb : NULL, (void *)h);
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
    md_dialog_show(title, text, ok, cancel, h ? dialog_cb : NULL, (void *)h);
    py_ui_unlock();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(ui_dialog_obj, 1, ui_dialog);

static mp_obj_t ui_dp(mp_obj_t n)
{
    return mp_obj_new_int(T5_DP(mp_obj_get_int(n)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ui_dp_obj, ui_dp);

static mp_obj_t ui_size(void)
{
    py_ui_lock();
    int32_t w = lv_display_get_horizontal_resolution(NULL), h = lv_display_get_vertical_resolution(NULL);
    py_ui_unlock();
    mp_obj_t t[2] = { MP_OBJ_NEW_SMALL_INT(w / T5_DP(1)), MP_OBJ_NEW_SMALL_INT(h / T5_DP(1)) };
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
