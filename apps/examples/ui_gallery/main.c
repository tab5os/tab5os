/* UI Gallery: the two ways to build a Tab5 OS UI, side by side.
 *
 *   Material   the MD2 components (md_*): buttons, switch, slider,
 *              checkbox, chips, a text field, a card, a snackbar.
 *   Custom     raw LVGL objects styled by hand (an arc, a chart), and a
 *              widget class of our own: a gauge drawn in its draw event.
 *   Canvas     free drawing into a pixel buffer (lv_canvas): a sine wave
 *              redrawn by a timer.
 *   Shell      the bars around the app: full screen, the insets they
 *              take, and an ongoing activity in the status bar.
 *
 * The custom widget follows the ABI rule for widgets (docs/abi-stability.md):
 * its class leaves instance_size at 0, so objects are lv_obj_t-sized, and
 * its own data lives in a block hung on the object's user data. lv_obj_t's
 * layout isn't part of the ABI; lv_obj_class_t's is.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <t5/app.h>
#include <t5/shell.h>
#include <t5/ui.h>

/* ---- a custom widget: the gauge ---------------------------------------------- */
typedef struct {
    int32_t value;            /* 0..100 */
    char text[8];
} gauge_t;

static lv_obj_class_t s_gauge_class;     /* filled in by gauge_class_init */

static void gauge_ctor(const lv_obj_class_t *cls, lv_obj_t *obj)
{
    (void)cls;
    lv_obj_set_user_data(obj, calloc(1, sizeof(gauge_t)));
    lv_obj_set_clickable(obj, false);
}

static void gauge_dtor(const lv_obj_class_t *cls, lv_obj_t *obj)
{
    (void)cls;
    free(lv_obj_get_user_data(obj));
}

static void gauge_draw(lv_obj_t *obj, lv_layer_t *layer, const gauge_t *g)
{
    lv_area_t a;
    lv_obj_get_content_coords(obj, &a);
    int32_t w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    int32_t r = (w < h ? w : h) / 2;
    const md_scheme_t *sc = md_scheme();
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.center.x = a.x1 + w / 2;
    arc.center.y = a.y1 + h / 2;
    arc.radius = (uint16_t)r;
    arc.width = T5_DP(10);
    arc.rounded = 1;
    arc.start_angle = 135;
    arc.end_angle = 45;                                   /* the track: 270 degrees */
    arc.color = md_color(sc->c.on_surface);
    arc.opa = LV_OPA_20;
    lv_draw_arc(layer, &arc);
    arc.end_angle = 135 + g->value * 270 / 100;           /* the value */
    arc.color = md_color(sc->c.primary);
    arc.opa = LV_OPA_COVER;
    if (g->value > 0) lv_draw_arc(layer, &arc);
    lv_draw_label_dsc_t lbl;
    lv_draw_label_dsc_init(&lbl);
    lbl.text = g->text;
    lbl.text_local = 1;                                   /* copied: the buffer may change */
    lbl.font = lv_obj_get_style_text_font(obj, 0);
    lbl.color = md_color(sc->c.on_surface);
    lbl.align = LV_TEXT_ALIGN_CENTER;
    int32_t lh = lv_font_get_line_height(lbl.font);
    lv_area_t t = { a.x1, a.y1 + h / 2 - lh / 2, a.x2, a.y1 + h / 2 + lh / 2 };
    lv_draw_label(layer, &lbl, &t);
}

static void gauge_event(const lv_obj_class_t *cls, lv_event_t *e)
{
    (void)cls;
    /* The base class handles everything else (style, size, scrolling...). */
    if (lv_obj_event_base(&s_gauge_class, e) != LV_RESULT_OK) return;
    if (lv_event_get_code(e) == LV_EVENT_DRAW_MAIN) {
        lv_obj_t *obj = lv_event_get_current_target_obj(e);
        const gauge_t *g = lv_obj_get_user_data(obj);
        if (g) gauge_draw(obj, lv_event_get_layer(e), g);
    }
}

static void gauge_class_init(void)
{
    s_gauge_class.base_class = &lv_obj_class;
    s_gauge_class.constructor_cb = gauge_ctor;
    s_gauge_class.destructor_cb = gauge_dtor;
    s_gauge_class.event_cb = gauge_event;
    s_gauge_class.width_def = T5_DP(160);
    s_gauge_class.height_def = T5_DP(160);
    s_gauge_class.name = "gauge";
    s_gauge_class.instance_size = 0;          /* lv_obj_t's own size: see the top of the file */
}

static lv_obj_t *gauge_create(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_class_create_obj(&s_gauge_class, parent);
    lv_obj_class_init_obj(obj);
    return obj;
}

static void gauge_set(lv_obj_t *obj, int32_t v)
{
    gauge_t *g = lv_obj_get_user_data(obj);
    if (!g) return;
    g->value = v < 0 ? 0 : v > 100 ? 100 : v;
    snprintf(g->text, sizeof g->text, "%d%%", (int)g->value);
    lv_obj_invalidate(obj);
}

