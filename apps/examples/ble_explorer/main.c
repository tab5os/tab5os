/* BLE Explorer: the Bluetooth LE API (t5/ble.h) in both roles.
 *
 *   Scan (the button at the top): lists nearby devices with their
 *   signal, newest names filled in as scan responses arrive.
 *   Tap a device: connects, discovers its GATT database, reads its name
 *   (0x2a00) and battery level (0x2a19) if it has them, and disconnects.
 *   Serve (the button next to it): serves the battery service (0x180f)
 *   with the Tab5's own level, notified every 5 s, and advertises as
 *   "Tab5 Explorer" so a phone can connect.
 *
 * The blocking calls run on a worker task; the UI is updated with
 * t5_ui_async. While it scans or serves, an activity in the status bar says
 * so. Everything it opens is closed by the OS if it exits mid-way.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <t5/app.h>
#include <t5/ble.h>
#include <t5/power.h>
#include <t5/shell.h>
#include <t5/ui.h>

#define MAX_DEVS 40

typedef struct {
    uint8_t addr[6], type;
    int8_t rssi;
    char name[T5_BLE_NAME_LEN];
} peer_t;

typedef enum { JOB_NONE, JOB_SCAN, JOB_PROBE, JOB_SERVE } job_t;

static struct {
    t5_app_t *app;
    lv_obj_t *status, *list;
    t5_mutex_t mx;
    peer_t devs[MAX_DEVS];
    int ndevs;
    volatile job_t job;               /* the worker's next or current job */
    volatile bool serving;            /* the peripheral is on; cleared to stop it */
    int probe;                        /* device index for JOB_PROBE */
    t5_task_t worker;
} s;

/* ---- UI (UI task) ------------------------------------------------------------ */
static void set_status_ui(void *text)
{
    lv_label_set_text(s.status, text);
    free(text);
}

static void status(const char *fmt, const char *arg)
{
    char *t = malloc(96);
    if (!t) return;
    snprintf(t, 96, fmt, arg ? arg : "");
    if (t5_ui_async(set_status_ui, t) != T5_OK) free(t);
}

static void bind(md_list_row_t *row, uint32_t i, void *ud)
{
    (void)ud;
    peer_t d;
    t5_mutex_lock(s.mx);
    d = s.devs[i];
    t5_mutex_unlock(s.mx);
    char a[18], sub[48];
    t5_ble_format_addr(d.addr, a);
    snprintf(sub, sizeof sub, "%s  ·  %d dBm", a, d.rssi);
    lv_label_set_text(row->primary, d.name[0] ? d.name : "(no name)");
    if (row->secondary) lv_label_set_text(row->secondary, sub);
}

static void refresh_ui(void *arg)
{
    (void)arg;
    t5_mutex_lock(s.mx);
    int n = s.ndevs;
    t5_mutex_unlock(s.mx);
    md_list_set_count(s.list, (uint32_t)n);
    md_list_refresh(s.list);
}

static void clicked(uint32_t i, void *ud)
{
    (void)ud;
    if (s.job != JOB_NONE) {
        T5_IGNORE(t5_shell_toast("Busy: wait for the scan to finish"));   /* a toast failing is harmless */
        return;
    }
    s.probe = (int)i;
    s.job = JOB_PROBE;
}

static void scan_clicked(lv_event_t *e)
{
    (void)e;
    if (!t5_ble_ready()) {
        T5_IGNORE(t5_shell_toast("Turn on Bluetooth in Settings"));
        return;
    }
    if (s.job == JOB_NONE) s.job = JOB_SCAN;
}

static void serve_clicked(lv_event_t *e)
{
    (void)e;
    if (s.serving) s.serving = false;               /* the worker stops it */
    else if (s.job == JOB_NONE) s.job = JOB_SERVE;
}

