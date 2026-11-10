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

/* ---- names people know ------------------------------------------------------------
 * The port table's ids (mbus.uart) are what code and the console use; the
 * screens lead with the name printed on the case or in the docs and keep
 * the id for the detail line. */
static const char *port_title(const char *id, char *buf, size_t n)
{
    static const struct {
        const char *id, *title;
    } k[] = {
        { "grove", "Grove" },
        { "mbus.spi", "M5-Bus SPI" },
        { "mbus.uart", "M5-Bus UART" },
        { "mbus.debug", "M5-Bus debug UART" },
        { "mbus.i2c", "M5-Bus I2C" },
        { "mbus.g35", "M5-Bus power key (G35)" },
        { "ext", "Expansion header" },
        { "rs485", "RS-485" },
        { "stamp", "Stamp pads" },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (!strcmp(id, k[i].id)) return k[i].title;
    if (!strncmp(id, "mbus.g", 6)) {
        snprintf(buf, n, "M5-Bus G%s", id + 6);
        return buf;
    }
    return id;
}

static const char *mode_title(t5_port_mode_t m)
{
    static const char *const t[T5_MODE_COUNT] = { [T5_MODE_I2C] = "I2C",   [T5_MODE_UART] = "UART",
                                                   [T5_MODE_SPI] = "SPI",   [T5_MODE_GPIO] = "GPIO",
                                                   [T5_MODE_PWM] = "PWM",   [T5_MODE_ADC] = "ADC",
                                                   [T5_MODE_RS485] = "RS-485" };
    return (m >= 0 && m < T5_MODE_COUNT && t[m]) ? t[m] : t5_port_mode_name(m);
}

/* Owners are app ids (cut to T5_PORT_OWNER_LEN) or a service name: show
 * the app's name when it is one. */
static t5_app_entry_t *s_ents;   /* heap: ~7 KB, only while the app runs */
static int s_n_ents = -1;

static const char *owner_title(const char *owner)
{
    if (!owner[0]) return "?";
    if (!strcmp(owner, "system")) return "Tab5 OS";
    if (s_n_ents < 0) {
        if (!s_ents) s_ents = calloc(T5_APPS_MAX, sizeof *s_ents);
        s_n_ents = s_ents ? t5_apps_list(s_ents, T5_APPS_MAX) : 0;
    }
    size_t n = strlen(owner);
    for (int i = 0; i < s_n_ents; i++)
        if (!strncmp(s_ents[i].id, owner, n) && s_ents[i].name[0]) return s_ents[i].name;
    return owner;
}

static const char *level_text(int level)
{
    return level < 0 ? "level unknown" : level ? "reads high" : "reads low";
}

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
    char title[64], sub[128], tb[32];
    int nports = t5_ports_count();
    for (int i = 0; i < n; i++) {
        t5_pin_info_t *f = &info[i];
        /* Title: the pin and where it comes out, from the first port that has it. */
        const char *where = NULL, *label = NULL;
        for (int k = 0; k < nports && !where; k++) {
            t5_port_info_t pi;
            if (t5_ports_info(k, &pi) != T5_OK || pi.shared) continue;
            for (int j = 0; j < pi.npins; j++)
                if (pi.gpio[j] == gpio[i]) {
                    where = port_title(pi.name, tb, sizeof tb);
                    label = pi.label[j];
                    break;
                }
        }
        if (where) snprintf(title, sizeof title, "G%u, %s %s", gpio[i], where, label ? label : "");
        else snprintf(title, sizeof title, "G%u", gpio[i]);
        int off;
        if (f->claimed)
            off = snprintf(sub, sizeof sub, "Used by %s as %s, %s", owner_title(f->owner), mode_title(f->mode),
                           level_text(f->level));
        else off = snprintf(sub, sizeof sub, "Free, %s", level_text(f->level));
        if (f->risky && off < (int)sizeof sub)
            snprintf(sub + off, sizeof sub - (size_t)off, ". Boot-sensitive pin");
        md_row_create(p->list, f->claimed ? MD_ICON_LOCK : MD_ICON_LOCK_OPEN, title, sub);
    }
}

