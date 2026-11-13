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
#include "t5_gui/t5ui.h"
#include "t5_gui/t5ui_icons.h"
#include "sysapp_system.h"
#include "sysapps_priv.h"

static sa_nav_t s_nav;

/* A list row, as md made them: grows when its second line wraps. */
static t5ui_obj_t *row_create(t5ui_obj_t *parent, const char *icon, const char *primary, const char *secondary)
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

/* A row that opens a page. */
static void page_row(t5ui_obj_t *parent, const char *icon, const char *primary, const char *secondary,
                     t5ui_event_cb_t cb)
{
    t5ui_on(row_create(parent, icon, primary, secondary), T5UI_EV_CLICK, cb, NULL);
}

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
    t5ui_obj_t *list;
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
    t5ui_clean(p->list);
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
        row_create(p->list, f->claimed ? T5UI_ICON_LOCK : T5UI_ICON_LOCK_OPEN, title, sub);
    }
}

static void open_pins(t5ui_event_t *e)
{
    (void)e;
    pins_t *p = calloc(1, sizeof *p);
    if (!p) return;
    t5ui_obj_t *body = sa_push(&s_nav, "Pins");
    sa_note(body, "Boot-sensitive pins are the power key and the chip's strapping pins. An app has to ask "
                  "specially to use them.");
    p->list = column(body);
    p->hash = UINT32_MAX;
    sa_set_refresh(&s_nav, pins_refresh, p, free);
}

/* ---- rails -------------------------------------------------------------------------- */
typedef struct {
    t5ui_obj_t *row[T5_RAIL_COUNT];
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
        t5ui_set_str(r->row[i], T5UI_P_SECONDARY, buf);
    }
}

static void open_rails(t5ui_event_t *e)
{
    (void)e;
    rails_t *r = calloc(1, sizeof *r);
    if (!r) return;
    t5ui_obj_t *body = sa_push(&s_nav, "Power rails");
    for (int i = 0; i < T5_RAIL_COUNT; i++) {
        static const char *const names[T5_RAIL_COUNT] = { [T5_RAIL_USB5V] = "USB-A power",
                                                          [T5_RAIL_EXT5V] = "External 5V (Grove, M5-Bus)",
                                                          [T5_RAIL_WLAN] = "Wi-Fi and Bluetooth radio",
                                                          [T5_RAIL_SPK] = "Speaker amplifier",
                                                          [T5_RAIL_BACKLIGHT] = "Screen backlight" };
        r->row[i] = row_create(body, T5UI_ICON_BOLT, names[i] ? names[i] : t5_rail_name((t5_rail_t)i), " ");
    }
    sa_note(body, "A rail stays on while anything uses it. When an app closes, the rails it used are released.");
    sa_set_refresh(&s_nav, rails_refresh, r, free);
}

/* ---- I2C ------------------------------------------------------------------------------ */
typedef struct {
    t5ui_obj_t *bus, *devs, *trace;
    bool tracing;
} i2c_t;

static void trace_toggle(t5ui_event_t *e)
{
    i2c_t *p = s_nav.lv[s_nav.depth - 1].arg;
    p->tracing = t5ui_get_int(t5ui_event_target(e), T5UI_P_CHECKED, 0);
    t5_i2c_trace_enable(p->tracing);
    if (!p->tracing) t5ui_set_int(p->trace, T5UI_P_HIDDEN, true);
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
        t5ui_obj_t *row = i < (int)t5ui_child_count(p->devs) ? t5ui_child(p->devs, i) : NULL;
        if (!row) row = row_create(p->devs, T5UI_ICON_MEMORY, title, buf);
        t5ui_set_str(row, T5UI_P_TEXT, title);
        t5ui_set_str(row, T5UI_P_SECONDARY, buf);
        t5ui_set_str(row, T5UI_P_ICON, ds[i].faulted ? T5UI_ICON_ERROR : T5UI_ICON_MEMORY);
    }
    while ((int)t5ui_child_count(p->devs) > n) t5ui_delete(t5ui_child(p->devs, -1));
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
    t5ui_set_int(p->trace, T5UI_P_HIDDEN, false);
}

static void i2c_free(void *arg)
{
    i2c_t *p = arg;
    if (p->tracing) t5_i2c_trace_enable(false);
    free(p);
}