/* ---- the worker's jobs ------------------------------------------------------- */
static void add(const t5_ble_report_t *r)
{
    t5_mutex_lock(s.mx);
    int i = 0;
    while (i < s.ndevs && memcmp(s.devs[i].addr, r->addr, 6)) i++;
    if (i == s.ndevs && s.ndevs < MAX_DEVS) {
        memset(&s.devs[i], 0, sizeof s.devs[i]);
        memcpy(s.devs[i].addr, r->addr, 6);
        s.devs[i].type = r->addr_type;
        s.ndevs++;
    }
    if (i < s.ndevs) {
        s.devs[i].rssi = r->rssi;
        if (r->name[0]) snprintf(s.devs[i].name, sizeof s.devs[i].name, "%s", r->name);
    }
    t5_mutex_unlock(s.mx);
}

static void do_scan(void)
{
    t5_shell_activity_t *act = NULL;
    T5_IGNORE(t5_shell_activity_start(MD_ICON_SEARCH, "Scanning for Bluetooth devices", &act));  /* optional */
    t5_mutex_lock(s.mx);
    s.ndevs = 0;
    t5_mutex_unlock(s.mx);
    t5_ble_scanner_t *sc;
    t5_err_t e = t5_ble_scan_start(&(t5_ble_scan_cfg_t){ .duration_ms = 6000, .active = true }, &sc);
    if (e != T5_OK) {
        status("Can't scan: %s", t5_err_str(e));
    } else {
        status("Scanning...", NULL);
        t5_ble_report_t r;
        uint32_t last = 0;
        while (t5_app_running(s.app) && t5_ble_scan_next(sc, &r, 500) != T5_ERR_STATE) {
            if (r.data_len || r.name[0]) add(&r);
            uint32_t now = (uint32_t)(t5_time_us() / 1000);
            if (now - last > 400) {                 /* redraw at most every 400 ms */
                last = now;
                T5_IGNORE(t5_ui_async(refresh_ui, NULL));
            }
        }
        T5_IGNORE(t5_ble_scan_stop(sc));
        T5_IGNORE(t5_ui_async(refresh_ui, NULL));
        char n[8];
        snprintf(n, sizeof n, "%d", s.ndevs);
        status("%s devices. Tap one to read it.", n);
    }
    if (act) T5_IGNORE(t5_shell_activity_end(act));
}

static void do_probe(void)
{
    peer_t d;
    t5_mutex_lock(s.mx);
    d = s.devs[s.probe];
    t5_mutex_unlock(s.mx);
    status("Connecting to %s...", d.name[0] ? d.name : "the device");
    t5_ble_conn_t *c;
    t5_err_t e = t5_ble_conn_open(d.addr, d.type, 8000, &c);
    if (e != T5_OK) {
        status("Couldn't connect: %s", t5_err_str(e));
        return;
    }
    char msg[160];
    e = t5_ble_gatt_discover(c, 10000);
    t5_ble_chr_t ch[T5_BLE_MAX_CHARS];
    int n = e == T5_OK ? t5_ble_gatt_chars(c, ch, T5_BLE_MAX_CHARS) : 0;
    int len = snprintf(msg, sizeof msg, "%d characteristics", n);
    t5_ble_chr_t one;
    char name[64];
    if (t5_ble_gatt_find(c, "0x1800", "0x2a00", &one) == T5_OK) {
        int k = t5_ble_gatt_read(c, one.val_handle, name, sizeof name - 1, 3000);
        if (k > 0) {
            name[k] = 0;
            len += snprintf(msg + len, sizeof msg - (size_t)len, ", named \"%s\"", name);
        }
    }
    uint8_t pct;
    if (t5_ble_gatt_find(c, "0x180f", "0x2a19", &one) == T5_OK && t5_ble_gatt_read(c, one.val_handle, &pct, 1, 3000) == 1) {
        snprintf(msg + len, sizeof msg - (size_t)len, ", battery %u%%", pct);
    }
    T5_IGNORE(t5_ble_conn_close(c));
    status("%s", msg);
}