static void open_pins(lv_event_t *e)
{
    (void)e;
    pins_t *p = calloc(1, sizeof *p);
    if (!p) return;
    lv_obj_t *body = sa_push(&s_nav, "Pins");
    sa_note(body, "Boot-sensitive pins are the power key and the chip's strapping pins. An app has to ask "
                  "specially to use them.");
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
        char buf[160] = "";
        bool on = t5_rail_is_on((t5_rail_t)i);
        int k = snprintf(buf, sizeof buf, "%s", on ? "On" : "Off");
        for (int j = 0; j < n && j < 8 && k < (int)sizeof buf - 2; j++)
            k += snprintf(buf + k, sizeof buf - (size_t)k, "%s%s", j ? ", " : ", used by ", owner_title(owners[j]));
        uint32_t err = t5_rail_errors((t5_rail_t)i);
        if (err && k < (int)sizeof buf - 2)
            snprintf(buf + k, sizeof buf - (size_t)k, ". %u switching error%s", (unsigned)err, err == 1 ? "" : "s");
        md_row_set_secondary(r->row[i], buf);
    }
}

static void open_rails(lv_event_t *e)
{
    (void)e;
    rails_t *r = calloc(1, sizeof *r);
    if (!r) return;
    lv_obj_t *body = sa_push(&s_nav, "Power rails");
    for (int i = 0; i < T5_RAIL_COUNT; i++) {
        static const char *const names[T5_RAIL_COUNT] = { [T5_RAIL_USB5V] = "USB-A power",
                                                          [T5_RAIL_EXT5V] = "External 5V (Grove, M5-Bus)",
                                                          [T5_RAIL_WLAN] = "Wi-Fi and Bluetooth radio",
                                                          [T5_RAIL_SPK] = "Speaker amplifier",
                                                          [T5_RAIL_BACKLIGHT] = "Screen backlight" };
        r->row[i] = md_row_create(body, MD_ICON_BOLT, names[i] ? names[i] : t5_rail_name((t5_rail_t)i), " ");
    }
    sa_note(body, "A rail stays on while anything uses it. When an app closes, the rails it used are released.");
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
    if (!p->tracing) lv_obj_set_hidden(p->trace, true);
}

