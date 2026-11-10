/* SPDX-License-Identifier: Apache-2.0
 * Power (plan 6.8): live voltage, current and watts from the INA226, the
 * estimate of time left, a 24-hour charge history chart, the average draw
 * of each app while it was in front (measured on battery only), and the
 * charge limit.
 */
#include <stdlib.h>
#include <string.h>
#include "t5/power.h"
#include "t5/system.h"
#include "sysapps_priv.h"

static sa_nav_t s_nav;

typedef struct {
    lv_obj_t *pct, *state, *cycles, *volts, *amps, *watts;
    lv_obj_t *chart;
    lv_chart_series_t *ser;
    lv_obj_t *chart_note, *apps, *limit, *clear_row;
    uint32_t last_hist_count;
    int apps_shown;
    t5_bat_hist_pt_t hist[T5_BAT_HIST_LEN];
    t5_app_entry_t ents[T5_APPS_MAX];   /* for app names in the draw list */
    int n_ents;
} power_t;

/* A live reading: the value is the hero, the label sits under it. Three
 * share a row in either orientation (flex-grow), so nothing wraps. */
static lv_obj_t *tile(lv_obj_t *parent, const char *caption, lv_obj_t **value)
{
    lv_obj_t *c = md_card_create(parent);
    lv_obj_set_size(c, 0, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(c, 1);
    *value = md_label_create(c, "-", MD_TYPE_HEADLINE5);
    lv_obj_t *l = md_label_create(c, caption, MD_TYPE_BODY2);
    lv_obj_set_style_text_opa(l, md_scheme()->on_surface.medium, 0);
    return c;
}

/* "3 h", "3 h 20 min", "45 min": no "0 min" tails. */
static void fmt_span(char *out, size_t n, uint32_t s)
{
    uint32_t h = s / 3600, m = s % 3600 / 60;
    if (h && m) snprintf(out, n, "%u h %u min", (unsigned)h, (unsigned)m);
    else if (h) snprintf(out, n, "%u h", (unsigned)h);
    else snprintf(out, n, "%u min", (unsigned)(m ? m : 1));
}

static void limit_changed(lv_event_t *e)
{
    t5_battery_set_charge_limit(lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
}

static void reset_clicked(lv_event_t *e)
{
    (void)e;
    t5_apps_energy_reset();
    md_snackbar_show("App figures cleared", NULL, NULL, NULL);
}

static const char *pretty_id(const power_t *p, const char *id)
{
    if (!strcmp(id, T5_ENERGY_LAUNCHER)) return "Home screen";
    if (!strcmp(id, T5_ENERGY_SCREEN_OFF)) return "Screen off and asleep";
    for (int i = 0; i < p->n_ents; i++)
        if (!strcmp(p->ents[i].id, id)) return p->ents[i].name[0] ? p->ents[i].name : id;
    return id;
}

static void refresh(sa_nav_t *nav, void *arg)
{
    (void)nav;
    power_t *p = arg;
    t5_battery_status_t b;
    t5_battery_status(&b);
    char buf[96];
    if (b.percent >= 0) snprintf(buf, sizeof buf, "%d%%", b.percent);
    else snprintf(buf, sizeof buf, "--");
    sa_set_text(p->pct, buf);
    /* One plain sentence: what the battery is doing, and for how long. */
    static const char *chg[] = { "Plugged in, not charging", "Charging", "Fully charged",
                                 "Holding at 80% to spare the battery", "Charging paused while the battery is warm" };
    char left[32];
    if (b.usb) {
        snprintf(buf, sizeof buf, "%s", (unsigned)b.charge < sizeof chg / sizeof chg[0] ? chg[b.charge] : "Plugged in");
    } else if (b.minutes_left > 0) {
        fmt_span(left, sizeof left, (uint32_t)b.minutes_left * 60);
        snprintf(buf, sizeof buf, "On battery, about %s left", left);
    } else {
        snprintf(buf, sizeof buf, "On battery, estimating time left");
    }
    sa_set_text(p->state, buf);
    snprintf(buf, sizeof buf, "%u charge cycle%s so far", b.cycles, b.cycles == 1 ? "" : "s");
    sa_set_text(p->cycles, buf);
    snprintf(buf, sizeof buf, "%u.%02u V", b.mv / 1000, (b.mv % 1000) / 10);
    sa_set_text(p->volts, buf);
    snprintf(buf, sizeof buf, "%d mA", (int)(b.ma10 / 10));
    sa_set_text(p->amps, buf);
    int32_t mw_abs = b.mw < 0 ? -b.mw : b.mw;   /* keep the sign of -0.50 W */
    snprintf(buf, sizeof buf, "%s%d.%02d W", b.mw < 0 ? "-" : "", (int)(mw_abs / 1000), (int)(mw_abs % 1000 / 10));
    sa_set_text(p->watts, buf);
    if (!lv_obj_has_state(p->limit, LV_STATE_PRESSED)) {
        if (t5_battery_charge_limit()) lv_obj_add_state(p->limit, LV_STATE_CHECKED);
        else lv_obj_remove_state(p->limit, LV_STATE_CHECKED);
    }

    int n = t5_battery_history(p->hist, T5_BAT_HIST_LEN);
    if ((uint32_t)n != p->last_hist_count || !n) {
        p->last_hist_count = (uint32_t)n;
        lv_chart_set_all_values(p->chart, p->ser, LV_CHART_POINT_NONE);
        int32_t *y = lv_chart_get_series_y_array(p->chart, p->ser);
        int off = T5_BAT_HIST_LEN - n;
        for (int i = 0; i < n; i++) y[off + i] = p->hist[i].pct >= 0 ? p->hist[i].pct : LV_CHART_POINT_NONE;
        lv_chart_refresh(p->chart);
        /* A line needs two points; until then the chart would be an empty
         * grid, so say when it fills in instead. */
        if (n >= 2) {
            lv_obj_set_hidden(p->chart, false);
            char a[24];
            fmt_span(a, sizeof a, (uint32_t)n * T5_BAT_HIST_PERIOD_S);
            snprintf(buf, sizeof buf, "Last %s, newest on the right. The lines mark 25%% steps.", a);
        } else {
            lv_obj_set_hidden(p->chart, true);
            snprintf(buf, sizeof buf, "The chart starts 10 minutes after the Tab5 turns on and keeps the last "
                                      "24 hours.");
        }
        sa_set_text(p->chart_note, buf);
    }

    t5_energy_rec_t rec[T5_ENERGY_MAX];
    int k = t5_apps_energy(rec, T5_ENERGY_MAX);
    int h = k;
    for (int i = 0; i < k; i++) h = h * 31 + t5_energy_avg_mw(&rec[i]) / 10;
    if (h == p->apps_shown) return;
    p->apps_shown = h;
    lv_obj_clean(p->apps);
    p->n_ents = t5_apps_list(p->ents, T5_APPS_MAX);
    int shown = 0;
    char a[24], sub[64];
    for (int i = 0; i < k; i++) {
        int32_t mw = t5_energy_avg_mw(&rec[i]);
        if (mw < 0) continue;
        sa_fmt_duration(a, sizeof a, rec[i].ms / 1000);
        snprintf(sub, sizeof sub, "%d mW on average, over %s", (int)mw, a);
        md_row_create(p->apps, MD_ICON_BOLT, pretty_id(p, rec[i].id), sub);
        shown++;
    }
    if (!shown)
        sa_note(p->apps, "Nothing yet. Use the Tab5 unplugged for a while: draw can only be measured on battery.");
    lv_obj_set_hidden(p->clear_row, !shown);
}

static void on_create(t5_app_t *app)
{
    sa_nav_init(&s_nav, app);
    lv_obj_t *body = sa_push(&s_nav, "Power");
    power_t *p = calloc(1, sizeof *p);
    if (!p) return;
    p->apps_shown = -1;
    lv_obj_t *hdr = lv_obj_create(body);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(hdr, T5_DP(16), 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_COLUMN);
    p->pct = md_label_create(hdr, "", MD_TYPE_HEADLINE4);
    p->state = md_label_create(hdr, "", MD_TYPE_SUBTITLE1);
    p->cycles = md_label_create(hdr, "", MD_TYPE_BODY2);
    lv_obj_set_style_text_opa(p->cycles, md_scheme()->on_surface.medium, 0);

    lv_obj_t *tiles = lv_obj_create(body);
    lv_obj_remove_style_all(tiles);
    lv_obj_set_size(tiles, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(tiles, T5_DP(16), 0);
    lv_obj_set_style_pad_gap(tiles, T5_DP(8), 0);
    lv_obj_set_flex_flow(tiles, LV_FLEX_FLOW_ROW);
    tile(tiles, "Voltage", &p->volts);
    tile(tiles, "Current", &p->amps);
    tile(tiles, "Power", &p->watts);

    md_section_create(body, "Battery level");
    p->chart = lv_chart_create(body);
    lv_obj_set_size(p->chart, LV_PCT(100), T5_DP(120));
    lv_obj_set_style_pad_hor(p->chart, T5_DP(16), 0);
    lv_obj_set_style_pad_ver(p->chart, T5_DP(8), 0);
    lv_obj_set_style_bg_opa(p->chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p->chart, 0, 0);
    lv_obj_set_style_line_color(p->chart, md_color(md_scheme()->c.on_surface), LV_PART_MAIN);
    lv_obj_set_style_line_opa(p->chart, md_scheme()->on_surface.divider, LV_PART_MAIN);
    lv_obj_set_style_size(p->chart, 0, 0, LV_PART_INDICATOR);       /* no point markers */
    lv_obj_set_style_line_width(p->chart, T5_DP(2), LV_PART_ITEMS);
    lv_chart_set_type(p->chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(p->chart, T5_BAT_HIST_LEN);
    lv_chart_set_axis_range(p->chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(p->chart, 5, 0);
    p->ser = lv_chart_add_series(p->chart, md_color(md_scheme()->c.primary), LV_CHART_AXIS_PRIMARY_Y);
    p->chart_note = sa_note(body, "");
    p->last_hist_count = UINT32_MAX;

    md_section_create(body, "Charging");
    lv_obj_t *r = md_row_create(body, MD_ICON_BATTERY_CHARGING_FULL, "Stop charging at 80%",
                                "Helps the battery last longer. Charging resumes at 75%.");
    p->limit = md_row_switch(r, t5_battery_charge_limit());
    lv_obj_add_event_cb(p->limit, limit_changed, LV_EVENT_VALUE_CHANGED, NULL);

    md_section_create(body, "Average draw by app");
    p->apps = lv_obj_create(body);
    lv_obj_remove_style_all(p->apps);
    lv_obj_set_size(p->apps, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(p->apps, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *row = p->clear_row = sa_button_row(body);
    lv_obj_t *b = md_button_create(row, "Clear figures", MD_BUTTON_TEXT);
    lv_obj_add_event_cb(b, reset_clicked, LV_EVENT_CLICKED, NULL);
    sa_set_refresh(&s_nav, refresh, p, free);
}

static void on_destroy(t5_app_t *app)
{
    (void)app;
    sa_nav_destroy(&s_nav);
}

static const t5_app_desc_t s_desc = { .api = T5_API_LEVEL, .on_create = on_create, .on_destroy = on_destroy };

const t5_builtin_app_t sysapp_power = {
    .id = T5_APP_ID_POWER,
    .name = "Power",
    .desc = &s_desc,
    .perms = T5_PERM_SYSTEM,
    .orientation = T5_ORIENT_ANY,
    .icon = MD_ICON_BATTERY_CHARGING_FULL,
};