static void do_serve(void)
{
    static const t5_ble_server_chr_t chrs[] = { { "0x2a19", T5_BLE_PROP_READ | T5_BLE_PROP_NOTIFY } };
    t5_ble_server_t *srv = NULL;
    t5_ble_advertiser_t *adv = NULL;
    t5_shell_activity_t *act = NULL;
    t5_err_t e = t5_ble_server_open("0x180f", chrs, 1, &srv);
    if (e == T5_OK) {
        e = t5_ble_adv_start(&(t5_ble_adv_cfg_t){ .name = "Tab5 Explorer", .svc_uuid = "0x180f", .connectable = true },
                             &adv);
    }
    if (e != T5_OK) {
        status("Can't serve: %s", t5_err_str(e));
        if (srv) T5_IGNORE(t5_ble_server_close(srv));
        return;
    }
    s.serving = true;
    T5_IGNORE(t5_shell_activity_start(MD_ICON_BLUETOOTH, "Serving the battery level", &act));
    status("Advertising as \"Tab5 Explorer\" with the battery service", NULL);
    uint32_t next = 0;
    while (s.serving && t5_app_running(s.app)) {
        t5_ble_server_ev_t ev;
        uint8_t buf[8];
        if (t5_ble_server_next(srv, &ev, buf, sizeof buf, 500) >= 0) {
            char a[18];
            t5_ble_format_addr(ev.addr, a);
            if (ev.type == T5_BLE_SERVER_CONNECTED) status("%s connected", a);
            if (ev.type == T5_BLE_SERVER_DISCONNECTED) status("%s left; still advertising", a);
        }
        uint32_t now = (uint32_t)(t5_time_us() / 1000);
        if (now >= next) {
            next = now + 5000;
            uint8_t pct = (uint8_t)t5_battery_percent();
            T5_IGNORE(t5_ble_server_set(srv, 0, &pct, 1));    /* a stale level is all a failure costs */
        }
    }
    T5_IGNORE(t5_ble_adv_stop(adv));
    T5_IGNORE(t5_ble_server_close(srv));
    if (act) T5_IGNORE(t5_shell_activity_end(act));
    s.serving = false;
    status("Peripheral off", NULL);
}

static void worker(void *arg)
{
    (void)arg;
    while (t5_app_running(s.app)) {
        job_t j = s.job;
        if (j == JOB_NONE) {
            t5_delay_ms(100);
            continue;
        }
        if (j == JOB_SCAN) do_scan();
        else if (j == JOB_PROBE) do_probe();
        else do_serve();
        s.job = JOB_NONE;                 /* taps set it only while it is NONE */
    }
}

static void on_create(t5_app_t *app)
{
    s.app = app;
    s.mx = t5_mutex_create();
    lv_obj_t *scr = t5_app_screen(app);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    /* No title bar: the status line and the two actions share one row. */
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, T5_DP(8), 0);
    s.status = md_label_create(row, t5_ble_ready() ? "Tap Scan to look for devices." : "Bluetooth is off: turn it on in Settings.",
                               MD_TYPE_BODY2);
    lv_obj_set_style_pad_all(s.status, T5_DP(8), 0);
    lv_obj_set_flex_grow(s.status, 1);
    lv_label_set_long_mode(s.status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_add_event_cb(md_button_create(row, "Scan", MD_BUTTON_TEXT), scan_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(md_button_create(row, "Serve", MD_BUTTON_TEXT), serve_clicked, LV_EVENT_CLICKED, NULL);
    s.list = md_list_create(scr, 0, 72, bind, clicked, NULL);
    lv_obj_set_width(s.list, LV_PCT(100));
    lv_obj_set_flex_grow(s.list, 1);
    if (t5_task_create("ble.worker", worker, NULL, 6144, 2, T5_CORE_ANY, &s.worker) != T5_OK) {
        lv_label_set_text(s.status, "Out of memory");
    }
}

T5_APP(.on_create = on_create);