/* ---- state ---------------------------------------------------------------- */
static struct {
    t5_app_t *app;
    lv_obj_t *gauge, *arc, *chart, *insets, *canvas;
    lv_chart_series_t *series;
    lv_draw_buf_t *buf;
    int32_t phase;
    t5_shell_activity_t *activity;
    int act_pct;
    lv_timer_t *act_timer;
} s;

/* ---- Material tab ----------------------------------------------------------- */
static void snack_clicked(lv_event_t *e)
{
    (void)e;
    md_snackbar_show("Saved to the gallery", "Undo", NULL, NULL);
}

static void build_material(lv_obj_t *page)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, T5_DP(16), 0);
    lv_obj_set_style_pad_row(page, T5_DP(12), 0);
    md_section_create(page, "Buttons");
    lv_obj_t *row = lv_obj_create(page);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(row, T5_DP(12), 0);
    lv_obj_add_event_cb(md_button_create(row, "Contained", MD_BUTTON_CONTAINED), snack_clicked, LV_EVENT_CLICKED, NULL);
    md_button_create(row, "Outlined", MD_BUTTON_OUTLINED);
    md_button_create(row, "Text", MD_BUTTON_TEXT);
    md_section_create(page, "Controls");
    md_switch_create(page, true);
    md_slider_create(page, 0, 100, 40);
    md_checkbox_create(page, "Keep the screen on", false);
    lv_obj_t *chips = lv_obj_create(page);
    lv_obj_remove_style_all(chips);
    lv_obj_set_size(chips, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(chips, T5_DP(8), 0);
    md_chip_create(chips, "Hourly", NULL, MD_CHIP_CHOICE);
    md_chip_create(chips, "Daily", NULL, MD_CHIP_CHOICE);
    md_chip_create(chips, "Alerts", MD_ICON_NOTIFICATIONS, MD_CHIP_FILTER);
    md_textfield_create(page, "Name", false);
    lv_obj_t *card = md_card_create(page);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, T5_DP(16), 0);
    lv_obj_t *txt = md_label_create(card, "A card holds related content at 1 dp elevation.", MD_TYPE_BODY2);
    lv_obj_set_width(txt, LV_PCT(100));
    lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_WRAP);
}

/* ---- Custom tab --------------------------------------------------------------- */
static void arc_changed(lv_event_t *e)
{
    int32_t v = lv_arc_get_value(lv_event_get_target_obj(e));
    gauge_set(s.gauge, v);
    lv_chart_set_next_value(s.chart, s.series, v);
}

