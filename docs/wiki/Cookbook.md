# Cookbook

Complete apps you can copy. Each C recipe is a whole `main.c`: every one on this page was compiled against the API level 1 SDK and its imports checked against [`sdk/abi/api_v1.sym`](../../sdk/abi/api_v1.sym). Replace the template's `main.c` with one, add the manifest lines shown, and `t5 run`.

| Recipe | Shows |
| --- | --- |
| [Background work with a UI result](#background-work-with-a-ui-result) | The worker pattern, coalescing, stale results |
| [Sensor dashboard](#sensor-dashboard) | Polling a port on a task, latest-value hand-off |
| [Long list](#long-list) | `md_list`: 10,000 rows with about 20 objects |
| [Settings that survive](#settings-that-survive) | Versioned struct, atomic write, debounce |
| [Session restore](#session-restore) | `on_suspend` state and `t5_app_restored` |
| [Scheduled background job](#scheduled-background-job) | `on_alarm` with no UI |
| [Open a file the person chooses](#open-a-file-the-person-chooses) | The system file picker, reading a folder |
| [Streaming download to the SD card](#streaming-download-to-the-sd-card) | A shared folder, progress, temp file |
| [Reconnecting WebSocket](#reconnecting-websocket) | Backoff, wake lock, clean exit |
| [Fast pixel drawing](#fast-pixel-drawing) | A canvas with dirty-rectangle invalidation |
| [Audio synth](#audio-synth) | Streaming PCM from a task |
| [Camera with permission](#camera-with-permission) | The prompt, the preview, the suspend pair |
| [Modbus poller](#modbus-poller) | RS-485 and error handling |
| [Python dashboard](#python-dashboard) | The same dashboard in MicroPython |

## Background work with a UI result

Anything that can take more than a few milliseconds runs on a task, and its result goes back to the UI task with `t5_ui_async`. The generation number discards a result nobody wants any more. See [Threading](Threading.md).

Manifest: `net = true` in `[permissions]`.

<!-- compile: worker -->
```c
#include <stdio.h>
#include <stdlib.h>
#include <t5/app.h>
#include <t5/net.h>
#include <t5/shell.h>
#include <t5/sys.h>
#include <t5/ui.h>

#define URL "https://example.com/"

typedef struct {
    t5_app_t *app;
    lv_obj_t *status, *button;
    uint32_t generation;        /* UI task only: bumped to abandon a request */
    bool busy;                  /* UI task only */
} state_t;

typedef struct {                /* one per request; owned by whoever holds it */
    state_t *s;
    uint32_t generation;
    int http_status;
    int len;                    /* negative: a t5_err_t */
} job_t;

/* Runs on the UI task. */
static void job_done(void *arg)
{
    job_t *j = arg;
    state_t *s = j->s;
    if (j->generation == s->generation) {       /* still the request we care about */
        s->busy = false;
        lv_obj_remove_state(s->button, LV_STATE_DISABLED);
        if (j->len < 0) lv_label_set_text_fmt(s->status, "Failed: %s", t5_err_str((t5_err_t)j->len));
        else lv_label_set_text_fmt(s->status, "HTTP %d, %d bytes", j->http_status, j->len);
    }
    free(j);
}

/* Runs on its own task: no LVGL here. */
static void fetch_task(void *arg)
{
    job_t *j = arg;
    char *buf = malloc(64 * 1024);
    if (!buf) {
        j->len = T5_ERR_NO_MEM;
    } else {
        t5_http_req_t req = { .url = URL };
        j->len = t5_http_fetch(&req, buf, 64 * 1024, &j->http_status);
        free(buf);
    }
    if (t5_ui_async(job_done, j) != T5_OK) free(j);     /* queue full: drop the result */
}

static void start(lv_event_t *e)
{
    state_t *s = lv_event_get_user_data(e);
    if (s->busy) return;
    job_t *j = calloc(1, sizeof *j);
    if (!j) return;
    j->s = s;
    j->generation = ++s->generation;
    /* TLS needs a big stack: 12 KB. Priorities above 2 are capped to 2. */
    if (t5_task_create("fetch", fetch_task, j, 12 * 1024, 2, T5_CORE_ANY, NULL) != T5_OK) {
        free(j);
        T5_IGNORE(t5_shell_toast("Couldn't start the request"));    /* best effort */
        return;
    }
    s->busy = true;
    lv_obj_add_state(s->button, LV_STATE_DISABLED);
    lv_label_set_text(s->status, "Fetching...");
}

static void back(lv_event_t *e)
{
    (void)e;
    t5_app_exit(t5_app_self());
}

static void on_create(t5_app_t *app)
{
    state_t *s = calloc(1, sizeof *s);
    if (!s) return;
    s->app = app;
    t5_app_set_user(app, s);
    lv_obj_t *page = md_page_create(t5_app_screen(app), "Fetch", back, NULL, NULL);
    lv_obj_t *body = md_page_body(page);
    lv_obj_set_style_pad_all(body, T5_DP(16), 0);
    lv_obj_set_style_pad_row(body, T5_DP(12), 0);
    s->status = md_label_create(body, "Tap to fetch", MD_TYPE_BODY1);
    s->button = md_button_create(body, "Fetch", MD_BUTTON_CONTAINED);
    lv_obj_add_event_cb(s->button, start, LV_EVENT_CLICKED, s);
}

/* No on_destroy that frees s: a fetch may still be running. The OS frees it
 * with the rest of the app's heap after the task is gone, and doesn't run
 * job_done once the app has exited. */
T5_APP(.on_create = on_create);
```

## Sensor dashboard

A task reads the sensor once a second and publishes the newest value. Only one UI update is ever queued, so a slow UI can't build a backlog. The Grove 5 V rail is held while the app runs.

Manifest: `ports = ["grove"]` in `[permissions]`.

<!-- compile: dashboard -->
```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <t5/app.h>
#include <t5/port.h>
#include <t5/sys.h>
#include <t5/ui.h>

#define SHT_ADDR 0x44

typedef struct {
    t5_app_t *app;
    lv_obj_t *temp, *state;
    t5_mutex_t lock;
    float celsius;              /* guarded by lock */
    t5_err_t error;             /* guarded by lock */
    volatile bool ui_queued;    /* one pending update at most */
} state_t;

static void show(void *arg)
{
    state_t *s = arg;
    s->ui_queued = false;
    t5_mutex_lock(s->lock);
    float c = s->celsius;
    t5_err_t e = s->error;
    t5_mutex_unlock(s->lock);
    if (e != T5_OK) {
        lv_label_set_text(s->temp, "--");
        lv_label_set_text_fmt(s->state, "Sensor: %s", t5_err_str(e));
        return;
    }
    char text[16];
    snprintf(text, sizeof text, "%.1f °C", (double)c);
    lv_label_set_text(s->temp, text);
    lv_label_set_text(s->state, "Sensor OK");
}

static t5_err_t read_sht4x(t5_port_t *p, float *c)
{
    static const uint8_t cmd = 0xFD;
    uint8_t rx[6];
    T5_RETURN_ON_ERR(t5_port_i2c_xfer(p, SHT_ADDR, &cmd, 1, NULL, 0));
    t5_delay_ms(20);
    T5_RETURN_ON_ERR(t5_port_i2c_xfer(p, SHT_ADDR, NULL, 0, rx, sizeof rx));
    *c = -45.0f + 175.0f * (float)((rx[0] << 8) | rx[1]) / 65535.0f;
    return T5_OK;
}

static void sensor_task(void *arg)
{
    state_t *s = arg;
    t5_port_t *port = NULL;
    t5_rail_hold_t *rail = NULL;
    t5_err_t e = t5_rail_acquire(T5_RAIL_EXT5V, "dash", 0, &rail);
    if (e == T5_OK) {
        t5_port_i2c_cfg_t cfg = { .hz = 100000 };
        e = t5_port_open("grove", T5_MODE_I2C, &cfg, &port);
    }
    t5_delay_ms(50);                            /* let the module power up */
    while (t5_app_running(s->app)) {
        float c = 0;
        t5_err_t r = port ? read_sht4x(port, &c) : e;
        t5_mutex_lock(s->lock);
        s->celsius = c;
        s->error = r;
        t5_mutex_unlock(s->lock);
        if (!s->ui_queued) {
            s->ui_queued = true;
            if (t5_ui_async(show, s) != T5_OK) s->ui_queued = false;
        }
        t5_delay_ms(1000);
    }
    /* The port and the rail are released by the OS if this doesn't get to run. */
    if (port) T5_IGNORE(t5_port_close(port));
    if (rail) T5_IGNORE(t5_rail_release(rail));
}

static void back(lv_event_t *e)
{
    (void)e;
    t5_app_exit(t5_app_self());
}

static void on_create(t5_app_t *app)
{
    state_t *s = calloc(1, sizeof *s);
    if (!s) return;
    s->app = app;
    s->lock = t5_mutex_create();
    lv_obj_t *page = md_page_create(t5_app_screen(app), "Room", back, NULL, NULL);
    lv_obj_t *body = md_page_body(page);
    lv_obj_set_style_pad_all(body, T5_DP(16), 0);
    s->temp = md_label_create(body, "--", MD_TYPE_HEADLINE4);
    s->state = md_label_create(body, "Starting...", MD_TYPE_BODY2);
    if (t5_task_create("sensor", sensor_task, s, 4096, 1, T5_CORE_ANY, NULL) != T5_OK) {
        lv_label_set_text(s->state, "Couldn't start the sensor task");
    }
}

T5_APP(.on_create = on_create);
```

## Long list

Use `md_list` for anything longer than a screen: it keeps only the visible rows alive and rebinds them as you scroll. `bind` runs during scrolling, so it must be quick and must not do I/O.

<!-- compile: longlist -->
```c
#include <stdio.h>
#include <t5/app.h>
#include <t5/ui.h>

#define COUNT 10000

static void bind(md_list_row_t *row, uint32_t index, void *user)
{
    (void)user;
    char text[32];
    snprintf(text, sizeof text, "Row %u", (unsigned)index);
    lv_label_set_text(row->primary, text);
    if (row->secondary) {
        snprintf(text, sizeof text, "Secondary line %u", (unsigned)(index * 7));
        lv_label_set_text(row->secondary, text);
    }
}

static void clicked(uint32_t index, void *user)
{
    (void)user;
    char text[32];
    snprintf(text, sizeof text, "Tapped row %u", (unsigned)index);
    md_snackbar_show(text, NULL, NULL, NULL);
}

static void back(lv_event_t *e)
{
    (void)e;
    t5_app_exit(t5_app_self());
}

static void on_create(t5_app_t *app)
{
    lv_obj_t *scr = t5_app_screen(app);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(scr, 0, 0);
    md_appbar_create(scr, "Long list", MD_ICON_ARROW_BACK, back, NULL);
    lv_obj_t *list = md_list_create(scr, COUNT, MD_LIST_ROW_2_DP, bind, clicked, NULL);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
}

T5_APP(.on_create = on_create);
```

## Settings that survive

Settings are a versioned struct written with `t5_storage_write_atomic`, delayed by a second after the last change so dragging a slider doesn't wear the flash. The write is also forced in `on_suspend`. The power button can cut power with no warning, so don't rely on `on_suspend` alone.

<!-- compile: settings -->
```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <t5/app.h>
#include <t5/storage.h>
#include <t5/ui.h>

#define SETTINGS_VERSION 2

typedef struct {
    uint32_t version;
    int32_t volume;
    bool loud_mode;
} settings_t;

typedef struct {
    t5_app_t *app;
    settings_t cfg;
    lv_timer_t *save_timer;
    bool dirty;
} state_t;

static void settings_path(t5_app_t *app, char *out, size_t n)
{
    snprintf(out, n, "%s/settings.bin", t5_app_data_dir(app));
}

static void defaults(settings_t *c)
{
    *c = (settings_t){ .version = SETTINGS_VERSION, .volume = 50, .loud_mode = false };
}

static void load(t5_app_t *app, settings_t *c)
{
    char path[200];
    settings_path(app, path, sizeof path);
    defaults(c);
    FILE *f = fopen(path, "r");
    if (!f) return;
    settings_t got;
    size_t n = fread(&got, 1, sizeof got, f);
    fclose(f);
    if (n == sizeof got && got.version == SETTINGS_VERSION) *c = got;
    /* An older version: keep the defaults, or migrate field by field here. */
}

static void save_now(state_t *s)
{
    if (!s->dirty) return;
    char path[200];
    settings_path(s->app, path, sizeof path);
    t5_err_t e = t5_storage_write_atomic(path, &s->cfg, sizeof s->cfg);
    if (e == T5_OK) s->dirty = false;
    else md_snackbar_show("Couldn't save the settings", NULL, NULL, NULL);
}

static void save_timer_cb(lv_timer_t *t)
{
    save_now(lv_timer_get_user_data(t));
    lv_timer_pause(t);
}

static void changed(state_t *s)
{
    s->dirty = true;
    lv_timer_reset(s->save_timer);          /* one second after the last change */
    lv_timer_resume(s->save_timer);
}

static void on_slider(lv_event_t *e)
{
    state_t *s = lv_event_get_user_data(e);
    s->cfg.volume = lv_slider_get_value(lv_event_get_target_obj(e));
    changed(s);
}

static void on_switch(lv_event_t *e)
{
    state_t *s = lv_event_get_user_data(e);
    s->cfg.loud_mode = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    changed(s);
}

static void back(lv_event_t *e)
{
    (void)e;
    t5_app_exit(t5_app_self());
}

static void on_create(t5_app_t *app)
{
    state_t *s = calloc(1, sizeof *s);
    if (!s) return;
    s->app = app;
    t5_app_set_user(app, s);
    load(app, &s->cfg);
    s->save_timer = lv_timer_create(save_timer_cb, 1000, s);
    lv_timer_pause(s->save_timer);

    lv_obj_t *page = md_page_create(t5_app_screen(app), "Settings", back, NULL, NULL);
    lv_obj_t *body = md_page_body(page);
    md_section_create(body, "Sound");
    lv_obj_t *row = md_row_create(body, MD_ICON_VOLUME_UP, "Loud mode", "Louder than sensible");
    lv_obj_t *sw = md_row_switch(row, s->cfg.loud_mode);
    lv_obj_add_event_cb(sw, on_switch, LV_EVENT_VALUE_CHANGED, s);
    lv_obj_t *slider = md_slider_create(body, 0, 100, s->cfg.volume);
    lv_obj_set_width(slider, LV_PCT(90));
    lv_obj_add_event_cb(slider, on_slider, LV_EVENT_VALUE_CHANGED, s);
}

static void on_suspend(t5_app_t *app)
{
    state_t *s = t5_app_get_user(app);
    if (s) save_now(s);
}

T5_APP(.on_create = on_create, .on_suspend = on_suspend);
```

## Session restore

After a soft off the OS reopens the app that was in front. Save the position in `on_suspend` and put it back in `on_create`. The power button can cut power with no warning, so for anything that matters, also save when it changes.

<!-- compile: restore -->
```c
#include <stdlib.h>
#include <t5/app.h>
#include <t5/ui.h>

typedef struct {
    int32_t scroll_y;
} saved_t;

typedef struct {
    lv_obj_t *body;
} state_t;

static void back(lv_event_t *e)
{
    (void)e;
    t5_app_exit(t5_app_self());
}

static void on_create(t5_app_t *app)
{
    state_t *s = calloc(1, sizeof *s);
    if (!s) return;
    t5_app_set_user(app, s);
    lv_obj_t *page = md_page_create(t5_app_screen(app), "Notes", back, NULL, NULL);
    s->body = md_page_body(page);
    for (int i = 0; i < 40; i++) {
        lv_obj_t *l = md_label_create(s->body, "A line of text in a long page.", MD_TYPE_BODY1);
        lv_obj_set_style_pad_all(l, T5_DP(8), 0);
    }
    if (t5_app_restored(app)) {
        saved_t st;
        size_t len = 0;
        if (t5_app_state_load(app, &st, sizeof st, &len) == T5_OK && len == sizeof st) {
            lv_obj_update_layout(s->body);      /* positions exist only after a layout */
            lv_obj_scroll_to_y(s->body, st.scroll_y, LV_ANIM_OFF);
        }
    }
}

static void on_suspend(t5_app_t *app)
{
    state_t *s = t5_app_get_user(app);
    if (!s) return;
    saved_t st = { .scroll_y = lv_obj_get_scroll_y(s->body) };
    T5_IGNORE(t5_app_state_save(app, &st, sizeof st));      /* nothing useful to do on failure */
}

T5_APP(.on_create = on_create, .on_suspend = on_suspend);
```

## Scheduled background job

The OS boots from soft off, runs `on_alarm` with no screen, then powers down again. Read [Power and background](Power-and-Background.md) first: `on_alarm` runs only if the device was in soft off when the alarm came due, so also reschedule when the app is opened.

```toml
[permissions]
background_wake = true

[background]
entry = "on_alarm"
max_seconds = 30
```

<!-- compile: alarm -->
```c
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <t5/app.h>
#include <t5/power.h>
#include <t5/sys.h>
#include <t5/ui.h>

#define INTERVAL_S 3600

static void log_battery(t5_app_t *app)
{
    char path[200];
    snprintf(path, sizeof path, "%s/battery.csv", t5_app_data_dir(app));
    FILE *f = fopen(path, "a");
    if (!f) return;
    t5_battery_status_t b;
    t5_battery_status(&b);
    fprintf(f, "%lld,%d\n", (long long)time(NULL), (int)b.mv);
    fclose(f);
}

/* Headless: no LVGL, no t5_app_screen(). */
static void on_alarm(t5_app_t *app)
{
    /* First, so a failure below doesn't end the series. */
    T5_IGNORE(t5_app_schedule_alarm(app, time(NULL) + INTERVAL_S));     /* retried on the next open */
    log_battery(app);
}

static void back(lv_event_t *e)
{
    (void)e;
    t5_app_exit(t5_app_self());
}

static void on_create(t5_app_t *app)
{
    lv_obj_t *page = md_page_create(t5_app_screen(app), "Battery log", back, NULL, NULL);
    t5_err_t e = t5_app_schedule_alarm(app, time(NULL) + INTERVAL_S);
    md_label_create(md_page_body(page), e == T5_OK ? "Next reading in an hour" : t5_err_str(e), MD_TYPE_BODY1);
}

T5_APP(.on_create = on_create, .on_alarm = on_alarm);
```

## Open a file the person chooses

`t5_file_pick` needs no permission: choosing the file is the consent, and that one file stays open to the app until it exits. This recipe also lists its own data folder.

<!-- compile: picker -->
```c
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <t5/app.h>
#include <t5/ui.h>

typedef struct {
    t5_app_t *app;
    lv_obj_t *label;
} state_t;

/* UI task. path is NULL if the person backed out. */
static void picked(t5_app_t *app, const char *path, void *user)
{
    (void)app;
    state_t *s = user;
    if (!path) return;
    FILE *f = fopen(path, "r");
    if (!f) {
        lv_label_set_text(s->label, "Couldn't open that file");
        return;
    }
    char head[64];
    size_t n = fread(head, 1, sizeof head - 1, f);
    fclose(f);
    head[n] = 0;
    lv_label_set_text(s->label, head);
}

static void choose(lv_event_t *e)
{
    state_t *s = lv_event_get_user_data(e);
    t5_err_t err = t5_file_pick(s->app, "Open a text file", "txt,csv", 0, picked, s);
    if (err != T5_OK) md_snackbar_show(t5_err_str(err), NULL, NULL, NULL);
}

static int count_own_files(t5_app_t *app)
{
    DIR *d = opendir(t5_app_data_dir(app));
    if (!d) return -1;
    int n = 0;
    for (struct dirent *ent; (ent = readdir(d)) != NULL;) {
        if (ent->d_name[0] != '.') n++;
    }
    closedir(d);
    return n;
}

static void back(lv_event_t *e)
{
    (void)e;
    t5_app_exit(t5_app_self());
}

static void on_create(t5_app_t *app)
{
    state_t *s = calloc(1, sizeof *s);
    if (!s) return;
    s->app = app;
    lv_obj_t *page = md_page_create(t5_app_screen(app), "Reader", back, NULL, NULL);
    lv_obj_t *body = md_page_body(page);
    lv_obj_set_style_pad_all(body, T5_DP(16), 0);
    lv_obj_set_style_pad_row(body, T5_DP(12), 0);
    s->label = md_label_create(body, "", MD_TYPE_BODY1);
    lv_label_set_text_fmt(s->label, "%d files in the data folder", count_own_files(app));
    lv_obj_add_event_cb(md_button_create(body, "Open...", MD_BUTTON_CONTAINED), choose, LV_EVENT_CLICKED, s);
}

T5_APP(.on_create = on_create);
```

## Streaming download to the SD card

The body is streamed in 4 KB pieces to a temporary name, then renamed, so a partial file never looks complete. Progress goes to the UI at most five times a second. Opening a handle holds a net wake lock, so the transfer continues with the screen off. `/sd/Downloads` is a [shared folder](Storage.md#which-files-an-app-can-use). The first write asks the person, and on a worker task the call waits for their answer.

Manifest: `net = true` and `shared_write = true`.

<!-- compile: download -->
```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <t5/app.h>
#include <t5/net.h>
#include <t5/storage.h>
#include <t5/sys.h>
#include <t5/ui.h>

#define URL "https://example.com/big.bin"
#define DIR_PATH "/sd/Downloads"
#define DEST DIR_PATH "/big.bin"
#define TEMP DEST ".part"

typedef struct {
    t5_app_t *app;
    lv_obj_t *bar, *label;
    volatile int64_t got, total;
    volatile t5_err_t result;
    volatile bool done;
} state_t;

static void show(lv_timer_t *t)
{
    state_t *s = lv_timer_get_user_data(t);
    int64_t total = s->total;
    if (total > 0) lv_bar_set_value(s->bar, (int32_t)(s->got * 100 / total), LV_ANIM_OFF);
    lv_label_set_text_fmt(s->label, "%d KB", (int)(s->got / 1024));
    if (s->done) {
        lv_label_set_text(s->label, s->result == T5_OK ? "Saved" : t5_err_str(s->result));
        lv_timer_pause(t);
    }
}

static t5_err_t download(state_t *s)
{
    if (t5_storage_sd_state() != T5_SD_MOUNTED) return T5_ERR_NOT_FOUND;
    t5_http_t *h = NULL;
    t5_http_req_t req = { .url = URL };
    T5_RETURN_ON_ERR(t5_http_open(&req, &h));
    mkdir(DIR_PATH, 0755);                      /* fine if it already exists */
    t5_err_t e = T5_OK;
    FILE *f = NULL;
    char *buf = malloc(4096);
    if (t5_http_status(h) != 200) e = T5_ERR_NOT_FOUND;
    else if (!buf || !(f = fopen(TEMP, "w"))) e = T5_ERR_IO;
    s->total = t5_http_content_length(h);
    while (e == T5_OK && t5_app_running(s->app)) {
        int n = t5_http_read(h, buf, 4096, 15000);
        if (n < 0) e = (t5_err_t)n;
        if (n <= 0) break;
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) e = T5_ERR_IO;
        s->got += n;
    }
    if (f) fclose(f);
    free(buf);
    T5_IGNORE(t5_http_close(h));                /* the transfer's result is in e */
    if (e == T5_OK && t5_app_running(s->app)) {
        remove(DEST);                           /* FAT: rename won't replace a file */
        if (rename(TEMP, DEST) != 0) e = T5_ERR_IO;
    } else {
        remove(TEMP);
    }
    return e;
}

static void task(void *arg)
{
    state_t *s = arg;
    s->result = download(s);
    s->done = true;
}

static void back(lv_event_t *e)
{
    (void)e;
    t5_app_exit(t5_app_self());
}

static void on_create(t5_app_t *app)
{
    state_t *s = calloc(1, sizeof *s);
    if (!s) return;
    s->app = app;
    lv_obj_t *page = md_page_create(t5_app_screen(app), "Download", back, NULL, NULL);
    lv_obj_t *body = md_page_body(page);
    lv_obj_set_style_pad_all(body, T5_DP(16), 0);
    s->label = md_label_create(body, "Starting...", MD_TYPE_BODY1);
    s->bar = lv_bar_create(body);
    lv_obj_set_width(s->bar, LV_PCT(100));
    lv_timer_create(show, 200, s);
    if (t5_task_create("dl", task, s, 12 * 1024, 2, T5_CORE_ANY, NULL) != T5_OK) {
        s->result = T5_ERR_NO_MEM;
        s->done = true;
    }
}

T5_APP(.on_create = on_create);
```

## Reconnecting WebSocket

A network loop must expect the connection to fail and must end promptly when the app exits, so each wait is short and the backoff is sliced.

Manifest: `net = true`.

<!-- compile: websocket -->
```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <t5/app.h>
#include <t5/net.h>
#include <t5/shell.h>
#include <t5/sys.h>
#include <t5/ui.h>

#define URL "wss://echo.websocket.org/"

typedef struct {
    t5_app_t *app;
    lv_obj_t *label;
    volatile bool ui_queued;
    t5_mutex_t lock;
    char last[128];             /* guarded by lock */
} state_t;

static void show(void *arg)
{
    state_t *s = arg;
    s->ui_queued = false;
    t5_mutex_lock(s->lock);
    lv_label_set_text(s->label, s->last);
    t5_mutex_unlock(s->lock);
}

static void publish(state_t *s, const char *text)
{
    t5_mutex_lock(s->lock);
    snprintf(s->last, sizeof s->last, "%s", text);
    t5_mutex_unlock(s->lock);
    if (!s->ui_queued) {
        s->ui_queued = true;
        if (t5_ui_async(show, s) != T5_OK) s->ui_queued = false;
    }
}

/* Sleeps in slices, so exiting isn't held up by a long backoff. */
static void wait_running(state_t *s, uint32_t ms)
{
    for (uint32_t t = 0; t < ms && t5_app_running(s->app); t += 100) t5_delay_ms(100);
}

static bool online(void)
{
    t5_net_status_t st;
    t5_net_status(&st);
    return st.state == T5_NET_CONNECTED;
}

static void session(state_t *s, t5_ws_t *ws)
{
    char buf[512];
    publish(s, "Connected");
    while (t5_app_running(s->app)) {
        t5_ws_op_t op;
        int n = t5_ws_recv(ws, buf, sizeof buf - 1, &op, 500);
        if (n == T5_ERR_TIMEOUT) continue;
        if (n < 0 || op == T5_WS_CLOSE) return;
        if (op == T5_WS_TEXT) {
            buf[n] = 0;
            publish(s, buf);
        }
    }
}

static void net_task(void *arg)
{
    state_t *s = arg;
    uint32_t backoff = 1000;
    while (t5_app_running(s->app)) {
        if (!online()) {
            publish(s, "Waiting for Wi-Fi");
            wait_running(s, 2000);
            continue;
        }
        t5_ws_t *ws = NULL;
        if (t5_ws_connect(URL, NULL, &ws) == T5_OK) {
            backoff = 1000;
            session(s, ws);
            T5_IGNORE(t5_ws_close(ws, 1000));   /* the connection is over either way */
        }
        publish(s, "Disconnected, retrying");
        wait_running(s, backoff);
        backoff = backoff < 30000 ? backoff * 2 : 30000;
    }
}

static void back(lv_event_t *e)
{
    (void)e;
    t5_app_exit(t5_app_self());
}

static void on_create(t5_app_t *app)
{
    state_t *s = calloc(1, sizeof *s);
    if (!s) return;
    s->app = app;
    s->lock = t5_mutex_create();
    lv_obj_t *page = md_page_create(t5_app_screen(app), "Live", back, NULL, NULL);
    s->label = md_label_create(md_page_body(page), "Starting...", MD_TYPE_BODY1);
    if (t5_task_create("ws", net_task, s, 12 * 1024, 2, T5_CORE_ANY, NULL) != T5_OK) {
        lv_label_set_text(s->label, "Couldn't start the network task");
    }
}

T5_APP(.on_create = on_create);
```

## Fast pixel drawing

For games and visualisations, draw into your own RGB565 buffer and invalidate only what changed. Keep the changed area small: a full-screen update is limited by PSRAM bandwidth to about 27 fps (see [Performance](Performance.md)).

<!-- compile: canvas -->
```c
#include <stdlib.h>
#include <t5/app.h>
#include <t5/ui.h>

#define W T5_DP(160)
#define H T5_DP(160)
#define BALL T5_DP(10)

typedef struct {
    lv_obj_t *canvas;
    lv_draw_buf_t *buf;
    int32_t x, y, vx, vy;
} state_t;

static void fill(state_t *s, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t color)
{
    for (int32_t y = y0; y < y1; y++) {
        uint16_t *row = (uint16_t *)lv_draw_buf_goto_xy(s->buf, (uint32_t)x0, (uint32_t)y);
        for (int32_t x = x0; x < x1; x++) *row++ = color;
    }
}

/* Redraw the union of the old and new ball, and only that. */
static void step(lv_timer_t *t)
{
    state_t *s = lv_timer_get_user_data(t);
    int32_t ox = s->x, oy = s->y;
    s->x += s->vx;
    s->y += s->vy;
    if (s->x < 0 || s->x + BALL >= W) s->vx = -s->vx;
    if (s->y < 0 || s->y + BALL >= H) s->vy = -s->vy;
    if (s->x < 0) s->x = 0;
    if (s->y < 0) s->y = 0;
    if (s->x + BALL > W) s->x = W - BALL;
    if (s->y + BALL > H) s->y = H - BALL;
    fill(s, ox, oy, ox + BALL, oy + BALL, 0x0000);
    fill(s, s->x, s->y, s->x + BALL, s->y + BALL, 0xFFE0);

    lv_area_t area, screen;
    lv_area_set(&area, LV_MIN(ox, s->x), LV_MIN(oy, s->y), LV_MAX(ox, s->x) + BALL - 1, LV_MAX(oy, s->y) + BALL - 1);
    lv_obj_get_coords(s->canvas, &screen);
    lv_area_move(&area, screen.x1, screen.y1);
    lv_obj_invalidate_area(s->canvas, &area);
}

static void back(lv_event_t *e)
{
    (void)e;
    t5_app_exit(t5_app_self());
}

static void on_create(t5_app_t *app)
{
    state_t *s = calloc(1, sizeof *s);
    if (!s) return;
    s->vx = 6;
    s->vy = 4;
    lv_obj_t *scr = t5_app_screen(app);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(scr, 0, 0);
    md_appbar_create(scr, "Pixels", MD_ICON_ARROW_BACK, back, NULL);
    s->buf = lv_draw_buf_create(W, H, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);  /* tracked; freed at exit */
    if (!s->buf) return;
    lv_draw_buf_clear(s->buf, NULL);
    s->canvas = lv_canvas_create(scr);
    lv_canvas_set_draw_buf(s->canvas, s->buf);
    lv_timer_create(step, 16, s);
}

T5_APP(.on_create = on_create);
```

## Audio synth

Stream PCM from a task. `t5_audio_out_write` blocks until there's room, which paces the loop. An open stream holds an audio wake lock, so it plays with the screen off.

<!-- compile: audio -->
```c
#include <math.h>
#include <stdlib.h>
#include <t5/app.h>
#include <t5/audio.h>
#include <t5/shell.h>
#include <t5/sys.h>
#include <t5/ui.h>

#define RATE 16000
#define FRAMES 160

typedef struct {
    t5_app_t *app;
    volatile bool playing;
    volatile int hz;
} state_t;

static void synth_task(void *arg)
{
    state_t *s = arg;
    t5_audio_out_cfg_t cfg = { .rate = RATE, .channels = 1 };
    t5_audio_out_t *out = NULL;
    t5_err_t e = t5_audio_out_open(&cfg, &out);
    if (e != T5_OK) {
        T5_IGNORE(t5_shell_toast(t5_err_str(e)));       /* any task; best effort */
        return;
    }
    int16_t pcm[FRAMES];
    float phase = 0;
    while (t5_app_running(s->app)) {
        if (!s->playing) {
            t5_delay_ms(20);
            continue;
        }
        float step = 2.0f * 3.14159265f * (float)s->hz / (float)RATE;
        for (int i = 0; i < FRAMES; i++) {
            pcm[i] = (int16_t)(sinf(phase) * 8000.0f);
            phase += step;
            if (phase > 6.28318531f) phase -= 6.28318531f;
        }
        if (t5_audio_out_write(out, pcm, sizeof pcm, 200) < 0) t5_delay_ms(20);
    }
    T5_IGNORE(t5_audio_out_close(out));                 /* closed at exit anyway */
}

static void toggled(lv_event_t *e)
{
    state_t *s = lv_event_get_user_data(e);
    s->playing = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
}

static void pitch(lv_event_t *e)
{
    state_t *s = lv_event_get_user_data(e);
    s->hz = lv_slider_get_value(lv_event_get_target_obj(e));
}

static void back(lv_event_t *e)
{
    (void)e;
    t5_app_exit(t5_app_self());
}

static void on_create(t5_app_t *app)
{
    state_t *s = calloc(1, sizeof *s);
    if (!s) return;
    s->app = app;
    s->hz = 440;
    lv_obj_t *page = md_page_create(t5_app_screen(app), "Tone", back, NULL, NULL);
    lv_obj_t *body = md_page_body(page);
    lv_obj_t *row = md_row_create(body, MD_ICON_MUSIC_NOTE, "Play", NULL);
    lv_obj_add_event_cb(md_row_switch(row, false), toggled, LV_EVENT_VALUE_CHANGED, s);
    lv_obj_t *sl = md_slider_create(body, 110, 880, s->hz);
    lv_obj_set_width(sl, LV_PCT(90));
    lv_obj_add_event_cb(sl, pitch, LV_EVENT_VALUE_CHANGED, s);
    if (t5_task_create("synth", synth_task, s, 4096, 2, T5_CORE_ANY, NULL) != T5_OK) {
        T5_IGNORE(t5_shell_toast("Couldn't start audio"));
    }
}

T5_APP(.on_create = on_create);
```

## Camera with permission

The camera asks the person the first time. Open it in the answer callback, close it in `on_suspend`, and reopen it in `on_resume`.

Manifest: `camera = true` and `shared_write = true`, for saving to `/sd/DCIM`.

<!-- compile: camera -->
```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <t5/app.h>
#include <t5/camera.h>
#include <t5/shell.h>
#include <t5/storage.h>
#include <t5/sys.h>
#include <t5/ui.h>

typedef struct {
    t5_app_t *app;
    t5_camera_t *cam;
    lv_obj_t *holder, *button, *preview;
    bool granted;
} state_t;

static void open_camera(state_t *s)
{
    if (s->cam || !s->granted) return;
    t5_err_t e = t5_camera_open(&s->cam);
    if (e != T5_OK) {
        s->cam = NULL;
        md_snackbar_show(t5_err_str(e), NULL, NULL, NULL);
        return;
    }
    s->preview = t5_camera_preview_create(s->cam, s->holder);
}

static void close_camera(state_t *s)
{
    if (s->preview) lv_obj_delete(s->preview);
    s->preview = NULL;
    if (s->cam) T5_IGNORE(t5_camera_close(s->cam));     /* nothing to do if it fails */
    s->cam = NULL;
}

static void answer(t5_app_t *app, uint32_t perm, bool granted, void *user)
{
    (void)app;
    (void)perm;
    state_t *s = user;
    s->granted = granted;
    if (granted) open_camera(s);
    else md_snackbar_show("The camera permission is off (App Manager)", NULL, NULL, NULL);
}

/* The still blocks until the file is written, so it can't run on the UI task. */
static void photo_done(void *arg)
{
    state_t *s = arg;
    lv_obj_remove_state(s->button, LV_STATE_DISABLED);
}

static void photo_task(void *arg)
{
    state_t *s = arg;
    mkdir("/sd/DCIM", 0755);                    /* fine if it already exists */
    t5_err_t e = s->cam ? t5_camera_photo(s->cam, "/sd/DCIM/photo.jpg", 90) : T5_ERR_STATE;
    T5_IGNORE(t5_shell_toast(e == T5_OK ? "Saved to /sd/DCIM/photo.jpg" : t5_err_str(e)));
    T5_IGNORE(t5_ui_async(photo_done, s));      /* only fails if the queue is full */
}

static void shoot(lv_event_t *e)
{
    state_t *s = lv_event_get_user_data(e);
    if (t5_task_create("photo", photo_task, s, 8192, 2, T5_CORE_ANY, NULL) == T5_OK) {
        lv_obj_add_state(s->button, LV_STATE_DISABLED);
    }
}

static void back(lv_event_t *e)
{
    (void)e;
    t5_app_exit(t5_app_self());
}

static void on_create(t5_app_t *app)
{
    state_t *s = calloc(1, sizeof *s);
    if (!s) return;
    s->app = app;
    t5_app_set_user(app, s);
    lv_obj_t *scr = t5_app_screen(app);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(scr, 0, 0);
    md_appbar_create(scr, "Camera", MD_ICON_ARROW_BACK, back, NULL);
    s->holder = lv_obj_create(scr);
    lv_obj_remove_style_all(s->holder);
    lv_obj_set_width(s->holder, LV_PCT(100));
    lv_obj_set_flex_grow(s->holder, 1);
    s->button = md_fab_create(scr, MD_ICON_CAMERA_ALT, false);
    lv_obj_set_floating(s->button, true);
    lv_obj_align(s->button, LV_ALIGN_BOTTOM_RIGHT, -T5_DP(16), -T5_DP(16));
    lv_obj_add_event_cb(s->button, shoot, LV_EVENT_CLICKED, s);
    if (t5_app_has_permission(app, T5_PERM_CAMERA)) s->granted = true;
    else if (t5_app_request_permission(app, T5_PERM_CAMERA, answer, s) != T5_OK) answer(app, T5_PERM_CAMERA, false, s);
}

static void on_resume(t5_app_t *app)    /* also runs right after on_create */
{
    state_t *s = t5_app_get_user(app);
    if (s) open_camera(s);
}

static void on_suspend(t5_app_t *app)
{
    state_t *s = t5_app_get_user(app);
    if (s) close_camera(s);
}

T5_APP(.on_create = on_create, .on_resume = on_resume, .on_suspend = on_suspend);
```

## Modbus poller

RS-485 transactions take tens of milliseconds, so they run on a task.

Manifest: `ports = ["rs485"]`.

<!-- compile: modbus -->
```c
#include <stdio.h>
#include <stdlib.h>
#include <t5/app.h>
#include <t5/port.h>
#include <t5/sys.h>
#include <t5/ui.h>

typedef struct {
    t5_app_t *app;
    lv_obj_t *label;
} state_t;

typedef struct {
    state_t *s;
    char text[64];
} update_t;

static void apply(void *arg)
{
    update_t *u = arg;
    lv_label_set_text(u->s->label, u->text);
    free(u);
}

static void post(state_t *s, const char *text)
{
    update_t *u = malloc(sizeof *u);
    if (!u) return;
    u->s = s;
    snprintf(u->text, sizeof u->text, "%s", text);
    if (t5_ui_async(apply, u) != T5_OK) free(u);
}

static void poll_task(void *arg)
{
    state_t *s = arg;
    t5_port_t *bus = NULL;
    t5_port_uart_cfg_t uc = { .baud = 9600, .parity = 2 };
    char why[T5_PORT_WHY_LEN] = "";
    t5_err_t e = t5_port_open_ex("rs485", T5_MODE_RS485, &uc, 0, &bus, why, sizeof why);
    if (e != T5_OK) {
        post(s, why[0] ? why : t5_err_str(e));
        return;
    }
    t5_modbus_t mb;
    t5_modbus_init(&mb, bus);
    while (t5_app_running(s->app)) {
        uint16_t regs[2];
        char text[64];
        e = t5_modbus_read_holding(&mb, 1, 0x0000, 2, regs);
        if (e == T5_OK) snprintf(text, sizeof text, "Registers: %u, %u", regs[0], regs[1]);
        else snprintf(text, sizeof text, "Read failed: %s", t5_err_str(e));
        post(s, text);
        t5_delay_ms(1000);
    }
    T5_IGNORE(t5_port_close(bus));                      /* closed at exit anyway */
}

static void back(lv_event_t *e)
{
    (void)e;
    t5_app_exit(t5_app_self());
}

static void on_create(t5_app_t *app)
{
    state_t *s = calloc(1, sizeof *s);
    if (!s) return;
    s->app = app;
    lv_obj_t *page = md_page_create(t5_app_screen(app), "Modbus", back, NULL, NULL);
    s->label = md_label_create(md_page_body(page), "Starting...", MD_TYPE_BODY1);
    if (t5_task_create("modbus", poll_task, s, 4096, 2, T5_CORE_ANY, NULL) != T5_OK) {
        lv_label_set_text(s->label, "Couldn't start the task");
    }
}

T5_APP(.on_create = on_create);
```

## Python dashboard

The sensor dashboard in about twenty lines. Tap callbacks run during `t5.sleep_ms`, and the port closes itself when the app exits.

```python
import t5

screen = t5.ui.screen("Room")
temp = screen.label("--", role="h4")
state = screen.label("Starting...", role="body2")
chart = screen.chart(points=60, min=10, max=40)

rail = t5.port.rail("ext5v")
grove = t5.port.open("grove", mode="i2c", hz=100_000)
t5.sleep_ms(50)

while t5.app.running():
    try:
        grove.xfer(0x44, b"\xFD", 0)
        t5.sleep_ms(20)
        raw = grove.xfer(0x44, b"", 6)
        c = -45 + 175 * (raw[0] << 8 | raw[1]) / 65535
        temp.set_text("%.1f °C" % c)
        chart.push(c)
        state.set_text("Sensor OK")
    except OSError as e:
        temp.set_text("--")
        state.set_text("Sensor: %s" % e)
    t5.sleep_ms(1000)
```

```toml
id = "com.you.room"
name = "Room"
version = "1.0.0"
api = 1
runtime = "micropython"
entry = "main.py"

[permissions]
ports = ["grove"]
```