static void open_i2c(t5ui_event_t *e)
{
    (void)e;
    i2c_t *p = calloc(1, sizeof *p);
    if (!p) return;
    t5ui_obj_t *body = sa_push(&s_nav, "System I2C bus");
    p->bus = sa_note(body, "");
    t5ui_set_int(p->bus, T5UI_P_TEXT_OPA, t5ui_theme_emphasis(T5UI_EMPHASIS_HIGH));
    section(body, "Devices");
    p->devs = column(body);
    section(body, "Traffic");
    t5ui_obj_t *r = row_create(body, T5UI_ICON_TERMINAL, "Show live traffic", "The latest transfers, every second");
    t5ui_obj_t *sw = t5ui_create(r, T5UI_MD_ROW_SWITCH);
    t5ui_on(sw, T5UI_EV_VALUE, trace_toggle, NULL);
    p->trace = t5ui_create(body, T5UI_MD_TEXT);
    t5ui_set_int(p->trace, T5UI_P_TEXT_STYLE, T5UI_TEXT_CAPTION);
    t5ui_set_int(p->trace, T5UI_P_TEXT_STYLE, T5UI_TEXT_MONO);   /* columns line up; the caption's line spacing stays */
    t5ui_set_int(p->trace, T5UI_P_LETTER_SPACE, 0);
    t5ui_set_int(p->trace, T5UI_P_PAD_LEFT, t5ui_dp(16));
    t5ui_set_int(p->trace, T5UI_P_PAD_RIGHT, t5ui_dp(16));
    t5ui_set_int(p->trace, T5UI_P_PAD_BOTTOM, t5ui_dp(16));
    t5ui_set_int(p->trace, T5UI_P_HIDDEN, true);
    sa_set_refresh(&s_nav, i2c_refresh, p, i2c_free);
}

/* ---- module scan ------------------------------------------------------------------------ */
typedef struct {
    t5ui_obj_t *out, *found;
} scan_t;

/* The external I2C buses; also the app's manifest ports, which opening them needs. */
static const char *const s_scan_ports[] = { "grove", "ext", NULL };

static void scan_port(t5ui_event_t *e)
{
    const char *port = t5ui_event_user(e);
    scan_t *s = s_nav.lv[s_nav.depth - 1].arg;
    uint8_t found[32];
    int n = t5_profiles_scan(port, found, 32);
    char buf[512], tb[32];
    const char *pname = port_title(port, tb, sizeof tb);
    int off = 0;
    t5ui_clean(s->found);
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
            row_create(s->found, T5UI_ICON_EXTENSION, m[i].profile->name,
                       m[i].complete ? "Matches every address" : "Matches some addresses");
        if (!k && off < (int)sizeof buf - 64)
            snprintf(buf + off, sizeof buf - (size_t)off, ". None match a known module.");
    }
    sa_set_text(s->out, buf);
}

static void open_scan(t5ui_event_t *e)
{
    (void)e;
    scan_t *s = calloc(1, sizeof *s);
    if (!s) return;
    t5ui_obj_t *body = sa_push(&s_nav, "Module scan");
    sa_note(body, "Looks for I2C modules plugged into Grove or the expansion header and names the ones it knows.");
    t5ui_obj_t *row = sa_button_row(body);
    char tb[32];
    for (size_t i = 0; s_scan_ports[i]; i++) {
        char label[48];
        snprintf(label, sizeof label, "Scan %s", port_title(s_scan_ports[i], tb, sizeof tb));
        t5ui_obj_t *b = t5ui_create(row, T5UI_MD_BUTTON_OUTLINED);
        t5ui_set_str(b, T5UI_P_TEXT, label);
        t5ui_on(b, T5UI_EV_CLICK, scan_port, (void *)s_scan_ports[i]);
    }
    s->out = sa_note(body, "");
    t5ui_set_int(s->out, T5UI_P_TEXT_OPA, t5ui_theme_emphasis(T5UI_EMPHASIS_HIGH));
    s->found = column(body);
    sa_set_refresh(&s_nav, NULL, s, free);
}

/* ---- root: the ports -------------------------------------------------------------------- */
typedef struct {
    t5ui_obj_t *rows[16];
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
        t5ui_set_str(r->rows[i], T5UI_P_SECONDARY, buf);
    }
}

static void on_create(t5_app_t *app)
{
    sa_nav_init(&s_nav, app);
    t5ui_obj_t *body = sa_push(&s_nav, "Port Inspector");
    s_n_ents = -1;   /* re-read app names on each visit */
    page_row(body, T5UI_ICON_SEARCH, "Find modules", "Look for I2C modules on Grove or the header", open_scan);
    page_row(body, T5UI_ICON_DEVELOPER_BOARD, "Pins", "Who uses each pin, and its level", open_pins);
    page_row(body, T5UI_ICON_BOLT, "Power rails", "Which outputs are on, and why", open_rails);
    page_row(body, T5UI_ICON_MEMORY, "System I2C bus", "Built-in chips, errors and live traffic", open_i2c);
    section(body, "Ports");
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
        rt->rows[i] = row_create(body, T5UI_ICON_CABLE, port_title(pi.name, tb, sizeof tb), " ");
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
    .icon = T5UI_ICON_CABLE,
};
