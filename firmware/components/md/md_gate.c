/* SPDX-License-Identifier: Apache-2.0
 * The rendering gate (plan 4.8, design "Rendering budget"): an app bar, a
 * 100-row list, a card grid, a FAB and an open dialog, plus scripted load
 * (scroll, full-screen transitions, dialog open/close) so the device and
 * the simulator can measure the same thing.
 *
 * Landscape lays the list and the card grid side by side; portrait stacks
 * them. Everything follows the budget rules: square full-width surfaces,
 * flat list rows, baked shadows only on the FAB and the dialog.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "md/md.h"
#include "md/md_icons.h"
#include "md_priv.h"

#define GATE_ROWS 100
#define GATE_CARDS 6

typedef struct {
    lv_obj_t *scr, *alt, *body, *list, *grid, *fab, *dialog;
    lv_timer_t *timer;
    lv_anim_t scroll;
    md_gate_mode_t mode;
    bool on_alt;
} gate_t;

static void frame_done(lv_event_t *e);

static const char *const s_icons[] = {
    MD_ICON_WIFI, MD_ICON_BLUETOOTH, MD_ICON_BATTERY_FULL, MD_ICON_FOLDER, MD_ICON_SENSORS,
    MD_ICON_USB, MD_ICON_MEMORY, MD_ICON_ALARM,
};

static void bind_row(md_list_row_t *r, uint32_t i, void *ud)
{
    (void)ud;
    lv_label_set_text_fmt(r->primary, "List item %u", (unsigned)(i + 1));
    if (r->secondary) lv_label_set_text_fmt(r->secondary, "Secondary text for row %u", (unsigned)(i + 1));
    lv_label_set_text(r->icon, s_icons[i % (sizeof s_icons / sizeof s_icons[0])]);
}

static void child_gone(lv_event_t *e)
{
    gate_t *g = lv_event_get_user_data(e);
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    if (o == g->dialog) g->dialog = NULL;
    if (o == g->alt) g->alt = NULL;
}

static void dialog_cb(int button, void *ud)
{
    gate_t *g = ud;
    (void)button;
    g->dialog = NULL;
}

static void open_dialog(gate_t *g)
{
    if (g->dialog) return;
    g->dialog = md_dialog_show("Use location service?",
                               "Let the device determine location. This means sending anonymous "
                               "location data, even when no apps are running.",
                               "Agree", "Disagree", dialog_cb, g);
    if (g->dialog) lv_obj_add_event_cb(g->dialog, child_gone, LV_EVENT_DELETE, g);
}

static void fab_clicked(lv_event_t *e)
{
    open_dialog(lv_event_get_user_data(e));
}

static lv_obj_t *card(lv_obj_t *parent, int i)
{
    lv_obj_t *c = md_card_create(parent);
    char buf[32];
    snprintf(buf, sizeof buf, "OVERLINE %d", i + 1);
    lv_obj_t *o = md_label_create(c, buf, MD_TYPE_OVERLINE);
    lv_obj_set_style_text_opa(o, md_scheme()->on_surface.medium, 0);
    snprintf(buf, sizeof buf, "Card title %d", i + 1);
    md_label_create(c, buf, MD_TYPE_HEADLINE6);
    lv_obj_t *t = md_label_create(c, "Supporting text that wraps onto a second line.", MD_TYPE_BODY2);
    lv_obj_set_style_text_opa(t, md_scheme()->on_surface.medium, 0);
    lv_obj_set_width(t, LV_PCT(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *row = lv_obj_create(c);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    md_button_create(row, "Action", MD_BUTTON_TEXT);
    return c;
}

static void relayout(gate_t *g)
{
    lv_display_t *d = lv_obj_get_display(g->scr);
    bool land = lv_display_get_horizontal_resolution(d) > lv_display_get_vertical_resolution(d);
    lv_obj_set_flex_flow(g->body, land ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_size(g->list, land ? LV_PCT(45) : LV_PCT(100), land ? LV_PCT(100) : LV_PCT(55));
    lv_obj_set_size(g->grid, land ? LV_PCT(55) : LV_PCT(100), land ? LV_PCT(100) : LV_PCT(45));
}

static void scr_event(lv_event_t *e)
{
    gate_t *g = lv_event_get_user_data(e);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_SIZE_CHANGED:
        relayout(g);
        break;
    case LV_EVENT_DELETE:
        if (g->timer) lv_timer_delete(g->timer);
        lv_anim_delete(g, NULL);
        /* The dialog lives on the top layer and the alt screen on its own;
         * child_gone() clears the pointers if they go first. */
        if (g->alt) {
            lv_obj_remove_event_cb_with_user_data(g->alt, child_gone, g);
            lv_obj_delete(g->alt);
        }
        if (g->dialog) {
            lv_obj_remove_event_cb_with_user_data(g->dialog, child_gone, g);
            lv_obj_delete(g->dialog);
        }
        free(g);
        break;
    default:
        break;
    }
}