static void build_custom(lv_obj_t *page)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(page, T5_DP(16), 0);
    const md_scheme_t *sc = md_scheme();
    /* A raw LVGL arc, styled by hand. Drag it. */
    s.arc = lv_arc_create(page);
    lv_obj_set_size(s.arc, T5_DP(160), T5_DP(160));
    lv_arc_set_value(s.arc, 40);
    lv_obj_set_style_arc_color(s.arc, md_color(md_accent(MD_ACCENT_AMBER)->dark), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s.arc, T5_DP(12), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s.arc, T5_DP(12), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s.arc, md_color(sc->c.on_surface), LV_PART_KNOB);
    lv_obj_add_event_cb(s.arc, arc_changed, LV_EVENT_VALUE_CHANGED, NULL);
    /* Our own widget class, following it. */
    s.gauge = gauge_create(page);
    gauge_set(s.gauge, 40);
    /* A raw LVGL chart of the values. */
    s.chart = lv_chart_create(page);
    lv_obj_set_size(s.chart, T5_DP(240), T5_DP(140));
    lv_chart_set_point_count(s.chart, 20);
    lv_chart_set_axis_range(s.chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    s.series = lv_chart_add_series(s.chart, md_color(sc->c.primary), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(s.chart, s.series, 40);
}

/* ---- Canvas tab ---------------------------------------------------------------- */
#define CANVAS_W_DP 300          /* fits portrait (360 dp wide) as well as landscape */
#define CANVAS_H_DP 160

static void canvas_draw(lv_timer_t *t)
{
    (void)t;
    if (!s.canvas || lv_obj_is_hidden(lv_obj_get_parent(s.canvas))) return;
    const md_scheme_t *sc = md_scheme();
    int32_t w = T5_DP(CANVAS_W_DP), h = T5_DP(CANVAS_H_DP);
    lv_canvas_fill_bg(s.canvas, md_color(sc->c.surface), LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(s.canvas, &layer);
    lv_draw_line_dsc_t ln;
    lv_draw_line_dsc_init(&ln);
    ln.color = md_color(sc->c.primary);
    ln.width = T5_DP(3);
    ln.round_end = ln.round_start = 1;
    s.phase += 8;
    for (int32_t x = 0; x + 8 <= w; x += 8) {
        float a0 = (float)(x + s.phase) * 0.012f, a1 = (float)(x + 8 + s.phase) * 0.012f;
        ln.p1.x = x;
        ln.p1.y = h / 2 + (int32_t)(sinf(a0) * (float)h * 0.4f);
        ln.p2.x = x + 8;
        ln.p2.y = h / 2 + (int32_t)(sinf(a1) * (float)h * 0.4f);
        lv_draw_line(&layer, &ln);
    }
    lv_canvas_finish_layer(s.canvas, &layer);
}

static void build_canvas(lv_obj_t *page)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(page, T5_DP(16), 0);
    lv_obj_t *cap = md_label_create(page, "Drawn into a pixel buffer with lv_draw_line, 30 times a second",
                                    MD_TYPE_CAPTION);
    lv_obj_set_width(cap, LV_PCT(100));
    lv_label_set_long_mode(cap, LV_LABEL_LONG_MODE_WRAP);
    s.buf = lv_draw_buf_create(T5_DP(CANVAS_W_DP), T5_DP(CANVAS_H_DP), LV_COLOR_FORMAT_RGB565, 0);
    if (!s.buf) return;                       /* the app's memory: freed by the OS at exit */
    s.canvas = lv_canvas_create(page);
    lv_canvas_set_draw_buf(s.canvas, s.buf);
    canvas_draw(NULL);
    lv_timer_create(canvas_draw, 33, NULL);
}

/* ---- Shell tab ----------------------------------------------------------------- */
static void show_insets(void)
{
    t5_shell_insets_t in;
    char buf[96];
    if (t5_shell_get_insets(&in) == T5_OK) {
        snprintf(buf, sizeof buf, "The shell's bars take %d dp at the top and %d dp at the bottom%s.", (int)in.top,
                 (int)in.bottom, t5_shell_fullscreen() ? " (full screen)" : "");
        lv_label_set_text(s.insets, buf);
    }
}

static void insets_ui(void *arg)
{
    (void)arg;
    if (s.insets) show_insets();
}

static void insets_event(t5_event_id_t id, const void *data, size_t len, void *arg)
{
    (void)id; (void)data; (void)len; (void)arg;
    T5_IGNORE(t5_ui_async(insets_ui, NULL));      /* dropped only when the app is going */
}

static void fullscreen_clicked(lv_event_t *e)
{
    (void)e;
    T5_IGNORE(t5_shell_set_fullscreen(!t5_shell_fullscreen()));   /* on the UI task: can't fail */
}

static void act_tick(lv_timer_t *t)
{
    s.act_pct += 10;
    if (s.act_pct > 100) {
        T5_IGNORE(t5_shell_activity_end(s.activity));
        s.activity = NULL;
        lv_timer_delete(t);
        s.act_timer = NULL;
        return;
    }
    char text[40];
    snprintf(text, sizeof text, "Rendering: %d%%", s.act_pct);
    T5_IGNORE(t5_shell_activity_update(s.activity, text, s.act_pct));
}

static void activity_clicked(lv_event_t *e)
{
    (void)e;
    if (s.activity) return;
    s.act_pct = 0;
    if (t5_shell_activity_start(MD_ICON_PALETTE, "Rendering", &s.activity) == T5_OK) {
        s.act_timer = lv_timer_create(act_tick, 1000, NULL);
    }
}

static void build_shell(lv_obj_t *page)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, T5_DP(16), 0);
    lv_obj_set_style_pad_row(page, T5_DP(12), 0);
    s.insets = md_label_create(page, "", MD_TYPE_BODY1);
    lv_obj_set_width(s.insets, LV_PCT(100));
    lv_label_set_long_mode(s.insets, LV_LABEL_LONG_MODE_WRAP);
    show_insets();
    lv_obj_add_event_cb(md_button_create(page, "Full screen on or off", MD_BUTTON_OUTLINED), fullscreen_clicked,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(md_button_create(page, "Post a 10 s activity", MD_BUTTON_OUTLINED), activity_clicked,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_t *hint = md_label_create(page, "In full screen, touch the bottom edge to see the handle; swipe up from "
                                           "it to leave.", MD_TYPE_CAPTION);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
}

static void on_create(t5_app_t *app)
{
    s.app = app;
    gauge_class_init();
    lv_obj_t *tabs = md_tabs_create(t5_app_screen(app));
    lv_obj_set_size(tabs, LV_PCT(100), LV_PCT(100));
    build_material(md_tabs_add(tabs, "Material"));
    build_custom(md_tabs_add(tabs, "Custom"));
    build_canvas(md_tabs_add(tabs, "Canvas"));
    build_shell(md_tabs_add(tabs, "Shell"));
    T5_IGNORE(t5_event_subscribe(T5_EV_UI_INSETS, insets_event, NULL));   /* only the label misses updates */
}

T5_APP(.on_create = on_create);
