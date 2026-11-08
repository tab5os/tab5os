/* SPDX-License-Identifier: Apache-2.0
 * Tab5 OS recovery (plan 9.2).
 *
 * Started by the bootloader after three failed boots in a row, or when the
 * OS asks (Settings > Updates > Restart into recovery, or a RECOVERY file on
 * the SD card). It clears the boot counter first, so "Restart Tab5 OS"
 * gives the system three fresh tries.
 *
 *   Reinstall Tab5 OS   a .t5os from the SD card (top folder or /update),
 *                       checked like any update; an unsigned one asks first
 *   Clear settings      NVS (Wi-Fi, Bluetooth) and /data/sys; apps and user
 *                       files stay
 *   Erase all data      formats /data
 *   Save logs to SD     core dump, crash records and app dumps, and a
 *                       summary, to /sd/tab5-recovery-N
 *   Restart             into the OS, or into its safe mode
 *
 * The UI is plain LVGL drawn straight into the framebuffer the panel scans
 * (portrait, native), with the built-in Montserrat fonts. Long jobs run on
 * a worker task; the UI shows their progress.
 */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_core_dump.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "t5_core/log.h"
#include "t5_bsp/board.h"
#include "t5_bsp/xpin.h"
#include "t5_core/event.h"
#include "t5_display/t5_display.h"
#include "t5_i2c/t5_i2c.h"
#include "t5_input/t5_touch.h"
#include "t5_ota/bootctl.h"
#include "t5_ota/ota.h"
#include "t5_storage/t5_storage.h"

static const char *TAG = "recovery";

#define W T5_DISPLAY_W
#define H T5_DISPLAY_H
#define C_BG 0x101418
#define C_CARD 0x1e252b
#define C_ACCENT 0x4dd0e1
#define C_WARN 0xff8a65

/* ---- worker ------------------------------------------------------------------------ */
typedef enum { JOB_NONE, JOB_INSTALL, JOB_CLEAR, JOB_ERASE, JOB_LOGS } job_t;

static volatile job_t s_job;
static volatile bool s_busy;
static char s_status[200] = "Choose an action.";
static char s_job_path[128];
static bool s_job_unsigned;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_status_dirty = true;

__attribute__((format(printf, 1, 2))) static void status(const char *fmt, ...)
{
    char tmp[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    taskENTER_CRITICAL(&s_mux);
    memcpy(s_status, tmp, sizeof s_status);
    s_status_dirty = true;
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "%s", tmp);
}

/* Bounded recursion (docs/coding-standard.md, rule 1): /data/sys is the
 * OS's own and a few levels deep; anything deeper is left alone rather
 * than risk the stack. */
#define RM_TREE_DEPTH_MAX 8

static int rm_tree(const char *path, int depth)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return remove(path) == 0;
    if (depth >= RM_TREE_DEPTH_MAX) return 0;
    int n = 0;
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        char sub[256];
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(sub, sizeof sub, "%.180s/%.60s", path, e->d_name);
            n += rm_tree(sub, depth + 1);
        }
        closedir(d);
    }
    rmdir(path);
    return n;
}

static bool copy_file(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb");
    if (!a) return false;
    FILE *b = fopen(to, "wb");
    if (!b) {
        fclose(a);
        return false;
    }
    char buf[2048];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, a)) > 0) ok &= fwrite(buf, 1, n, b) == n;
    fclose(a);
    ok &= fclose(b) == 0;
    return ok;
}

static void describe_slot(FILE *f, const char *label)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label);
    esp_app_desc_t d;
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (!p) return;
    esp_ota_get_state_partition(p, &st);
    if (esp_ota_get_partition_description(p, &d) == ESP_OK) {
        fprintf(f, "%-8s %s %s, built %s %s, state %d\n", label, d.project_name, d.version, d.date, d.time, (int)st);
    } else {
        fprintf(f, "%-8s (no valid image)\n", label);
    }
}

