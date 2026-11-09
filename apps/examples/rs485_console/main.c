/* RS-485 Console (plan 9.5): a terminal for the RS-485 port.
 *
 * Pick a baud rate and open the port; everything received shows as hex and
 * ASCII, and the text field sends a line (with CR LF). Modbus: "read SLAVE
 * ADDR COUNT" in the same field reads holding registers with Modbus RTU
 * (t5/port.h), "write SLAVE ADDR VALUE" writes one.
 *
 * Reception runs on an app task that formats lines and posts them to the UI
 * task; the port is closed (and the task stopped) when the app exits, by the
 * OS if not by us.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <t5/app.h>
#include <t5/port.h>
#include <t5/shell.h>
#include <t5/sys.h>
#include <t5/ui.h>

#define LOG_MAX 12000

typedef struct {
    t5_app_t *app;
    t5_port_t *port;
    t5_modbus_t mb;
    t5_mutex_t lock;            /* port use: the reader and Modbus */
    lv_obj_t *log, *field, *open_btn, *baud_btn;
    char *text;
    size_t len;
    int baud_i;
    volatile bool reading;
    bool reader_up;             /* started and not yet joined */
    SemaphoreHandle_t reader_done;
    t5_task_t task;
} con_t;

static const uint32_t s_bauds[] = { 9600, 19200, 38400, 57600, 115200 };
#define NBAUD (sizeof s_bauds / sizeof s_bauds[0])

static void log_add(con_t *c, const char *s)
{
    size_t n = strlen(s);
    if (c->len + n + 1 > LOG_MAX) {           /* drop the oldest half */
        size_t cut = c->len / 2;
        while (cut < c->len && c->text[cut] != '\n') cut++;
        memmove(c->text, c->text + cut, c->len - cut);
        c->len -= cut;
    }
    memcpy(c->text + c->len, s, n + 1);
    c->len += n;
    lv_label_set_text(c->log, c->text);
    lv_obj_t *box = lv_obj_get_parent(c->log);
    lv_obj_update_layout(box);
    lv_obj_scroll_to_y(box, LV_COORD_MAX, LV_ANIM_OFF);
}

typedef struct {
    con_t *c;
    char line[160];
} post_t;

static void posted(void *arg)
{
    post_t *p = arg;
    log_add(p->c, p->line);
    free(p);
}

static void post_line(con_t *c, const char *line)
{
    post_t *p = malloc(sizeof *p);
    if (!p) return;
    p->c = c;
    snprintf(p->line, sizeof p->line, "%s", line);
    if (t5_ui_async(posted, p) != T5_OK) free(p);
}

static void reader(void *arg)
{
    con_t *c = arg;
    uint8_t buf[16];
    while (c->reading && t5_app_running(c->app)) {
        t5_mutex_lock(c->lock);
        int n = c->port ? t5_port_uart_read(c->port, buf, sizeof buf, 50) : 0;
        t5_mutex_unlock(c->lock);
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        char line[160];
        size_t k = (size_t)snprintf(line, sizeof line, "< ");
        for (int i = 0; i < n; i++) k += (size_t)snprintf(line + k, sizeof line - k, "%02X ", buf[i]);
        k += (size_t)snprintf(line + k, sizeof line - k, "%*s", (int)(16 - n) * 3, "");
        for (int i = 0; i < n && k < sizeof line - 2; i++) line[k++] = buf[i] >= 32 && buf[i] < 127 ? (char)buf[i] : '.';
        line[k++] = '\n';
        line[k] = 0;
        post_line(c, line);
    }
    xSemaphoreGive(c->reader_done);
}

static void close_port(con_t *c)
{
    c->reading = false;
    if (c->reader_up) {
        /* One pass of its loop: a 50 ms read, plus waiting for a Modbus exchange to finish. */
        xSemaphoreTake(c->reader_done, pdMS_TO_TICKS(2000));
        c->reader_up = false;
    }
    t5_mutex_lock(c->lock);
    if (c->port) T5_IGNORE(t5_port_close(c->port));   /* closing on exit */
    c->port = NULL;
    t5_mutex_unlock(c->lock);
}

static void open_clicked(lv_event_t *e)
{
    con_t *c = lv_event_get_user_data(e);
    if (c->port) {
        close_port(c);
        lv_label_set_text(lv_obj_get_child(c->open_btn, 0), "Open");
        log_add(c, "-- closed\n");
        return;
    }
    t5_port_uart_cfg_t cfg = { .baud = s_bauds[c->baud_i], .data_bits = 8, .stop_bits = 1, .rx_buf = 2048 };
    char why[T5_PORT_WHY_LEN] = "";
    t5_err_t err = t5_port_open_ex("rs485", T5_MODE_RS485, &cfg, 0, &c->port, why, sizeof why);
    char msg[120];
    if (err != T5_OK) {
        snprintf(msg, sizeof msg, "-- can't open rs485: %s\n", why[0] ? why : t5_err_str(err));
        log_add(c, msg);
        return;
    }
    t5_modbus_init(&c->mb, c->port);
    snprintf(msg, sizeof msg, "-- open at %u baud, 8N1\n", (unsigned)cfg.baud);
    log_add(c, msg);
    lv_label_set_text(lv_obj_get_child(c->open_btn, 0), "Close");
    c->reading = true;
    c->reader_up = t5_task_create("rs485_rx", reader, c, 4096, 4, T5_CORE_ANY, &c->task) == T5_OK;
}