static void i2c_refresh(sa_nav_t *nav, void *arg)
{
    (void)nav;
    i2c_t *p = arg;
    t5_i2c_bus_stats_t bs;
    t5_i2c_bus_stats(&bs);
    char buf[160];
    if (!bs.errors && !bs.recoveries && !bs.lock_timeouts)
        snprintf(buf, sizeof buf, "%u transfers, no errors", (unsigned)bs.xfers);
    else
        snprintf(buf, sizeof buf, "%u transfers, %u errors. Bus recovered %u times (%u failed), %u lock timeouts.",
                 (unsigned)bs.xfers, (unsigned)bs.errors, (unsigned)bs.recoveries, (unsigned)bs.recovery_failures,
                 (unsigned)bs.lock_timeouts);
    sa_set_text(p->bus, buf);
    t5_i2c_dev_stats_t ds[T5_I2C_MAX_DEVICES];
    int n = t5_i2c_dev_stats(ds, T5_I2C_MAX_DEVICES);
    /* Update rows in place: rebuilding them every second makes the list
     * flicker and drops a press in progress. */
    char title[48];
    for (int i = 0; i < n; i++) {
        snprintf(title, sizeof title, "%s (0x%02x)", ds[i].name ? ds[i].name : "Unknown device", ds[i].addr);
        if (ds[i].faulted)
            snprintf(buf, sizeof buf, "Not responding. %u errors (%u no answer, %u timed out)", (unsigned)ds[i].errors,
                     (unsigned)ds[i].nacks, (unsigned)ds[i].timeouts);
        else if (ds[i].errors)
            snprintf(buf, sizeof buf, "%u transfers, %u errors (%u no answer, %u timed out)", (unsigned)ds[i].xfers,
                     (unsigned)ds[i].errors, (unsigned)ds[i].nacks, (unsigned)ds[i].timeouts);
        else snprintf(buf, sizeof buf, "%u transfers, no errors", (unsigned)ds[i].xfers);
        lv_obj_t *row = i < (int)lv_obj_get_child_count(p->devs) ? lv_obj_get_child(p->devs, i) : NULL;
        if (!row) row = md_row_create(p->devs, MD_ICON_MEMORY, title, buf);
        md_row_set_text(row, title);
        md_row_set_secondary(row, buf);
        md_row_set_icon(row, ds[i].faulted ? MD_ICON_ERROR : MD_ICON_MEMORY);
    }
    while ((int)lv_obj_get_child_count(p->devs) > n) lv_obj_delete(lv_obj_get_child(p->devs, -1));
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
    lv_obj_set_hidden(p->trace, false);
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
    lv_obj_set_style_text_opa(p->bus, md_scheme()->on_surface.high, 0);
    md_section_create(body, "Devices");
    p->devs = lv_obj_create(body);
    lv_obj_remove_style_all(p->devs);
    lv_obj_set_size(p->devs, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(p->devs, LV_FLEX_FLOW_COLUMN);
    md_section_create(body, "Traffic");
    lv_obj_t *r = md_row_create(body, MD_ICON_TERMINAL, "Show live traffic", "The latest transfers, every second");
    lv_obj_t *sw = md_row_switch(r, false);
    lv_obj_add_event_cb(sw, trace_toggle, LV_EVENT_VALUE_CHANGED, NULL);
    p->trace = md_label_create(body, "", MD_TYPE_CAPTION);
    lv_obj_set_style_text_font(p->trace, md_font_mono(), 0);   /* columns line up */
    lv_obj_set_style_text_letter_space(p->trace, 0, 0);
    lv_obj_set_style_pad_hor(p->trace, T5_DP(16), 0);
    lv_obj_set_style_pad_bottom(p->trace, T5_DP(16), 0);
    lv_obj_set_hidden(p->trace, true);
    sa_set_refresh(&s_nav, i2c_refresh, p, i2c_free);
}

/* ---- module scan ------------------------------------------------------------------------ */
typedef struct {
    lv_obj_t *out, *found;
} scan_t;

/* The external I2C buses; also the app's manifest ports, which opening them needs. */
static const char *const s_scan_ports[] = { "grove", "ext", NULL };

static void scan_port(lv_event_t *e)
{
    const char *port = lv_event_get_user_data(e);
    scan_t *s = s_nav.lv[s_nav.depth - 1].arg;
    uint8_t found[32];
    int n = t5_profiles_scan(port, found, 32);
    char buf[512], tb[32];
    const char *pname = port_title(port, tb, sizeof tb);
    int off = 0;
    lv_obj_clean(s->found);
    if (n < 0) {
        snprintf(buf, sizeof buf, "Couldn't scan %s (%s). If an app is using the port, close it and try again.", pname,
                 t5_err_str((t5_err_t)n));
    } else if (n == 0) {
        snprintf(buf, sizeof buf, "Nothing answered on %s. Check the cable and that the module is powered.", pname);
    } else {
        off += snprintf(buf + off, sizeof buf - (size_t)off, "%d device%s on %s at ", n, n == 1 ? "" : "s", pname);
        for (int i = 0; i < n && off < (int)sizeof buf - 8; i++)
            off += snprintf(buf + off, sizeof buf - (size_t)off, "%s0x%02x", i ? ", " : "", found[i]);
        t5_profile_match_t m[6];
        int k = t5_profiles_suggest(port, found, n, m, 6);
        for (int i = 0; i < k; i++)
            md_row_create(s->found, MD_ICON_EXTENSION, m[i].profile->name,
                          m[i].complete ? "Matches every address" : "Matches some addresses");
        if (!k && off < (int)sizeof buf - 64)
            snprintf(buf + off, sizeof buf - (size_t)off, ". None match a known module.");
    }
    sa_set_text(s->out, buf);
}

static void open_scan(lv_event_t *e)
{
    (void)e;
    scan_t *s = calloc(1, sizeof *s);
    if (!s) return;
    lv_obj_t *body = sa_push(&s_nav, "Module scan");
    sa_note(body, "Looks for I2C modules plugged into Grove or the expansion header and names the ones it knows.");
    lv_obj_t *row = sa_button_row(body);
    char tb[32];
    for (size_t i = 0; s_scan_ports[i]; i++) {
        char label[48];
        snprintf(label, sizeof label, "Scan %s", port_title(s_scan_ports[i], tb, sizeof tb));
        lv_obj_t *b = md_button_create(row, label, MD_BUTTON_OUTLINED);
        lv_obj_add_event_cb(b, scan_port, LV_EVENT_CLICKED, (void *)s_scan_ports[i]);
    }
    s->out = sa_note(body, "");
    lv_obj_set_style_text_opa(s->out, md_scheme()->on_surface.high, 0);
    s->found = lv_obj_create(body);
    lv_obj_remove_style_all(s->found);
    lv_obj_set_size(s->found, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s->found, LV_FLEX_FLOW_COLUMN);
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
        char buf[160];
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
        if (pi.shared) off = snprintf(buf, sizeof buf, "Shared with the system bus. ");
        else if (owner) off = snprintf(buf, sizeof buf, "%s, %s. ", owner_title(owner), mode_title(mode));
        else off = snprintf(buf, sizeof buf, "Free. ");
        for (int k = 0; k < pi.npins && off < (int)sizeof buf - 16; k++)
            off += snprintf(buf + off, sizeof buf - (size_t)off, "%sG%u", k ? ", " : "", pi.gpio[k]);
        md_row_set_secondary(r->rows[i], buf);
    }
}