static void job_logs(void)
{
    if (t5_storage_sd_state() != T5_SD_MOUNTED) {
        status("No SD card: put one in and try again.");
        return;
    }
    char dir[64];
    for (int i = 1; i < 1000; i++) {
        snprintf(dir, sizeof dir, "/sd/tab5-recovery-%d", i);
        struct stat st;
        if (stat(dir, &st) != 0) break;
    }
    mkdir(dir, 0755);
    char path[160];
    int saved = 0;
    /* The core dump partition, as the ELF core file espcoredump made. */
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_get(&addr, &size) == ESP_OK && size) {
        const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
        snprintf(path, sizeof path, "%s/coredump.elf", dir);
        FILE *f = p ? fopen(path, "wb") : NULL;
        char *buf = malloc(4096);
        if (f && buf) {
            for (size_t off = 0; off < size; off += 4096) {
                size_t n = size - off < 4096 ? size - off : 4096;
                esp_partition_read(p, addr - p->address + off, buf, n);
                fwrite(buf, 1, n, f);
            }
            saved++;
        }
        free(buf);
        if (f) fclose(f);
    }
    /* Crash records and the app dumps the OS kept. */
    snprintf(path, sizeof path, "%s/crashes.bin", dir);
    saved += copy_file("/data/sys/crashes.bin", path);
    DIR *d = opendir("/data/sys/dumps");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char from[160];
            snprintf(from, sizeof from, "/data/sys/dumps/%.60s", e->d_name);
            snprintf(path, sizeof path, "%s/%.60s", dir, e->d_name);
            saved += copy_file(from, path);
        }
        closedir(d);
    }
    snprintf(path, sizeof path, "%s/summary.txt", dir);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "Tab5 OS recovery %s\n", esp_app_get_description()->version);
        describe_slot(f, "ota_0");
        describe_slot(f, "ota_1");
        const esp_partition_t *boot = esp_ota_get_boot_partition();
        fprintf(f, "next boot: %s\n", boot ? boot->label : "?");
        fclose(f);
        saved++;
    }
    char msg[120];
    snprintf(msg, sizeof msg, "Saved %d file%s to %s on the SD card.", saved, saved == 1 ? "" : "s", dir);
    status("%s", msg);
}

static void job_clear(void)
{
    status("Clearing settings...");
    nvs_flash_erase_partition("nvs");
    int n = rm_tree("/data/sys", 0);
    mkdir("/data/sys", 0755);
    char msg[120];
    snprintf(msg, sizeof msg, "Settings cleared (Wi-Fi, Bluetooth and %d system files). Apps and your files stay.", n);
    status("%s", msg);
}

static void job_erase(void)
{
    status("Erasing /data...");
    esp_err_t e = esp_littlefs_format("data");
    status(e == ESP_OK ? "/data is empty: apps, settings and files are gone." : "Erasing /data failed.");
}

static void job_install(void)
{
    status("Installing %s...", s_job_path);
    char err[128] = "";
    t5_err_t e = t5_ota_install_file(s_job_path, s_job_unsigned, err, sizeof err);
    if (e == T5_OK) status("Installed. Restart Tab5 OS to run it.");
    else status("Not installed: %s", err);
}

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        job_t j = s_job;
        if (j != JOB_NONE) {
            s_busy = true;
            switch (j) {
            case JOB_INSTALL: job_install(); break;
            case JOB_CLEAR: job_clear(); break;
            case JOB_ERASE: job_erase(); break;
            case JOB_LOGS: job_logs(); break;
            default: break;
            }
            s_job = JOB_NONE;
            s_busy = false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ---- UI ------------------------------------------------------------------------------ */
static lv_obj_t *s_status_label, *s_overlay;
static uint32_t s_ms(void);
static uint32_t s_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a;
    (void)px;
    if (lv_display_flush_is_last(d)) t5_display_present(0, true);
    lv_display_flush_ready(d);
}

static uint32_t s_ms(void);
static volatile int32_t s_tap_x, s_tap_y;
static volatile uint32_t s_tap_until;

