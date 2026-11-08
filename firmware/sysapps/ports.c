/* SPDX-License-Identifier: Apache-2.0
 * Port Inspector (plan 6.9): for every named port and pin, the owner, mode
 * and level; the switched rails and who holds them; the system I2C bus with
 * per-device counters and live traffic; and a module scan of an external
 * bus with profile suggestions.
 */
#include <stdlib.h>
#include <string.h>
#include "t5/port.h"
#include "t5/system.h"
#include "sysapp_system.h"
#include "sysapps_priv.h"

static sa_nav_t s_nav;

/* ---- ports and pins -------------------------------------------------------------- */
typedef struct {
    lv_obj_t *list;
    uint32_t hash;
} pins_t;

static void pins_refresh(sa_nav_t *nav, void *arg)
{
    (void)nav;
    pins_t *p = arg;
    uint8_t gpio[64];
    int n = t5_ports_pins(gpio, 64);
    if (n > 64) n = 64;
    t5_pin_info_t info[64];
    uint32_t h = (uint32_t)n;
    for (int i = 0; i < n; i++) {
        if (t5_ports_pin_info(gpio[i], &info[i]) != T5_OK) memset(&info[i], 0, sizeof info[i]);
        h = h * 31 + (uint32_t)(info[i].level + 2) + (info[i].claimed << 3) + (uint32_t)info[i].owner[0];
    }
    if (h == p->hash) return;
    p->hash = h;
    lv_obj_clean(p->list);
    char title[32], sub[96];
    for (int i = 0; i < n; i++) {
        t5_pin_info_t *f = &info[i];
        snprintf(title, sizeof title, "G%u%s", gpio[i], f->risky ? " (risky)" : "");
        if (f->claimed)
            snprintf(sub, sizeof sub, "%s · %s as %s · level %s", f->owner, f->port ? f->port : "?",
                     t5_port_mode_name(f->mode), f->level < 0 ? "-" : f->level ? "high" : "low");
        else snprintf(sub, sizeof sub, "free · level %s", f->level < 0 ? "-" : f->level ? "high" : "low");
        md_row_create(p->list, f->claimed ? MD_ICON_LOCK : MD_ICON_LOCK_OPEN, title, sub);
    }
}

