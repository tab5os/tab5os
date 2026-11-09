/* Calculator: a basic and a scientific calculator.
 *
 * The engine (calc.c) keeps what's typed as tokens and works out its value;
 * this file is the UI. The display shows the last calculation, what's
 * typed (large, or smaller when it's long) and the result live as you type.
 * "=" moves the result to the main line, and an operator after that carries
 * on from it. The Scientific keys sit left of the basic ones in landscape
 * and above them in portrait. The mode, the angle unit, Ans and what's
 * typed are saved when the app is suspended and loaded at the next start.
 *
 * The OS fonts have no minus sign, root, pi or superscripts, so the app
 * brings two Roboto fonts with them (gen_fonts.py makes calc_font_*.c).
 */
#include <string.h>
#include <t5/app.h>
#include <t5/ui.h>
#include "calc.h"

LV_FONT_DECLARE(calc_font_40)
LV_FONT_DECLARE(calc_font_80)

#define S_MINUS "\xE2\x88\x92"     /* − */
#define S_TIMES "\xC3\x97"         /* × */
#define S_DIVIDE "\xC3\xB7"        /* ÷ */
#define S_PLUSMINUS "\xC2\xB1"     /* ± */
#define S_ROOT "\xE2\x88\x9A"      /* √ */
#define S_PI "\xCF\x80"            /* π */
#define S_SUP_MINUS_1 "\xE2\x81\xBB\xC2\xB9" /* ⁻¹ */
#define S_SUP_2 "\xC2\xB2"         /* ² */
#define S_SUP_X "\xCB\xA3"         /* ˣ */
#define S_SUP_Y "\xCA\xB8"         /* ʸ */

#define STATE_MAGIC 0x43414c32u    /* "CAL2" */

typedef struct {
    uint32_t magic;
    bool sci;
    calc_t calc;
} saved_t;

static struct {
    saved_t st;
    lv_obj_t *keys, *sci_grid, *basic_grid, *angle_label;
    lv_obj_t *hist, *main_box, *main, *preview;
    lv_obj_t *chip_basic, *chip_sci;
} s;

/* ---- display --------------------------------------------------------------- */
/* The main line: the large font if it fits, else the small one, scrolled
 * to the end so the newest input shows. */