static void baud_clicked(lv_event_t *e)
{
    con_t *c = lv_event_get_user_data(e);
    if (c->port) {
        t5_shell_toast("Close the port to change the speed");
        return;
    }
    c->baud_i = (c->baud_i + 1) % (int)NBAUD;
    lv_label_set_text_fmt(lv_obj_get_child(c->baud_btn, 0), "%u baud", (unsigned)s_bauds[c->baud_i]);
}

static void modbus(con_t *c, const char *cmd)
{
    unsigned slave = 0, addr = 0, v = 0;
    char out[160];
    t5_mutex_lock(c->lock);
    if (sscanf(cmd, "read %u %u %u", &slave, &addr, &v) == 3 && v >= 1 && v <= 16) {
        uint16_t regs[16];
        t5_err_t err = t5_modbus_read_holding(&c->mb, (uint8_t)slave, (uint16_t)addr, (uint16_t)v, regs);
        size_t k = (size_t)snprintf(out, sizeof out, "modbus %u: ", slave);
        if (err == T5_OK) {
            for (unsigned i = 0; i < v; i++) k += (size_t)snprintf(out + k, sizeof out - k, "%u=%u ", addr + i, regs[i]);
        } else {
            snprintf(out + k, sizeof out - k, "%s (exception %u)", t5_err_str(err), c->mb.exception);
        }
    } else if (sscanf(cmd, "write %u %u %u", &slave, &addr, &v) == 3) {
        t5_err_t err = t5_modbus_write_register(&c->mb, (uint8_t)slave, (uint16_t)addr, (uint16_t)v);
        snprintf(out, sizeof out, "modbus %u: write %u=%u %s", slave, addr, v, t5_err_str(err));
    } else {
        snprintf(out, sizeof out, "-- read SLAVE ADDR COUNT (1-16) | write SLAVE ADDR VALUE");
    }
    t5_mutex_unlock(c->lock);
    strncat(out, "\n", sizeof out - strlen(out) - 1);
    log_add(c, out);
}

static void send_ready(lv_event_t *e)
{
    con_t *c = lv_event_get_user_data(e);
    const char *t = md_textfield_text(c->field);
    if (!t[0]) return;
    if (!c->port) {
        log_add(c, "-- open the port first\n");
        return;
    }
    if (!strncmp(t, "read ", 5) || !strncmp(t, "write ", 6)) {
        modbus(c, t);
    } else {
        char line[160];
        snprintf(line, sizeof line, "%s\r\n", t);
        t5_mutex_lock(c->lock);
        int n = t5_port_uart_write(c->port, line, strlen(line));
        if (n >= 0 && t5_port_uart_wait_tx(c->port, 500) != T5_OK) n = -1;
        t5_mutex_unlock(c->lock);
        snprintf(line, sizeof line, "> %s%s\n", t, n < 0 ? "  (send failed)" : "");
        log_add(c, line);
    }
    md_textfield_set_text(c->field, "");
}

static void back(lv_event_t *e)
{
    (void)e;
    t5_app_exit(t5_app_self());
}

static void on_create(t5_app_t *app)
{
    con_t *c = calloc(1, sizeof *c);
    t5_app_set_user(app, c);
    c->app = app;
    c->text = calloc(1, LOG_MAX);
    c->lock = t5_mutex_create();
    c->reader_done = xSemaphoreCreateBinary();
    c->baud_i = 4;
    lv_obj_t *bar;
    lv_obj_t *page = md_page_create(t5_app_screen(app), "RS-485 Console", back, NULL, &bar);
    lv_obj_t *b = md_page_body(page);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(b, T5_DP(8), 0);
    lv_obj_set_style_pad_row(b, T5_DP(8), 0);
    lv_obj_set_scrollable(b, false);
    lv_obj_t *row = lv_obj_create(b);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, T5_DP(8), 0);
    c->baud_btn = md_button_create(row, "115200 baud", MD_BUTTON_OUTLINED);
    lv_obj_add_event_cb(c->baud_btn, baud_clicked, LV_EVENT_CLICKED, c);
    c->open_btn = md_button_create(row, "Open", MD_BUTTON_CONTAINED);
    lv_obj_add_event_cb(c->open_btn, open_clicked, LV_EVENT_CLICKED, c);
    lv_obj_t *box = lv_obj_create(b);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_flex_grow(box, 1);
    lv_obj_set_style_bg_color(box, md_color(md_scheme()->c.background), 0);
    lv_obj_set_style_border_width(box, 0, 0);
    c->log = lv_label_create(box);
    lv_obj_set_width(c->log, LV_PCT(100));
    lv_label_set_long_mode(c->log, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(c->log, md_font_mono(), 0);
    lv_obj_set_style_text_color(c->log, md_color(md_scheme()->c.on_surface), 0);
    lv_label_set_text(c->log, c->text);
    c->field = md_textfield_create(b, "Text to send, or read/write SLAVE ADDR N", false);
    lv_obj_set_width(c->field, LV_PCT(100));
    lv_obj_add_event_cb(md_textfield_ta(c->field), send_ready, LV_EVENT_READY, c);
    log_add(c, "-- RS-485 on the HC1.25 connector. Pick a speed and open the port.\n");
}

static void on_destroy(t5_app_t *app)
{
    con_t *c = t5_app_get_user(app);
    if (!c) return;
    close_port(c);
    t5_mutex_delete(c->lock);
    vSemaphoreDelete(c->reader_done);
    free(c->text);
    free(c);
}

T5_APP(.on_create = on_create, .on_destroy = on_destroy);
