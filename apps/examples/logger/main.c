/* Logger (plan 9.5): a scheduled sensor logger that spends its life in soft
 * off.
 *
 * Every interval the Tab5 wakes from soft off and runs on_alarm with no
 * screen: it reads a temperature and humidity sensor on the Grove port
 * (SHT4x or SHT3x at 0x44; without one it logs only the battery), appends a
 * line to log.csv in the app's data folder, POSTs the line to an upload URL
 * if one is set (plain HTTP, Wi-Fi permitting), schedules the next wake and
 * returns. The OS then goes straight back to soft off.
 *
 * Opened normally, it shows the latest readings and a chart, and sets the
 * interval, the upload URL and whether logging is on.
 *
 * Files in the data folder: log.csv ("utc,temp_c,rh_pct,battery_mv"),
 * config ("interval_s url" on one line), off (present when logging is off).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <t5/app.h>
#include <t5/port.h>
#include <t5/power.h>
#include <t5/shell.h>
#include <t5/storage.h>
#include <t5/sys.h>
#include <t5/ui.h>

#define SHT_ADDR 0x44
#define MAX_POINTS 96

typedef struct {
    int interval_s;
    char url[160];
} cfg_t;

typedef struct {
    float t, rh;
    int mv;
    bool have_sensor;
} reading_t;

/* ---- files ---------------------------------------------------------------------------- */
static void path(t5_app_t *app, const char *name, char *out, size_t n)
{
    snprintf(out, n, "%s/%s", t5_app_data_dir(app), name);
}

static void cfg_load(t5_app_t *app, cfg_t *c)
{
    char p[200];
    c->interval_s = 3600;
    c->url[0] = 0;
    path(app, "config", p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[200];
    if (fgets(line, sizeof line, f)) {
        int iv = 0;
        char url[160] = "";
        if (sscanf(line, "%d %159s", &iv, url) >= 1 && iv >= 60) c->interval_s = iv;
        if (strncmp(url, "http://", 7) == 0) snprintf(c->url, sizeof c->url, "%s", url);
    }
    fclose(f);
}

static void cfg_save(t5_app_t *app, const cfg_t *c)
{
    char p[200], line[200];
    path(app, "config", p, sizeof p);
    int n = snprintf(line, sizeof line, "%d %s\n", c->interval_s, c->url[0] ? c->url : "-");
    t5_storage_write_atomic(p, line, (size_t)n);
}

static bool logging_on(t5_app_t *app)
{
    char p[200];
    path(app, "off", p, sizeof p);
    FILE *f = fopen(p, "r");
    if (f) fclose(f);
    return f == NULL;
}

/* ---- the sensor ------------------------------------------------------------------------- */
static uint8_t crc8(const uint8_t *d, int n)
{
    uint8_t c = 0xff;
    for (int i = 0; i < n; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++) c = (uint8_t)(c & 0x80 ? (c << 1) ^ 0x31 : c << 1);
    }
    return c;
}

/* SHT4x: one-byte command 0xFD; SHT3x: 0x2400. Both answer 6 bytes. */
static bool read_sht(reading_t *r)
{
    t5_port_i2c_cfg_t cfg = { .hz = 100000 };
    t5_port_t *p = NULL;
    if (t5_port_open("grove", T5_MODE_I2C, &cfg, &p) != T5_OK) return false;
    uint8_t rx[6];
    bool ok = false;
    static const uint8_t cmd4[] = { 0xFD }, cmd3[] = { 0x24, 0x00 };
    const uint8_t *cmds[] = { cmd4, cmd3 };
    const size_t lens[] = { 1, 2 };
    for (int i = 0; i < 2 && !ok; i++) {
        if (t5_port_i2c_xfer(p, SHT_ADDR, cmds[i], lens[i], NULL, 0) != T5_OK) continue;
        vTaskDelay(pdMS_TO_TICKS(20));
        if (t5_port_i2c_xfer(p, SHT_ADDR, NULL, 0, rx, 6) != T5_OK) continue;
        if (crc8(rx, 2) != rx[2] || crc8(rx + 3, 2) != rx[5]) continue;
        uint16_t st = (uint16_t)(rx[0] << 8 | rx[1]), srh = (uint16_t)(rx[3] << 8 | rx[4]);
        r->t = -45.0f + 175.0f * st / 65535.0f;
        r->rh = i == 0 ? -6.0f + 125.0f * srh / 65535.0f : 100.0f * srh / 65535.0f;
        if (r->rh < 0) r->rh = 0;
        if (r->rh > 100) r->rh = 100;
        ok = true;
    }
    T5_IGNORE(t5_port_close(p));   /* opened above; nothing to do if closing fails */
    return ok;
}

static void take_reading(reading_t *r)
{
    memset(r, 0, sizeof *r);
    r->have_sensor = read_sht(r);
    t5_battery_status_t b;
    t5_battery_status(&b);
    r->mv = b.mv;
}

