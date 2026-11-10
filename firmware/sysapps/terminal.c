/* SPDX-License-Identifier: Apache-2.0
 * Terminal (plan 8.4, design "Shell and system apps"): a Python REPL, the
 * device log, and a summary of the services.
 *
 * The REPL is a MicroPython VM fed a line at a time from the input field
 * (the on-screen keyboard, or a USB keyboard on the USB-A port: Up and Down
 * walk the history, Enter runs the line). Its output, the echo of each line
 * and its prompts land in the console above. Interrupt raises
 * KeyboardInterrupt; Ctrl-D (or Restart) starts a fresh VM. The REPL runs
 * with the Terminal's permissions: ports, net, usb and storage. Anything it
 * builds with t5.ui.screen() covers the Terminal until its back arrow.
 */
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "t5/power.h"
#include "t5/storage.h"
#include "t5/usb.h"
#include "t5/sys.h"
#include "t5/system.h"
#include "t5_core/logring.h"
#include "t5_py/py.h"
#include "sysapps_priv.h"

#define HIST 32
#define INPUT_MAX 256

typedef struct {
    t5_app_t *app;
    lv_obj_t *con, *input, *log, *sys;
    t5_py_t *vm;
    lv_timer_t *timer;
    uint64_t log_pos;
    char *hist[HIST];
    int nhist, hpos;
    volatile bool restart;
} term_t;

static term_t *s_t;

static void repl_start(term_t *t);

static void repl_done(t5_py_t *vm, t5_py_state_t how, void *arg)
{
    (void)vm;
    (void)how;
    term_t *t = arg;
    t->restart = true;      /* the timer restarts it on the UI task */
}

static void repl_out(const char *s, size_t n, void *arg);

static void repl_start(term_t *t)
{
    if (t->vm) t5_py_stop(t->vm, 1000);
    t->vm = NULL;
    t5_py_cfg_t cfg = {
        .repl = true,
        .ui_parent = t5_app_screen(t->app),
        .cwd = T5_USER_DIR,
        .out = repl_out,
        .done = repl_done,
        .arg = t,
    };
    t5_err_t e = t5_py_start(&cfg, &t->vm);
    if (e != T5_OK) {
        char msg[96];
        snprintf(msg, sizeof msg, "Couldn't start Python: %s. Tap Restart to try again.\n", t5_err_str(e));
        t5_py_console_write(t->con, msg, strlen(msg));
    }
}

static void hist_add(term_t *t, const char *line)
{
    if (!line[0]) return;
    if (t->nhist && !strcmp(t->hist[t->nhist - 1], line)) return;
    if (t->nhist == HIST) {
        free(t->hist[0]);
        memmove(t->hist, t->hist + 1, sizeof(char *) * (HIST - 1));
        t->nhist--;
    }
    t->hist[t->nhist++] = strdup(line);
}

/* Indentation for the next line: one level more after a ':', the same
 * inside a block, none after an empty line (which ends the block). */
static int next_indent(const char *line)
{
    int ind = 0;
    while (line[ind] == ' ') ind++;
    int e = (int)strlen(line);
    while (e > 0 && line[e - 1] == ' ') e--;
    if (e == 0) return 0;
    if (line[e - 1] == ':') return ind + 4;
    return ind;
}

static void send_line(term_t *t)
{
    const char *line = lv_textarea_get_text(t->input);
    char buf[INPUT_MAX + 2];
    /* A line of only indentation is an empty line: it ends the block. */
    const char *p = line;
    while (*p == ' ') p++;
    snprintf(buf, sizeof buf, "%s\r", *p ? line : "");
    hist_add(t, *p ? line : "");
    t->hpos = t->nhist;
    int ind = *p ? next_indent(line) : 0;
    if (t->vm) t5_py_input(t->vm, buf, strlen(buf));
    char pre[INPUT_MAX];
    memset(pre, ' ', (size_t)ind);
    pre[ind] = 0;
    lv_textarea_set_text(t->input, pre);
}