static void set_main(const char *text)
{
    lv_label_set_text(s.main, text);
    lv_obj_update_layout(s.main_box);
    int32_t room = lv_obj_get_content_width(s.main_box);
    lv_point_t sz;
    lv_text_get_size(&sz, text, &calc_font_80, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const lv_font_t *f = sz.x <= room ? &calc_font_80 : &calc_font_40;
    lv_obj_set_style_text_font(s.main, f, 0);
    lv_text_get_size(&sz, text, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    bool fits = sz.x <= room;
    lv_obj_set_flex_align(s.main_box, fits ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    lv_obj_update_layout(s.main_box);
    lv_obj_scroll_to_x(s.main_box, fits ? 0 : LV_COORD_MAX, LV_ANIM_OFF);
}

static void refresh(void)
{
    char text[400];
    calc_text(&s.st.calc, text, sizeof text);
    set_main(text[0] ? text : "0");
    lv_label_set_text(s.hist, s.st.calc.hist);
    calc_preview(&s.st.calc, text, sizeof text);
    lv_label_set_text(s.preview, text);
    lv_obj_set_style_text_color(s.preview, md_color(md_scheme()->c.on_surface), 0);
    lv_label_set_text(s.angle_label, s.st.calc.deg ? "DEG" : "RAD");
}

/* ---- keys ------------------------------------------------------------------ */
typedef enum { ST_NUM, ST_FN, ST_OP, ST_EQ } key_style_t;

typedef struct {
    const char *label;        /* NULL: the backspace icon */
    uint8_t key, v, style;
} key_def_t;

#define KD(d) { #d, CALC_K_DIGIT, d, ST_NUM }
static const key_def_t s_basic[5][4] = {
    { { "AC", CALC_K_CLEAR, 0, ST_OP }, { NULL, CALC_K_BACK, 0, ST_OP }, { "%", CALC_K_POST, CALC_PCT, ST_FN },
      { S_DIVIDE, CALC_K_OP, CALC_DIV, ST_OP } },
    { KD(7), KD(8), KD(9), { S_TIMES, CALC_K_OP, CALC_MUL, ST_OP } },
    { KD(4), KD(5), KD(6), { S_MINUS, CALC_K_OP, CALC_SUB, ST_OP } },
    { KD(1), KD(2), KD(3), { "+", CALC_K_OP, CALC_ADD, ST_OP } },
    { { S_PLUSMINUS, CALC_K_SIGN, 0, ST_NUM }, KD(0), { ".", CALC_K_POINT, 0, ST_NUM },
      { "=", CALC_K_EQUALS, 0, ST_EQ } },
};
static const key_def_t s_sci[5][4] = {
    { { "DEG", CALC_K_ANGLE, 0, ST_FN }, { "(", CALC_K_LP, 0, ST_FN }, { ")", CALC_K_RP, 0, ST_FN },
      { "x" S_SUP_Y, CALC_K_OP, CALC_POW, ST_FN } },
    { { "sin", CALC_K_FUNC, CALC_SIN, ST_FN }, { "cos", CALC_K_FUNC, CALC_COS, ST_FN },
      { "tan", CALC_K_FUNC, CALC_TAN, ST_FN }, { S_ROOT, CALC_K_FUNC, CALC_SQRT, ST_FN } },
    { { "sin" S_SUP_MINUS_1, CALC_K_FUNC, CALC_ASIN, ST_FN }, { "cos" S_SUP_MINUS_1, CALC_K_FUNC, CALC_ACOS, ST_FN },
      { "tan" S_SUP_MINUS_1, CALC_K_FUNC, CALC_ATAN, ST_FN }, { "x" S_SUP_2, CALC_K_POST, CALC_SQ, ST_FN } },
    { { "ln", CALC_K_FUNC, CALC_LN, ST_FN }, { "log", CALC_K_FUNC, CALC_LOG, ST_FN },
      { "e" S_SUP_X, CALC_K_EXP, 0, ST_FN }, { "x!", CALC_K_POST, CALC_FACT, ST_FN } },
    { { S_PI, CALC_K_CONST, CALC_PI, ST_FN }, { "e", CALC_K_CONST, CALC_E, ST_FN },
      { "Ans", CALC_K_CONST, CALC_ANS, ST_FN }, { "1/x", CALC_K_POST, CALC_INV, ST_FN } },
};

static void key_clicked(lv_event_t *e)
{
    const key_def_t *k = lv_event_get_user_data(e);
    if (!calc_key(&s.st.calc, (calc_key_t)k->key, k->v)) {
        lv_label_set_text(s.preview, "Error");             /* "=" on something with no value */
        lv_obj_set_style_text_color(s.preview, md_color(md_scheme()->c.error), 0);
        return;
    }
    refresh();
}

static lv_obj_t *box(lv_obj_t *parent, lv_flex_flow_t flow)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_flex_flow(o, flow);
    lv_obj_set_scrollable(o, false);
    return o;
}

static lv_obj_t *key_create(lv_obj_t *row, const key_def_t *k)
{
    const md_scheme_t *sc = md_scheme();
    md_rgb_t base = sc->c.background, bg, ink;
    switch ((key_style_t)k->style) {
    case ST_NUM: bg = md_mix(sc->c.on_surface, base, 0x1f); ink = sc->c.on_surface; break;
    case ST_FN: bg = md_mix(sc->c.on_surface, base, 0x0f); ink = sc->c.on_surface; break;
    case ST_OP: bg = md_mix(sc->c.primary, base, 0x38); ink = sc->c.primary; break;
    default: bg = sc->c.primary; ink = sc->c.on_primary; break;
    }
    lv_obj_t *b = lv_obj_create(row);
    lv_obj_remove_style_all(b);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_height(b, LV_PCT(100));
    lv_obj_set_style_radius(b, T5_DP(12), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, md_color(bg), 0);
    lv_obj_set_style_bg_color(b, md_color(md_mix(ink, bg, sc->states.pressed + 0x18)), LV_STATE_PRESSED);
    lv_obj_set_scrollable(b, false);
    lv_obj_add_event_cb(b, key_clicked, LV_EVENT_CLICKED, (void *)k);
    lv_obj_t *l;
    if (k->label) {
        l = lv_label_create(b);
        lv_label_set_text(l, k->label);
        lv_obj_set_style_text_font(l, &calc_font_40, 0);
        if (k->key == CALC_K_ANGLE) s.angle_label = l;
    } else {
        l = md_icon_create(b, MD_ICON_BACKSPACE, 24);
    }
    lv_obj_set_style_text_color(l, md_color(ink), 0);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *grid_create(lv_obj_t *parent, const key_def_t keys[5][4])
{
    lv_obj_t *g = box(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g, T5_DP(6), 0);
    for (int r = 0; r < 5; r++) {
        lv_obj_t *row = box(g, LV_FLEX_FLOW_ROW);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_flex_grow(row, 1);
        lv_obj_set_style_pad_column(row, T5_DP(6), 0);
        for (int c = 0; c < 4; c++) key_create(row, &keys[r][c]);
    }
    return g;
}

/* Landscape: the scientific keys left of the basic ones. Portrait: above. */
static void layout(void)
{
    bool wide = lv_display_get_horizontal_resolution(NULL) > lv_display_get_vertical_resolution(NULL);
    lv_obj_set_flex_flow(s.keys, wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_hidden(s.sci_grid, !s.st.sci);
    lv_obj_t *grids[2] = { s.sci_grid, s.basic_grid };
    for (int i = 0; i < 2; i++) {
        lv_obj_set_size(grids[i], wide ? LV_SIZE_CONTENT : LV_PCT(100), wide ? LV_PCT(100) : LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(grids[i], 1);
    }
    refresh();
}

static void mode_changed(lv_event_t *e)
{
    lv_obj_t *chip = lv_event_get_target_obj(e);
    if (!md_chip_selected(chip)) return;
    s.st.sci = chip == s.chip_sci;
    layout();
}

static lv_obj_t *display_label(lv_obj_t *parent, const lv_font_t *font, uint8_t opa)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, md_color(md_scheme()->c.on_surface), 0);
    lv_obj_set_style_text_opa(l, opa, 0);
    lv_label_set_text(l, "");
    return l;
}

/* ---- lifecycle ------------------------------------------------------------- */
static void on_rotate(t5_app_t *app, int degrees)
{
    (void)app; (void)degrees;
    layout();
}

static void on_suspend(t5_app_t *app)
{
    T5_IGNORE(t5_app_state_save(app, &s.st, sizeof s.st));   /* losing it only resets the calculator */
}

static void on_create(t5_app_t *app)
{
    size_t len = 0;
    if (t5_app_state_load(app, &s.st, sizeof s.st, &len) != T5_OK || len != sizeof s.st ||
        s.st.magic != STATE_MAGIC || s.st.calc.ntok > CALC_MAX_TOK) {
        s.st.magic = STATE_MAGIC;
        s.st.sci = false;
        calc_reset(&s.st.calc);
    }
    s.st.calc.hist[sizeof s.st.calc.hist - 1] = 0;
    const md_scheme_t *sc = md_scheme();

    /* The screen's own padding keeps clear of the shell's bars: pad inside it. */
    lv_obj_t *root = box(t5_app_screen(app), LV_FLEX_FLOW_COLUMN);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(root, T5_DP(8), 0);
    lv_obj_set_style_pad_row(root, T5_DP(6), 0);

    /* The display: mode chips and the last calculation, what's typed, the live result. */
    lv_obj_t *top = box(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, T5_DP(8), 0);
    s.chip_basic = md_chip_create(top, "Basic", NULL, MD_CHIP_CHOICE);
    s.chip_sci = md_chip_create(top, "Scientific", NULL, MD_CHIP_CHOICE);
    md_chip_set_selected(s.st.sci ? s.chip_sci : s.chip_basic, true);
    lv_obj_add_event_cb(s.chip_basic, mode_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s.chip_sci, mode_changed, LV_EVENT_VALUE_CHANGED, NULL);
    s.hist = display_label(top, &calc_font_40, sc->on_surface.medium);
    lv_obj_set_flex_grow(s.hist, 1);
    lv_obj_set_style_text_align(s.hist, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s.hist, LV_LABEL_LONG_MODE_DOTS);

    s.main_box = box(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(s.main_box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_scrollable(s.main_box, true);
    lv_obj_set_scroll_dir(s.main_box, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s.main_box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_min_height(s.main_box, lv_font_get_line_height(&calc_font_80), 0);
    s.main = display_label(s.main_box, &calc_font_80, sc->on_surface.high);

    s.preview = display_label(root, &calc_font_40, sc->on_surface.medium);
    lv_obj_set_width(s.preview, LV_PCT(100));
    lv_obj_set_style_text_align(s.preview, LV_TEXT_ALIGN_RIGHT, 0);

    /* The keys fill the rest. */
    s.keys = box(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(s.keys, LV_PCT(100));
    lv_obj_set_flex_grow(s.keys, 1);
    lv_obj_set_style_pad_gap(s.keys, T5_DP(6), 0);
    s.sci_grid = grid_create(s.keys, s_sci);
    s.basic_grid = grid_create(s.keys, s_basic);
    layout();
}

T5_APP(.on_create = on_create, .on_suspend = on_suspend, .on_rotate = on_rotate);