static void touch_read(lv_indev_t *i, lv_indev_data_t *data)
{
    (void)i;
    if (s_tap_until && s_ms() < s_tap_until) {      /* a console tap */
        data->point.x = s_tap_x;
        data->point.y = s_tap_y;
        data->state = LV_INDEV_STATE_PRESSED;
        return;
    }
    s_tap_until = 0;
    t5_touch_state_t st;
    if (t5_touch_read(&st) == T5_OK && st.count) {
        data->point.x = st.p[0].x;
        data->point.y = st.p[0].y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static lv_obj_t *text(lv_obj_t *parent, const char *t, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, t);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

typedef void (*action_fn_t)(void);

static void action_clicked(lv_event_t *e)
{
    if (s_busy || s_overlay) return;
    ((action_fn_t)lv_event_get_user_data(e))();
}

static lv_obj_t *button(lv_obj_t *parent, const char *title, const char *sub, action_fn_t fn)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_width(b, LV_PCT(100));
    lv_obj_set_height(b, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_pad_all(b, 28, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(b, 8, 0);
    text(b, title, &lv_font_montserrat_28, 0xffffff);
    text(b, sub, &lv_font_montserrat_28, 0x9aa5ad);
    lv_obj_add_event_cb(b, action_clicked, LV_EVENT_CLICKED, (void *)fn);
    return b;
}

/* A confirmation over everything: title, text, the action, Cancel. */
static action_fn_t s_confirm_fn;

static void overlay_close(void)
{
    if (s_overlay) lv_obj_delete(s_overlay);
    s_overlay = NULL;
}

static void confirm_yes(lv_event_t *e)
{
    (void)e;
    action_fn_t fn = s_confirm_fn;
    overlay_close();
    if (fn) fn();
}

static void confirm_no(lv_event_t *e)
{
    (void)e;
    overlay_close();
}

static void confirm(const char *title, const char *body, const char *yes, action_fn_t fn)
{
    s_confirm_fn = fn;
    s_overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_overlay, W, H);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_t *box = lv_obj_create(s_overlay);
    lv_obj_set_size(box, W - 96, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 40, 0);
    lv_obj_set_style_pad_row(box, 24, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    text(box, title, &lv_font_montserrat_40, 0xffffff);
    text(box, body, &lv_font_montserrat_28, 0xc8d0d6);
    lv_obj_t *row = lv_obj_create(box);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 24, 0);
    lv_obj_t *no = lv_button_create(row);
    lv_obj_set_style_bg_color(no, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_pad_all(no, 24, 0);
    lv_obj_add_event_cb(no, confirm_no, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(no);
    lv_label_set_text(nl, "Cancel");
    lv_obj_t *y = lv_button_create(row);
    lv_obj_set_style_bg_color(y, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_pad_all(y, 24, 0);
    lv_obj_add_event_cb(y, confirm_yes, LV_EVENT_CLICKED, NULL);
    lv_obj_t *yl = lv_label_create(y);
    lv_label_set_text(yl, yes);
    lv_obj_set_style_text_color(yl, lv_color_hex(0x000000), 0);
}

static void start(job_t j)
{
    s_job = j;
}

static void do_install(void)
{
    start(JOB_INSTALL);
}

static void act_install(void)
{
    static char paths[8][128];
    int n = t5_ota_find_on_sd(paths, 8);
    if (!n) {
        status(t5_storage_sd_state() == T5_SD_MOUNTED ? "No .t5os file on the SD card (top folder or /update)."
                                                       : "No SD card: put one in with a .t5os update on it.");
        return;
    }
    snprintf(s_job_path, sizeof s_job_path, "%s", paths[0]);
    /* Is it signed by a key this recovery image trusts? Read its header. */
    t5_ota_hdr_t h;
    FILE *f = fopen(s_job_path, "rb");
    bool ok = f && fread(&h, 1, sizeof h, f) == sizeof h && !memcmp(h.magic, T5_OTA_MAGIC, 4);
    if (f) fclose(f);
    if (!ok) {
        status("%s isn't a Tab5 OS update.", s_job_path);
        return;
    }
    char body[300];
    s_job_unsigned = h.sig_len == 0;
    snprintf(body, sizeof body, "%s\nTab5 OS %.15s%s", s_job_path, h.os_version,
             h.sig_len ? "" : "\n\nThis update isn't signed. Install it only if you made it or trust where it "
                              "came from.");
    confirm("Reinstall Tab5 OS?", body, "Install", do_install);
}

static void do_clear(void)
{
    start(JOB_CLEAR);
}

static void act_clear(void)
{
    confirm("Clear settings?", "Wi-Fi networks, Bluetooth, paired computers and system settings are removed. "
            "Installed apps and your files stay.", "Clear", do_clear);
}

static void do_erase(void)
{
    start(JOB_ERASE);
}

static void act_erase(void)
{
    confirm("Erase all data?", "Everything in /data goes: installed apps on the device, their data, settings and "
            "your files there. The SD card isn't touched.", "Erase", do_erase);
}

static void act_logs(void)
{
    start(JOB_LOGS);
}

static void act_restart(void)
{
    status("Restarting...");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

static void act_safe(void)
{
    status("Restarting in safe mode...");
    vTaskDelay(pdMS_TO_TICKS(300));
    T5_LOG_ON_ERR(TAG, t5_ota_reboot_safe_mode());
    status("Couldn't restart in safe mode.");
}

static void build_ui(int failed, bool asked)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, 48, 0);
    lv_obj_set_style_pad_row(scr, 20, 0);
    text(scr, "Tab5 recovery", &lv_font_montserrat_40, 0xffffff);
    char why[160];
    if (asked) snprintf(why, sizeof why, "You asked for recovery.");
    else snprintf(why, sizeof why, "Tab5 OS didn't start properly %d times in a row.", failed);
    text(scr, why, &lv_font_montserrat_28, C_WARN);
    button(scr, "Restart Tab5 OS", "Try the system again", act_restart);
    button(scr, "Restart in safe mode", "The system without installed apps", act_safe);
    button(scr, "Reinstall Tab5 OS", "From a .t5os file on the SD card", act_install);
    button(scr, "Save logs to the SD card", "Crash dumps and records, for a bug report", act_logs);
    button(scr, "Clear settings", "Wi-Fi, Bluetooth and system settings", act_clear);
    button(scr, "Erase all data", "Apps, settings and files in /data", act_erase);
    s_status_label = text(scr, s_status, &lv_font_montserrat_28, C_ACCENT);
    lv_obj_t *ver = text(scr, "", &lv_font_montserrat_28, 0x6b767e);
    lv_label_set_text_fmt(ver, "Recovery %s", esp_app_get_description()->version);
}

/* ---- console: shot, tap, reboot (same format as the OS, so t5 shot works) ------------------- */
static void b64_line(const uint8_t *in, size_t n)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char out[80];
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = t[v >> 18 & 63];
        out[o++] = t[v >> 12 & 63];
        out[o++] = i + 1 < n ? t[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? t[v & 63] : '=';
    }
    out[o] = 0;
    printf("%s\n", out);
}

static int cmd_shot(int argc, char **argv)
{
    int div = argc >= 2 ? atoi(argv[1]) : 4;
    if (div < 1 || div > 16) div = 4;
    const uint16_t *fb = t5_display_fb_n(0);
    int w = W / div, h = H / div;
    uint16_t *row = malloc((size_t)w * 2 * 24);
    if (!fb || !row) {
        free(row);
        return 1;
    }
    printf("rotation 0\nSHOT %d %d\n", w, h);
    /* 48 bytes per line, as the OS prints it. */
    uint8_t buf[48];
    size_t nb = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint16_t p = fb[(size_t)(y * div) * W + x * div];
            buf[nb++] = (uint8_t)(p & 0xff);
            buf[nb++] = (uint8_t)(p >> 8);
            if (nb == sizeof buf) {
                b64_line(buf, nb);
                nb = 0;
            }
        }
    }
    if (nb) b64_line(buf, nb);
    printf("END\n");
    free(row);
    return 0;
}

static int cmd_tap(int argc, char **argv)
{
    if (argc < 3) return 1;
    s_tap_x = atoi(argv[1]);
    s_tap_y = atoi(argv[2]);
    s_tap_until = s_ms() + 100;
    vTaskDelay(pdMS_TO_TICKS(200));
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_restart();
    return 0;
}

static void console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "recovery> ";
    rc.task_stack_size = 6144;
    esp_console_dev_usb_serial_jtag_config_t dc = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    if (esp_console_new_repl_usb_serial_jtag(&dc, &rc, &repl) != ESP_OK) return;
    esp_console_register_help_command();
    const esp_console_cmd_t cmds[] = {
        { .command = "shot", .help = "Screen as base64 RGB565 (t5 shot)", .func = cmd_shot },
        { .command = "tap", .help = "tap X Y (portrait pixels)", .func = cmd_tap },
        { .command = "reboot", .help = "Restart (into the OS)", .func = cmd_reboot },
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) esp_console_cmd_register(&cmds[i]);
    esp_console_start_repl(repl);
}