static void input_ready(lv_event_t *e)
{
    send_line(lv_event_get_user_data(e));
}

static void input_key(lv_event_t *e)
{
    term_t *t = lv_event_get_user_data(e);
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        if (!t->nhist) return;
        if (key == LV_KEY_UP && t->hpos > 0) t->hpos--;
        else if (key == LV_KEY_DOWN && t->hpos < t->nhist) t->hpos++;
        lv_textarea_set_text(t->input, t->hpos < t->nhist ? t->hist[t->hpos] : "");
        lv_event_stop_processing(e);
    } else if (key == LV_KEY_ENTER) {
        send_line(t);
        lv_event_stop_processing(e);
    }
}

static void interrupt_clicked(lv_event_t *e)
{
    term_t *t = lv_event_get_user_data(e);
    if (t->vm) t5_py_interrupt(t->vm);
}

static void restart_clicked(lv_event_t *e)
{
    term_t *t = lv_event_get_user_data(e);
    t5_py_console_clear(t->con);
    repl_start(t);
}

static void back_clicked(lv_event_t *e)
{
    term_t *t = lv_event_get_user_data(e);
    t5_app_exit(t->app);
}

/* The REPL's output goes to the console. */
static void repl_out(const char *s, size_t n, void *arg)
{
    term_t *t = arg;
    t5_py_console_write(t->con, s, n);
}