static int format_line(const reading_t *r, char *out, size_t n)
{
    if (r->have_sensor) return snprintf(out, n, "%lld,%.2f,%.1f,%d\n", (long long)time(NULL), r->t, r->rh, r->mv);
    return snprintf(out, n, "%lld,,,%d\n", (long long)time(NULL), r->mv);
}

/* ---- upload: one small HTTP POST, plain text ------------------------------------------- */
static bool post_line(const char *url, const char *line)
{
    char host[96] = "", port[8] = "80", rest[96] = "/";
    const char *h = url + 7;
    const char *slash = strchr(h, '/');
    size_t hl = slash ? (size_t)(slash - h) : strlen(h);
    if (hl >= sizeof host) return false;
    memcpy(host, h, hl);
    if (slash) snprintf(rest, sizeof rest, "%s", slash);
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = 0;
        snprintf(port, sizeof port, "%s", colon + 1);
    }
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai = NULL;
    if (lwip_getaddrinfo(host, port, &hints, &ai) != 0 || !ai) return false;
    int s = lwip_socket(AF_INET, SOCK_STREAM, 0);
    bool ok = false;
    if (s >= 0) {
        struct timeval tv = { .tv_sec = 5 };
        lwip_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        lwip_setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (lwip_connect(s, ai->ai_addr, ai->ai_addrlen) == 0) {
            char req[400];
            int n = snprintf(req, sizeof req,
                             "POST %s HTTP/1.0\r\nHost: %s\r\nContent-Type: text/csv\r\nContent-Length: %u\r\n\r\n%s",
                             rest, host, (unsigned)strlen(line), line);
            char resp[16] = "";
            ok = lwip_send(s, req, (size_t)n, 0) == n && lwip_recv(s, resp, sizeof resp - 1, 0) > 9 &&
                 resp[9] == '2';
        }
        lwip_close(s);
    }
    lwip_freeaddrinfo(ai);
    return ok;
}

static void append(t5_app_t *app, const char *line)
{
    char p[200];
    path(app, "log.csv", p, sizeof p);
    FILE *f = fopen(p, "a");
    if (f) {
        fputs(line, f);
        fclose(f);
    }
}

/* ---- background run ---------------------------------------------------------------------------- */
static void on_alarm(t5_app_t *app)
{
    cfg_t c;
    cfg_load(app, &c);
    if (!logging_on(app)) return;           /* turned off: no next wake */
    /* Schedule first, so a failure below doesn't end the series. */
    t5_err_t ae = t5_app_schedule_alarm(app, time(NULL) + c.interval_s);
    if (ae != T5_OK) {
        char note[64];
        snprintf(note, sizeof note, "#next wake not scheduled: %s\n", t5_err_str(ae));
        append(app, note);
    }
    reading_t r;
    take_reading(&r);
    char line[96];
    format_line(&r, line, sizeof line);
    append(app, line);
    if (c.url[0]) {
        /* Wi-Fi comes up on its own after a wake; give it a few seconds. */
        t5_net_status_t ns;
        for (int i = 0; i < 80; i++) {
            t5_net_status(&ns);
            if (ns.state == T5_NET_CONNECTED) break;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (ns.state == T5_NET_CONNECTED && !post_line(c.url, line)) append(app, "#upload failed\n");
    }
}

/* ---- the screen ------------------------------------------------------------------------------- */
typedef struct {
    t5_app_t *app;
    cfg_t cfg;
    lv_obj_t *now, *sub, *chart, *sw, *url;
    lv_chart_series_t *ser;
    int ivl;
} ui_t;

static const int s_intervals[] = { 900, 1800, 3600, 3 * 3600, 6 * 3600 };
static const char *const s_interval_names[] = { "Every 15 minutes", "Every 30 minutes", "Every hour",
                                                "Every 3 hours", "Every 6 hours" };

static void load_chart(ui_t *u)
{
    char p[200];
    path(u->app, "log.csv", p, sizeof p);
    FILE *f = fopen(p, "r");
    lv_chart_set_all_values(u->chart, u->ser, LV_CHART_POINT_NONE);
    if (!f) return;
    char line[96];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char *c1 = strchr(line, ',');
        if (!c1 || c1[1] == ',') continue;
        lv_chart_set_next_value(u->chart, u->ser, (int32_t)(atof(c1 + 1) * 10));
    }
    fclose(f);
}

static void show(ui_t *u, const reading_t *r)
{
    char buf[96];
    if (r->have_sensor) snprintf(buf, sizeof buf, "%.1f °C  %.0f%%", r->t, r->rh);
    else snprintf(buf, sizeof buf, "No sensor on Grove");
    lv_label_set_text(u->now, buf);
    snprintf(buf, sizeof buf, "Battery %d.%02d V · %s · %s", r->mv / 1000, r->mv % 1000 / 10,
             s_interval_names[u->ivl], logging_on(u->app) ? "logging" : "paused");
    lv_label_set_text(u->sub, buf);
}

