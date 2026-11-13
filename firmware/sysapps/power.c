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
#include "t5_gui/t5ui.h"
#include "t5_gui/t5ui_icons.h"
#include "sysapps_priv.h"


static sa_nav_t s_nav;

typedef struct {
    t5ui_obj_t *pct, *state, *cycles, *volts, *amps, *watts;
    t5ui_obj_t *chart;
    int32_t ser;
    t5ui_obj_t *chart_note, *apps, *limit, *clear_row;
    uint32_t last_hist_count;
    int apps_shown;
    t5_bat_hist_pt_t hist[T5_BAT_HIST_LEN];
    t5_app_entry_t ents[T5_APPS_MAX];   /* for app names in the draw list */
    int n_ents;
} power_t;

/* A live reading: the value is the hero, the label sits under it. Three
 * share a row in either orientation (flex-grow), so nothing wraps. */
static t5ui_obj_t *text(t5ui_obj_t *parent, const char *s, t5ui_text_t style)
{
    t5ui_obj_t *l = t5ui_create(parent, T5UI_MD_TEXT);
    t5ui_set_int(l, T5UI_P_TEXT_STYLE, style);
    t5ui_set_str(l, T5UI_P_TEXT, s);
    return l;
}

/* A list row, as md made them: grows when its second line wraps. */
static t5ui_obj_t *row(t5ui_obj_t *parent, const char *icon, const char *primary, const char *secondary)
{
    t5ui_obj_t *r = t5ui_create(parent, T5UI_MD_ROW);
    if (icon) t5ui_set_str(r, T5UI_P_ICON, icon);
    t5ui_set_str(r, T5UI_P_TEXT, primary ? primary : "");
    if (secondary) {
        t5ui_set_str(r, T5UI_P_SECONDARY, secondary);
        t5ui_set_int(r, T5UI_P_HEIGHT, T5UI_CONTENT);
        t5ui_set_int(r, T5UI_P_MIN_HEIGHT, t5ui_dp(64));
    }
    return r;
}

static void section(t5ui_obj_t *parent, const char *title)
{
    t5ui_set_str(t5ui_create(parent, T5UI_MD_SECTION), T5UI_P_TEXT, title);
}