/* ---- the log and system tabs -------------------------------------------------------------- */
static void sys_refresh(term_t *t)
{
    char buf[1024];
    size_t n = 0;
#define P(...) n += (size_t)snprintf(buf + n, n < sizeof buf ? sizeof buf - n : 0, __VA_ARGS__)
    int64_t up = esp_timer_get_time() / 1000000;
    P("Uptime        %lld:%02lld:%02lld\n", up / 3600, up / 60 % 60, up % 60);
    P("Memory        internal %u KB free (min %u KB), PSRAM %u KB free\n",
      (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
      (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
      (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    P("Tasks         %u\n", (unsigned)uxTaskGetNumberOfTasks());
    P("Power         %s\n", t5_power_state_name(t5_power_state()));
    t5_battery_status_t b;
    t5_battery_status(&b);
    P("Battery       %d%%, %u mV, %d mA, %s\n", b.percent, b.mv, (int)(b.ma10 / 10), b.usb ? "on USB" : "on battery");
    t5_net_status_t ns;
    t5_net_status(&ns);
    P("Wi-Fi         %s %s, %u.%u.%u.%u, signal %d dBm\n", t5_net_state_name(ns.state), ns.ssid, ns.ip[0], ns.ip[1],
      ns.ip[2], ns.ip[3], ns.rssi);
    P("Clock         %s%s\n", t5_time_valid() ? "set" : "not set", ns.sntp_synced ? ", from the network" : "");
    P("Python        %s\n", t5_py_busy() ? "running" : "idle");
    P("SD card       %s\n", t5_storage_sd_state_str(t5_storage_sd_state()));
    P("USB devices   %d%s\n", t5_usb_device_count(), t5_usb_keyboard_present() ? " (keyboard)" : "");
#undef P
    sa_set_text(t->sys, buf);
}

static void tick(lv_timer_t *tm)
{
    term_t *t = lv_timer_get_user_data(tm);
    if (t->restart) {
        t->restart = false;
        repl_start(t);
    }
    char buf[1024];
    size_t n;
    int rounds = 0;
    while (rounds++ < 8 && (n = t5_log_read(&t->log_pos, buf, sizeof buf)) > 0) t5_py_console_write(t->log, buf, n);
    static uint32_t last;
    if (lv_tick_elaps(last) >= 1000) {
        last = lv_tick_get();
        sys_refresh(t);
    }
}

static void on_create(t5_app_t *app)
{
    term_t *t = calloc(1, sizeof *t);
    if (!t) return;
    s_t = t;
    t->app = app;
    lv_obj_t *scr = t5_app_screen(app);
    lv_obj_t *bar;
    lv_obj_t *page = md_page_create(scr, "Terminal", back_clicked, t, &bar);
    md_appbar_add_action(bar, MD_ICON_REFRESH, restart_clicked, t);
    md_appbar_add_action(bar, MD_ICON_STOP, interrupt_clicked, t);
    lv_obj_t *body = md_page_body(page);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_scrollable(body, false);
    lv_obj_t *tabs = md_tabs_create(body);
    lv_obj_set_size(tabs, LV_PCT(100), LV_PCT(100));

    lv_obj_t *py = md_tabs_add(tabs, "Python");
    lv_obj_set_style_pad_all(py, 0, 0);
    lv_obj_set_flex_flow(py, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(py, false);
    t->con = t5_py_console_create(py);
    lv_obj_set_height(t->con, 1);   /* px: flex-grow sets it */
    lv_obj_set_flex_grow(t->con, 1);
    t->input = lv_textarea_create(py);
    lv_textarea_set_one_line(t->input, true);
    lv_textarea_set_placeholder_text(t->input, "Type Python, then press Enter");
    lv_obj_set_width(t->input, LV_PCT(100));
    lv_obj_set_style_text_font(t->input, md_font_mono(), 0);
    /* A one-line field scrolls sideways under long lines; its scrollbar
     * showed as a stray bar at the right edge. The cursor is enough. */
    lv_obj_set_scrollbar_mode(t->input, LV_SCROLLBAR_MODE_OFF);
    /* Square top corners: the field sits flush under the console. */
    lv_obj_set_style_radius(t->input, 0, 0);
    lv_obj_add_event_cb(t->input, input_ready, LV_EVENT_READY, t);
    lv_obj_add_event_cb(t->input, input_key, LV_EVENT_KEY | LV_EVENT_PREPROCESS, t);
    lv_group_t *g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, t->input);
        lv_group_focus_obj(t->input);
    }

    lv_obj_t *lg = md_tabs_add(tabs, "Log");
    lv_obj_set_style_pad_all(lg, 0, 0);
    lv_obj_set_scrollable(lg, false);
    t->log = t5_py_console_create(lg);
    t->log_pos = t5_log_head() > 8192 ? t5_log_head() - 8192 : 0;

    lv_obj_t *sy = md_tabs_add(tabs, "System");
    lv_obj_set_style_pad_all(sy, T5_DP(16), 0);
    t->sys = lv_label_create(sy);
    lv_obj_set_width(t->sys, LV_PCT(100));
    lv_obj_set_style_text_font(t->sys, md_font_mono(), 0);
    lv_obj_set_style_text_color(t->sys, md_color(md_scheme()->c.on_surface), 0);
    lv_label_set_text(t->sys, "");
    sys_refresh(t);

    t->timer = lv_timer_create(tick, 200, t);
    repl_start(t);
}

static void on_destroy(t5_app_t *app)
{
    (void)app;
    term_t *t = s_t;
    if (!t) return;
    if (t->vm) t5_py_stop(t->vm, 1000);
    if (t->timer) lv_timer_delete(t->timer);
    for (int i = 0; i < t->nhist; i++) free(t->hist[i]);
    free(t);
    s_t = NULL;
}

static const t5_app_desc_t s_desc = { .api = T5_API_LEVEL, .on_create = on_create, .on_destroy = on_destroy };

const t5_builtin_app_t sysapp_terminal = {
    .id = T5_APP_ID_TERMINAL,
    .name = "Terminal",
    .desc = &s_desc,
    .perms = T5_PERM_PORTS | T5_PERM_NET | T5_PERM_USB | T5_PERM_STORAGE,
    .orientation = T5_ORIENT_ANY,
    .ports = sa_all_ports,
    .icon = MD_ICON_TERMINAL,
};