void app_main(void)
{
    /* Whatever brought us here, the next normal restart gets fresh tries. */
    const esp_partition_t *self = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_TEST, NULL);
    uint8_t rec[72];
    int failed = 0;
    bool asked = false;
    if (self && esp_partition_read(self, self->size - T5_BOOTCTL_SECTOR_FROM_END, rec, sizeof rec) == ESP_OK) {
        failed = t5_bootctl_count(rec);
        asked = !memcmp(rec + T5_BOOTCTL_REQ_OFFSET, T5_BOOTCTL_REQ_RECOVERY, T5_BOOTCTL_REQ_LEN);
    }
    t5_ota_bootctl_clear();
    ESP_LOGW(TAG, "recovery: %s", asked ? "requested" : "after failed boots");

    t5_event_init();
    t5_bsp_board_init();
    T5_LOG_ON_ERR(TAG, t5_i2c_init());
    t5_xpin_init();
    t5_xpin_apply_boot_defaults();
    t5_storage_init();
    if (t5_display_init() != T5_OK) {
        ESP_LOGE(TAG, "no display: only the console is left; restarting in 30 s");
        vTaskDelay(pdMS_TO_TICKS(30000));
        esp_restart();
    }
    t5_touch_init();

    lv_init();
    lv_tick_set_cb(s_ms);
    lv_display_t *d = lv_display_create(W, H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, t5_display_fb_n(0), NULL, (uint32_t)W * H * 2, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(d, flush);
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, touch_read);
    build_ui(failed, asked);
    lv_timer_handler();
    t5_display_release_hold();
    t5_display_set_brightness(70);

    xTaskCreatePinnedToCore(worker, "recovery_job", 8192, NULL, 4, NULL, 0);
    console_start();
    for (;;) {
        if (s_status_dirty) {
            char tmp[200];
            taskENTER_CRITICAL(&s_mux);
            memcpy(tmp, s_status, sizeof tmp);
            s_status_dirty = false;
            taskEXIT_CRITICAL(&s_mux);
            lv_label_set_text(s_status_label, tmp);
        }
        uint32_t next = lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(next < 10 ? 10 : next > 50 ? 50 : next));
    }
}