static void open_pins(lv_event_t *e)
{
    (void)e;
    pins_t *p = calloc(1, sizeof *p);
    if (!p) return;
    lv_obj_t *body = sa_push(&s_nav, "Pins");
    p->list = lv_obj_create(body);
    lv_obj_remove_style_all(p->list);
    lv_obj_set_size(p->list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(p->list, LV_FLEX_FLOW_COLUMN);
    p->hash = UINT32_MAX;
    sa_set_refresh(&s_nav, pins_refresh, p, free);
}

/* ---- rails -------------------------------------------------------------------------- */
typedef struct {
    lv_obj_t *row[T5_RAIL_COUNT];
} rails_t;

static void rails_refresh(sa_nav_t *nav, void *arg)
{
    (void)nav;
    rails_t *r = arg;
    for (int i = 0; i < T5_RAIL_COUNT; i++) {
        const char *owners[8];
        int n = t5_rail_owners((t5_rail_t)i, owners, 8);
        char buf[128] = "";
        int k = snprintf(buf, sizeof buf, "%s · %d holder%s", t5_rail_is_on((t5_rail_t)i) ? "on" : "off", n,
                         n == 1 ? "" : "s");
        for (int j = 0; j < n && j < 8 && k < (int)sizeof buf - 2; j++)
            k += snprintf(buf + k, sizeof buf - (size_t)k, "%s%s", j ? ", " : ": ", owners[j]);
        uint32_t err = t5_rail_errors((t5_rail_t)i);
        if (err && k < (int)sizeof buf - 2) snprintf(buf + k, sizeof buf - (size_t)k, " · %u errors", (unsigned)err);
        md_row_set_secondary(r->row[i], buf);
    }
}

static void open_rails(lv_event_t *e)
{
    (void)e;
    rails_t *r = calloc(1, sizeof *r);
    if (!r) return;
    lv_obj_t *body = sa_push(&s_nav, "Power rails");
    for (int i = 0; i < T5_RAIL_COUNT; i++) r->row[i] = md_row_create(body, MD_ICON_BOLT, t5_rail_name((t5_rail_t)i), " ");
    sa_note(body, "Rails are reference counted: a rail is on while anyone holds it, and apps' holds are dropped when "
                  "they exit.");
    sa_set_refresh(&s_nav, rails_refresh, r, free);
}

/* ---- I2C ------------------------------------------------------------------------------ */
typedef struct {
    lv_obj_t *bus, *devs, *trace;
    bool tracing;
} i2c_t;

static void trace_toggle(lv_event_t *e)
{
    i2c_t *p = s_nav.lv[s_nav.depth - 1].arg;
    p->tracing = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    t5_i2c_trace_enable(p->tracing);
}

static void i2c_refresh(sa_nav_t *nav, void *arg)
{
    (void)nav;
    i2c_t *p = arg;
    t5_i2c_bus_stats_t bs;
    t5_i2c_bus_stats(&bs);
    char buf[160];
    snprintf(buf, sizeof buf, "%u transfers, %u errors, %u recoveries (%u failed), %u lock timeouts",
             (unsigned)bs.xfers, (unsigned)bs.errors, (unsigned)bs.recoveries, (unsigned)bs.recovery_failures,
             (unsigned)bs.lock_timeouts);
    sa_set_text(p->bus, buf);
    t5_i2c_dev_stats_t ds[T5_I2C_MAX_DEVICES];
    int n = t5_i2c_dev_stats(ds, T5_I2C_MAX_DEVICES);
    lv_obj_clean(p->devs);
    char title[48];
    for (int i = 0; i < n; i++) {
        snprintf(title, sizeof title, "0x%02x %s", ds[i].addr, ds[i].name ? ds[i].name : "");
        snprintf(buf, sizeof buf, "%u transfers · %u errors (%u NACK, %u timeout)%s", (unsigned)ds[i].xfers,
                 (unsigned)ds[i].errors, (unsigned)ds[i].nacks, (unsigned)ds[i].timeouts,
                 ds[i].faulted ? " · FAULTED" : "");
        md_row_create(p->devs, ds[i].faulted ? MD_ICON_ERROR : MD_ICON_MEMORY, title, buf);
    }
    if (!p->tracing) return;
    t5_i2c_trace_t tr[24];
    int k = t5_i2c_trace_read(tr, 24);
    int off = 0;
    static char text[24 * 48];
    text[0] = 0;
    for (int i = 0; i < k && off < (int)sizeof text - 48; i++) {
        off += snprintf(text + off, sizeof text - (size_t)off, "%7u ms  0x%02x  reg %02x  w%u r%u  %s\n",
                        (unsigned)tr[i].t_ms, tr[i].addr, tr[i].first, tr[i].txlen, tr[i].rxlen,
                        tr[i].err ? t5_err_str((t5_err_t)tr[i].err) : "ok");
    }
    sa_set_text(p->trace, k ? text : "No traffic in the last second.");
}

static void i2c_free(void *arg)
{
    i2c_t *p = arg;
    if (p->tracing) t5_i2c_trace_enable(false);
    free(p);
}

static void open_i2c(lv_event_t *e)
{
    (void)e;
    i2c_t *p = calloc(1, sizeof *p);
    if (!p) return;
    lv_obj_t *body = sa_push(&s_nav, "System I2C bus");
    p->bus = sa_note(body, "");
    md_section_create(body, "Devices");
    p->devs = lv_obj_create(body);
    lv_obj_remove_style_all(p->devs);
    lv_obj_set_size(p->devs, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(p->devs, LV_FLEX_FLOW_COLUMN);
    md_section_create(body, "Traffic");
    lv_obj_t *r = md_row_create(body, MD_ICON_TERMINAL, "Record traffic", "The last transfers, refreshed every second");
    lv_obj_t *sw = md_row_switch(r, false);
    lv_obj_add_event_cb(sw, trace_toggle, LV_EVENT_VALUE_CHANGED, NULL);
    p->trace = md_label_create(body, "", MD_TYPE_CAPTION);
    lv_obj_set_style_pad_hor(p->trace, T5_DP(16), 0);
    sa_set_refresh(&s_nav, i2c_refresh, p, i2c_free);
}

/* ---- module scan ------------------------------------------------------------------------ */
typedef struct {
    lv_obj_t *out;
} scan_t;

static void scan_port(lv_event_t *e)
{
    const char *port = lv_event_get_user_data(e);
    scan_t *s = s_nav.lv[s_nav.depth - 1].arg;
    uint8_t found[32];
    int n = t5_profiles_scan(port, found, 32);
    char buf[512];
    int off = 0;
    if (n < 0) {
        snprintf(buf, sizeof buf, "Couldn't scan %s: %s (is the port in use?)", port, t5_err_str((t5_err_t)n));
    } else {
        off += snprintf(buf + off, sizeof buf - (size_t)off, "%s: %d device%s", port, n, n == 1 ? "" : "s");
        for (int i = 0; i < n && off < (int)sizeof buf - 8; i++)
            off += snprintf(buf + off, sizeof buf - (size_t)off, "%s0x%02x", i ? ", " : " at ", found[i]);
        t5_profile_match_t m[6];
        int k = t5_profiles_suggest(port, found, n, m, 6);
        for (int i = 0; i < k && off < (int)sizeof buf - 64; i++)
            off += snprintf(buf + off, sizeof buf - (size_t)off, "\nMaybe: %s%s", m[i].profile->name,
                            m[i].complete ? "" : " (partial match)");
    }
    sa_set_text(s->out, buf);
}

static void open_scan(lv_event_t *e)
{
    (void)e;
    scan_t *s = calloc(1, sizeof *s);
    if (!s) return;
    lv_obj_t *body = sa_push(&s_nav, "Module scan");
    sa_note(body, "Scans an external I2C bus for modules and matches them against the module profiles. Only external "
                  "buses are scanned, never the system bus.");
    lv_obj_t *row = sa_button_row(body);
    static const char *ports[] = { "grove", "ext" };
    for (size_t i = 0; i < sizeof ports / sizeof ports[0]; i++) {
        lv_obj_t *b = md_button_create(row, ports[i], MD_BUTTON_OUTLINED);
        lv_obj_add_event_cb(b, scan_port, LV_EVENT_CLICKED, (void *)ports[i]);
    }
    s->out = sa_note(body, "Pick a port.");
    sa_set_refresh(&s_nav, NULL, s, free);
}

/* ---- root: the ports -------------------------------------------------------------------- */
typedef struct {
    lv_obj_t *rows[16];
    int n;
} root_t;

static void root_refresh(sa_nav_t *nav, void *arg)
{
    (void)nav;
    root_t *r = arg;
    for (int i = 0; i < r->n; i++) {
        t5_port_info_t pi;
        if (t5_ports_info(i, &pi) != T5_OK) continue;
        char buf[128];
        int off = 0;
        const char *owner = NULL;
        t5_port_mode_t mode = T5_MODE_COUNT;
        for (int k = 0; k < pi.npins; k++) {
            t5_pin_info_t f;
            if (t5_ports_pin_info(pi.gpio[k], &f) == T5_OK && f.claimed && f.port && !strcmp(f.port, pi.name)) {
                static char own[T5_PORT_OWNER_LEN];
                snprintf(own, sizeof own, "%s", f.owner);
                owner = own;
                mode = f.mode;
            }
        }
        if (owner) off = snprintf(buf, sizeof buf, "%s as %s · ", owner, t5_port_mode_name(mode));
        else off = snprintf(buf, sizeof buf, "free · ");
        for (int k = 0; k < pi.npins && off < (int)sizeof buf - 12; k++)
            off += snprintf(buf + off, sizeof buf - (size_t)off, "%sG%u", k ? " " : "", pi.gpio[k]);
        md_row_set_secondary(r->rows[i], buf);
    }
}

static void on_create(t5_app_t *app)
{
    sa_nav_init(&s_nav, app);
    lv_obj_t *body = sa_push(&s_nav, "Port Inspector");
    lv_obj_t *r = md_row_create(body, MD_ICON_DEVELOPER_BOARD, "Pins", "Every port pin: owner, mode, level");
    md_row_set_clickable(r, open_pins, NULL);
    r = md_row_create(body, MD_ICON_BOLT, "Power rails", "USB 5V, external 5V, Wi-Fi, speaker, backlight");
    md_row_set_clickable(r, open_rails, NULL);
    r = md_row_create(body, MD_ICON_MEMORY, "System I2C bus", "Devices, errors, live traffic");
    md_row_set_clickable(r, open_i2c, NULL);
    r = md_row_create(body, MD_ICON_SEARCH, "Module scan", "Find modules on Grove or the header");
    md_row_set_clickable(r, open_scan, NULL);
    md_section_create(body, "Ports");
    root_t *rt = calloc(1, sizeof *rt);
    if (!rt) return;
    rt->n = t5_ports_count();
    if (rt->n > 16) rt->n = 16;
    for (int i = 0; i < rt->n; i++) {
        t5_port_info_t pi;
        if (t5_ports_info(i, &pi) != T5_OK) {
            rt->n = i;   /* the table is fixed; can't happen, but don't show garbage */
            break;
        }
        rt->rows[i] = md_row_create(body, MD_ICON_CABLE, pi.name, " ");
    }
    sa_set_refresh(&s_nav, root_refresh, rt, free);
}

static void on_destroy(t5_app_t *app)
{
    (void)app;
    sa_nav_destroy(&s_nav);
}

static const t5_app_desc_t s_desc = { .api = T5_API_LEVEL, .on_create = on_create, .on_destroy = on_destroy };

const t5_builtin_app_t sysapp_ports = {
    .id = "com.t5.ports",
    .name = "Port Inspector",
    .desc = &s_desc,
    .perms = T5_PERM_PORTS | T5_PERM_SYSTEM,
    .orientation = T5_ORIENT_ANY,
    .icon = MD_ICON_CABLE,
};