static void on_create(t5_app_t *app)
{
    sa_nav_init(&s_nav, app);
    lv_obj_t *body = sa_push(&s_nav, "Port Inspector");
    s_n_ents = -1;   /* re-read app names on each visit */
    lv_obj_t *r = md_row_create(body, MD_ICON_SEARCH, "Find modules",
                                "Look for I2C modules on Grove or the header");
    md_row_set_clickable(r, open_scan, NULL);
    r = md_row_create(body, MD_ICON_DEVELOPER_BOARD, "Pins", "Who uses each pin, and its level");
    md_row_set_clickable(r, open_pins, NULL);
    r = md_row_create(body, MD_ICON_BOLT, "Power rails", "Which outputs are on, and why");
    md_row_set_clickable(r, open_rails, NULL);
    r = md_row_create(body, MD_ICON_MEMORY, "System I2C bus", "Built-in chips, errors and live traffic");
    md_row_set_clickable(r, open_i2c, NULL);
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
        char tb[32];
        rt->rows[i] = md_row_create(body, MD_ICON_CABLE, port_title(pi.name, tb, sizeof tb), " ");
    }
    sa_set_refresh(&s_nav, root_refresh, rt, free);
}

static void on_destroy(t5_app_t *app)
{
    (void)app;
    sa_nav_destroy(&s_nav);
    free(s_ents);
    s_ents = NULL;
    s_n_ents = -1;
}

static const t5_app_desc_t s_desc = { .api = T5_API_LEVEL, .on_create = on_create, .on_destroy = on_destroy };

const t5_builtin_app_t sysapp_ports = {
    .id = T5_APP_ID_PORTS,
    .name = "Port Inspector",
    .desc = &s_desc,
    .perms = T5_PERM_PORTS | T5_PERM_SYSTEM,
    .orientation = T5_ORIENT_ANY,
    .ports = s_scan_ports,
    .icon = MD_ICON_CABLE,
};