/* A plain column filling the width. */
static t5ui_obj_t *column(t5ui_obj_t *parent)
{
    t5ui_obj_t *c = t5ui_create(parent, T5UI_BOX);
    t5ui_set_int(c, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(c, T5UI_P_HEIGHT, T5UI_CONTENT);
    t5ui_set_int(c, T5UI_P_FLOW, T5UI_COLUMN);
    return c;
}

static t5ui_obj_t *tile(t5ui_obj_t *parent, const char *caption, t5ui_obj_t **value)
{
    t5ui_obj_t *c = t5ui_create(parent, T5UI_MD_CARD);
    t5ui_set_int(c, T5UI_P_WIDTH, 0);
    t5ui_set_int(c, T5UI_P_HEIGHT, T5UI_CONTENT);
    t5ui_set_int(c, T5UI_P_GROW, 1);
    *value = text(c, "-", T5UI_TEXT_HEADLINE5);
    t5ui_obj_t *l = text(c, caption, T5UI_TEXT_BODY2);
    t5ui_set_int(l, T5UI_P_TEXT_OPA, t5ui_theme_emphasis(T5UI_EMPHASIS_MEDIUM));
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

static void limit_changed(t5ui_event_t *e)
{
    t5_battery_set_charge_limit(t5ui_get_int(t5ui_event_target(e), T5UI_P_CHECKED, 0));
}

static void reset_clicked(t5ui_event_t *e)
{
    (void)e;
    t5_apps_energy_reset();
    t5ui_snackbar("App figures cleared", NULL, NULL, NULL);
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
    if (!(t5ui_get_int(p->limit, T5UI_P_STATE, 0) & T5UI_STATE_PRESSED)) {
        t5ui_set_int(p->limit, T5UI_P_CHECKED, t5_battery_charge_limit());
    }

    int n = t5_battery_history(p->hist, T5_BAT_HIST_LEN);
    if ((uint32_t)n != p->last_hist_count || !n) {
        p->last_hist_count = (uint32_t)n;
        t5ui_cmd(p->chart, T5UI_CMD_CHART_SET_ALL, p->ser, T5UI_CHART_NONE, 0, NULL);
        int off = T5_BAT_HIST_LEN - n;
        for (int i = 0; i < n; i++) {
            t5ui_cmd(p->chart, T5UI_CMD_CHART_SET, p->ser, off + i, p->hist[i].pct >= 0 ? p->hist[i].pct : T5UI_CHART_NONE,
                     NULL);
        }
        /* A line needs two points; until then the chart would be an empty
         * grid, so say when it fills in instead. */
        if (n >= 2) {
            t5ui_set_int(p->chart, T5UI_P_HIDDEN, false);
            char a[24];
            fmt_span(a, sizeof a, (uint32_t)n * T5_BAT_HIST_PERIOD_S);
            snprintf(buf, sizeof buf, "Last %s, newest on the right. The lines mark 25%% steps.", a);
        } else {
            t5ui_set_int(p->chart, T5UI_P_HIDDEN, true);
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
    t5ui_clean(p->apps);
    p->n_ents = t5_apps_list(p->ents, T5_APPS_MAX);
    int shown = 0;
    char a[24], sub[64];
    for (int i = 0; i < k; i++) {
        int32_t mw = t5_energy_avg_mw(&rec[i]);
        if (mw < 0) continue;
        sa_fmt_duration(a, sizeof a, rec[i].ms / 1000);
        snprintf(sub, sizeof sub, "%d mW on average, over %s", (int)mw, a);
        row(p->apps, T5UI_ICON_BOLT, pretty_id(p, rec[i].id), sub);
        shown++;
    }
    if (!shown)
        sa_note(p->apps, "Nothing yet. Use the Tab5 unplugged for a while: draw can only be measured on battery.");
    t5ui_set_int(p->clear_row, T5UI_P_HIDDEN, !shown);
}

static void on_create(t5_app_t *app)
{
    sa_nav_init(&s_nav, app);
    t5ui_obj_t *body = sa_push(&s_nav, "Power");
    power_t *p = calloc(1, sizeof *p);
    if (!p) return;
    p->apps_shown = -1;
    t5ui_obj_t *hdr = column(body);
    t5ui_set_int(hdr, T5UI_P_PAD_ALL, t5ui_dp(16));
    p->pct = text(hdr, "", T5UI_TEXT_HEADLINE4);
    p->state = text(hdr, "", T5UI_TEXT_SUBTITLE1);
    p->cycles = text(hdr, "", T5UI_TEXT_BODY2);
    t5ui_set_int(p->cycles, T5UI_P_TEXT_OPA, t5ui_theme_emphasis(T5UI_EMPHASIS_MEDIUM));

    t5ui_obj_t *tiles = t5ui_create(body, T5UI_BOX);
    t5ui_set_int(tiles, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(tiles, T5UI_P_HEIGHT, T5UI_CONTENT);
    t5ui_set_int(tiles, T5UI_P_PAD_LEFT, t5ui_dp(16));
    t5ui_set_int(tiles, T5UI_P_PAD_RIGHT, t5ui_dp(16));
    t5ui_set_int(tiles, T5UI_P_PAD_ROW, t5ui_dp(8));
    t5ui_set_int(tiles, T5UI_P_PAD_COLUMN, t5ui_dp(8));
    t5ui_set_int(tiles, T5UI_P_FLOW, T5UI_ROW);
    tile(tiles, "Voltage", &p->volts);
    tile(tiles, "Current", &p->amps);
    tile(tiles, "Power", &p->watts);

    section(body, "Battery level");
    p->chart = t5ui_create(body, T5UI_CHART);
    t5ui_set_int(p->chart, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(p->chart, T5UI_P_HEIGHT, t5ui_dp(120));
    t5ui_set_int(p->chart, T5UI_P_PAD_LEFT, t5ui_dp(16));
    t5ui_set_int(p->chart, T5UI_P_PAD_RIGHT, t5ui_dp(16));
    t5ui_set_int(p->chart, T5UI_P_PAD_TOP, t5ui_dp(8));
    t5ui_set_int(p->chart, T5UI_P_PAD_BOTTOM, t5ui_dp(8));
    t5ui_set_int(p->chart, T5UI_P_BG_OPA, T5UI_OPA_TRANSP);
    t5ui_set_int(p->chart, T5UI_P_BORDER_WIDTH, 0);
    t5ui_set_color(p->chart, T5UI_P_LINE_COLOR, t5ui_theme_color(T5UI_COLOR_ON_SURFACE));   /* the grid */
    t5ui_set_int(p->chart, T5UI_P_LINE_OPA, t5ui_theme_emphasis(T5UI_EMPHASIS_DIVIDER));
    /* No point markers: a t5ui chart draws none. */
    t5ui_set_int_at(p->chart, T5UI_P_LINE_WIDTH, T5UI_AT(T5UI_PART_ITEMS, T5UI_STATE_DEFAULT), t5ui_dp(2));
    t5ui_set_int(p->chart, T5UI_P_CHART_TYPE, T5UI_CHART_LINE);
    t5ui_set_int(p->chart, T5UI_P_COUNT, T5_BAT_HIST_LEN);
    t5ui_set_int(p->chart, T5UI_P_MIN, 0);
    t5ui_set_int(p->chart, T5UI_P_MAX, 100);
    t5ui_set_int(p->chart, T5UI_P_CHART_GRID_ROWS, 5);
    t5ui_set_int(p->chart, T5UI_P_CHART_GRID_COLUMNS, 0);
    p->ser = t5ui_cmd(p->chart, T5UI_CMD_CHART_ADD_SERIES, (int32_t)t5ui_theme_color(T5UI_COLOR_PRIMARY), 0, 0, NULL);
    p->chart_note = sa_note(body, "");
    p->last_hist_count = UINT32_MAX;

    section(body, "Charging");
    t5ui_obj_t *r = row(body, T5UI_ICON_BATTERY_CHARGING_FULL, "Stop charging at 80%",
                        "Helps the battery last longer. Charging resumes at 75%.");
    p->limit = t5ui_create(r, T5UI_MD_ROW_SWITCH);
    t5ui_set_int(p->limit, T5UI_P_CHECKED, t5_battery_charge_limit());
    t5ui_on(p->limit, T5UI_EV_VALUE, limit_changed, NULL);

    section(body, "Average draw by app");
    p->apps = column(body);
    t5ui_obj_t *row_b = p->clear_row = sa_button_row(body);
    t5ui_obj_t *b = t5ui_create(row_b, T5UI_MD_BUTTON_TEXT);
    t5ui_set_str(b, T5UI_P_TEXT, "Clear figures");
    t5ui_on(b, T5UI_EV_CLICK, reset_clicked, NULL);
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
    .icon = T5UI_ICON_BATTERY_CHARGING_FULL,
};