static lv_obj_t *build_alt(void)
{
    /* Second full screen for transitions: a dense card wall (heavier than
     * the list, so the transition test is the harder case). */
    lv_obj_t *s = lv_obj_create(NULL);
    md_appbar_create(s, "Transition", MD_ICON_ARROW_BACK, NULL, NULL);
    lv_obj_t *wall = lv_obj_create(s);
    lv_obj_remove_style_all(wall);
    lv_obj_set_size(wall, LV_PCT(100), LV_PCT(100));
    lv_obj_set_y(wall, T5_DP(MD_APP_BAR_DENSE_DP));
    lv_obj_set_style_pad_all(wall, T5_DP(16), 0);
    lv_obj_set_style_pad_gap(wall, T5_DP(8), 0);
    lv_obj_set_flex_flow(wall, LV_FLEX_FLOW_ROW_WRAP);
    for (int i = 0; i < 12; i++) {
        lv_obj_t *c = card(wall, i);
        lv_obj_set_width(c, LV_PCT(31));
    }
    return s;
}

lv_obj_t *md_gate_create(void)
{
    gate_t *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    static bool s_hooked;
    if (!s_hooked && lv_display_get_default()) {
        lv_display_add_event_cb(lv_display_get_default(), frame_done, LV_EVENT_RENDER_READY, NULL);
        s_hooked = true;
    }
    lv_obj_t *scr = lv_obj_create(NULL);
    g->scr = scr;
    lv_obj_set_user_data(scr, g);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(scr, false);

    lv_obj_t *bar = md_appbar_create(scr, "Rendering gate", MD_ICON_MENU, NULL, NULL);
    md_appbar_add_action(bar, MD_ICON_SEARCH, NULL, NULL);
    md_appbar_add_action(bar, MD_ICON_MORE_VERT, NULL, NULL);

    g->body = lv_obj_create(scr);
    lv_obj_remove_style_all(g->body);
    lv_obj_set_width(g->body, LV_PCT(100));
    lv_obj_set_flex_grow(g->body, 1);
    lv_obj_set_scrollable(g->body, false);

    g->list = md_list_create(g->body, GATE_ROWS, MD_LIST_ROW_2_DP, bind_row, NULL, g);

    g->grid = lv_obj_create(g->body);
    lv_obj_remove_style_all(g->grid);
    lv_obj_set_style_pad_all(g->grid, T5_DP(16), 0);
    lv_obj_set_style_pad_gap(g->grid, T5_DP(8), 0);
    lv_obj_set_flex_flow(g->grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_scroll_dir(g->grid, LV_DIR_VER);
    for (int i = 0; i < GATE_CARDS; i++) {
        lv_obj_t *c = card(g->grid, i);
        lv_obj_set_width(c, LV_PCT(47));
    }

    g->fab = md_fab_create(scr, MD_ICON_ADD, false);
    lv_obj_set_ignore_layout(g->fab, true);
    lv_obj_set_floating(g->fab, true);
    lv_obj_align(g->fab, LV_ALIGN_BOTTOM_RIGHT, -T5_DP(16), -T5_DP(16));
    lv_obj_add_event_cb(g->fab, fab_clicked, LV_EVENT_CLICKED, g);

    g->alt = build_alt();
    lv_obj_add_event_cb(g->alt, child_gone, LV_EVENT_DELETE, g);
    lv_obj_add_event_cb(scr, scr_event, LV_EVENT_ALL, g);
    relayout(g);
    return scr;
}

static void scroll_exec(void *var, int32_t v)
{
    gate_t *g = var;
    lv_obj_scroll_to_y(g->list, v, LV_ANIM_OFF);
}

/* Frame rate while animating: the interval between consecutive frames that
 * both had an animation running. Up to a step's first animated frame is
 * start latency (set-up, snapshots, a frame showing a dialog's scrim,
 * waiting for the framebuffers), reported separately, not frame rate. */
static md_gate_anim_stats_t s_as;
static uint32_t s_last_ms;
static bool s_skip, s_counting;

static bool s_prev_anim;
static uint32_t s_step_ms;

static void frame_done(lv_event_t *e)
{
    (void)e;
    uint32_t now = lv_tick_get();
    bool anim = lv_anim_count_running() > 0;
    if (s_skip && anim) {
        /* The step's first animated frame: everything before it (set-up,
         * a frame that shows the scrim, waiting for the buffers) is start
         * latency, not frame rate. */
        uint32_t lat = now - s_step_ms;
        if (lat > s_as.start_ms_max) s_as.start_ms_max = lat;
        s_skip = false;
    } else if (s_counting && !s_skip && anim && s_prev_anim) {
        s_as.frames++;
        s_as.ms += now - s_last_ms;
    }
    s_prev_anim = anim && !s_skip;
    s_last_ms = now;
}

void md_gate_anim_stats(md_gate_anim_stats_t *out, bool reset)
{
    if (out) *out = s_as;
    if (reset) memset(&s_as, 0, sizeof s_as);
    s_counting = true;
}

static void transition_tick(lv_timer_t *t)
{
    gate_t *g = lv_timer_get_user_data(t);
    s_skip = true;
    s_last_ms = s_step_ms = lv_tick_get();
    if (g->mode == MD_GATE_TRANSITION) {
        g->on_alt = !g->on_alt;
        lv_screen_load_anim(g->on_alt ? g->alt : g->scr, g->on_alt ? LV_SCREEN_LOAD_ANIM_MOVE_LEFT
                                                                  : LV_SCREEN_LOAD_ANIM_MOVE_RIGHT,
                            MD_DUR_SCREEN_MS, 0, false);
    } else if (g->mode == MD_GATE_DIALOG) {
        if (g->dialog) {
            lv_obj_t *d = g->dialog;
            g->dialog = NULL;
            lv_obj_remove_event_cb_with_user_data(d, child_gone, g);
            md_dialog_close(d);
        } else {
            open_dialog(g);
        }
    }
}

const char *md_gate_mode_name(md_gate_mode_t m)
{
    switch (m) {
    case MD_GATE_SCROLL: return "scroll";
    case MD_GATE_TRANSITION: return "transition";
    case MD_GATE_DIALOG: return "dialog";
    default: return "idle";
    }
}

void md_gate_run(lv_obj_t *scr, md_gate_mode_t mode)
{
    gate_t *g = lv_obj_get_user_data(scr);
    if (!g) return;
    lv_anim_delete(g, scroll_exec);
    if (g->timer) {
        lv_timer_delete(g->timer);
        g->timer = NULL;
    }
    if (g->on_alt) {
        lv_screen_load(g->scr);
        g->on_alt = false;
    }
    g->mode = mode;
    if (mode == MD_GATE_SCROLL) {
        lv_obj_update_layout(g->list);
        int32_t max = (int32_t)GATE_ROWS * T5_DP(MD_LIST_ROW_2_DP) - lv_obj_get_height(g->list);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, g);
        lv_anim_set_exec_cb(&a, scroll_exec);
        lv_anim_set_values(&a, 0, max > 0 ? max : 0);
        lv_anim_set_duration(&a, 4000);              /* ~1 600 px/s, a brisk fling */
        lv_anim_set_reverse_duration(&a, 4000);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        md_anim_set_curve(&a, MD_CURVE_STANDARD);
        lv_anim_start(&a);
    } else if (mode == MD_GATE_TRANSITION) {
        g->timer = lv_timer_create(transition_tick, MD_DUR_SCREEN_MS + 200, g);
    } else if (mode == MD_GATE_DIALOG) {
        g->timer = lv_timer_create(transition_tick, 400, g);
    }
}