static void read_now(lv_event_t *e)
{
    ui_t *u = lv_event_get_user_data(e);
    reading_t r;
    take_reading(&r);
    char line[96];
    format_line(&r, line, sizeof line);
    append(u->app, line);
    show(u, &r);
    load_chart(u);
}

static void toggle(lv_event_t *e)
{
    ui_t *u = lv_event_get_user_data(e);
    char p[200];
    path(u->app, "off", p, sizeof p);
    if (lv_obj_has_state(u->sw, LV_STATE_CHECKED)) {
        remove(p);
        t5_err_t err = t5_app_schedule_alarm(u->app, time(NULL) + u->cfg.interval_s);
        t5_shell_toast(err == T5_OK ? "Logging: the next reading wakes the Tab5 from off" : "Can't schedule a wake");
    } else {
        t5_storage_write_atomic(p, "1", 1);
        t5_shell_toast("Logging paused");
    }
}

static void interval_next(lv_event_t *e)
{
    ui_t *u = lv_event_get_user_data(e);
    u->ivl = (u->ivl + 1) % (int)(sizeof s_intervals / sizeof s_intervals[0]);
    u->cfg.interval_s = s_intervals[u->ivl];
    cfg_save(u->app, &u->cfg);
    lv_label_set_text(lv_obj_get_child(lv_event_get_target_obj(e), 0), s_interval_names[u->ivl]);
}

static void url_done(lv_event_t *e)
{
    ui_t *u = lv_event_get_user_data(e);
    const char *t = md_textfield_text(u->url);
    if (t[0] && strncmp(t, "http://", 7)) {
        md_textfield_set_helper(u->url, "Starts with http://", true);
        return;
    }
    snprintf(u->cfg.url, sizeof u->cfg.url, "%s", t);
    cfg_save(u->app, &u->cfg);
    md_textfield_set_helper(u->url, t[0] ? "Each reading is POSTed here" : "No upload", false);
    t5_shell_keyboard_hide();
}

static void back(lv_event_t *e)
{
    (void)e;
    t5_app_exit(t5_app_self());
}

static void on_create(t5_app_t *app)
{
    ui_t *u = calloc(1, sizeof *u);
    t5_app_set_user(app, u);
    u->app = app;
    cfg_load(app, &u->cfg);
    for (int i = 0; i < (int)(sizeof s_intervals / sizeof s_intervals[0]); i++) {
        if (s_intervals[i] == u->cfg.interval_s) u->ivl = i;
    }
    lv_obj_t *page = md_page_create(t5_app_screen(app), "Logger", back, NULL, NULL);
    lv_obj_t *b = md_page_body(page);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(b, T5_DP(16), 0);
    lv_obj_set_style_pad_row(b, T5_DP(8), 0);
    u->now = md_label_create(b, "", MD_TYPE_HEADLINE4);
    u->sub = md_label_create(b, "", MD_TYPE_BODY2);
    u->chart = lv_chart_create(b);
    lv_obj_set_size(u->chart, LV_PCT(100), T5_DP(140));
    lv_obj_set_style_bg_opa(u->chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(u->chart, 0, 0);
    lv_obj_set_style_size(u->chart, 0, 0, LV_PART_INDICATOR);
    lv_chart_set_point_count(u->chart, MAX_POINTS);
    lv_chart_set_axis_range(u->chart, LV_CHART_AXIS_PRIMARY_Y, 0, 400);
    lv_chart_set_update_mode(u->chart, LV_CHART_UPDATE_MODE_SHIFT);
    u->ser = lv_chart_add_series(u->chart, md_color(md_scheme()->c.secondary), LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_t *row = md_row_create(b, MD_ICON_SCHEDULE, "Log in the background", "Wakes the Tab5 from off");
    u->sw = md_row_switch(row, logging_on(app));
    lv_obj_add_event_cb(u->sw, toggle, LV_EVENT_VALUE_CHANGED, u);
    lv_obj_t *iv = md_button_create(b, s_interval_names[u->ivl], MD_BUTTON_OUTLINED);
    lv_obj_add_event_cb(iv, interval_next, LV_EVENT_CLICKED, u);
    u->url = md_textfield_create(b, "Upload URL (http://...)", false);
    lv_obj_set_width(u->url, LV_PCT(100));
    if (u->cfg.url[0]) md_textfield_set_text(u->url, u->cfg.url);
    lv_obj_add_event_cb(md_textfield_ta(u->url), url_done, LV_EVENT_READY, u);
    lv_obj_t *rn = md_button_create(b, "Read now", MD_BUTTON_CONTAINED);
    lv_obj_add_event_cb(rn, read_now, LV_EVENT_CLICKED, u);
    reading_t r;
    take_reading(&r);
    show(u, &r);
    load_chart(u);
}

static void on_destroy(t5_app_t *app)
{
    free(t5_app_get_user(app));
}

T5_APP(.on_create = on_create, .on_destroy = on_destroy, .on_alarm = on_alarm);
