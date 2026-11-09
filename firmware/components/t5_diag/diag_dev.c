/* SPDX-License-Identifier: Apache-2.0
 * rtc, imu, fs, disp, touch commands (1.4-1.8)
 */
#include "esp_attr.h"
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include "soc/gpio_struct.h"
#include "t5_pins/pins.h"
#include "soc/gpio_sig_map.h"
#include "driver/gpio.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "t5_core/os.h"
#include "t5_bsp/panel.h"
#include "t5_display/t5_display.h"
#include "t5_display/timing.h"
#include "t5_imu/t5_imu.h"
#include "t5_input/t5_touch.h"
#include "t5_storage/t5_storage.h"
#include "t5_time/rtc.h"
#include "t5_time/t5_time.h"
#include "t5_storage/storage_diag.h"
#include "diag_priv.h"

/* ---- rtc ---------------------------------------------------------------- */
static void print_tm(const char *label, const struct tm *t)
{
    printf("%s%04d-%02d-%02d %02d:%02d:%02d UTC\n", label, t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
           t->tm_hour, t->tm_min, t->tm_sec);
}

static int cmd_rtc(int argc, char **argv)
{
    uint32_t v;
    if (argc == 1) {
        struct tm t;
        bool valid;
        uint8_t flags = 0;
        t5_err_t e = t5_rtc_read(&t, &valid);
        if (e != T5_OK) { printf("read: %s\n", t5_err_str(e)); return 1; }
        t5_rtc_flags(&flags);
        print_tm("rtc     ", &t);
        time_t now = t5_time_now();
        struct tm s;
        gmtime_r(&now, &s);
        print_tm("system  ", &s);
        printf("valid   %s (rtc), wall time %s, source %d\n", valid ? "yes" : "NO",
               t5_time_valid() ? "valid" : "unknown", t5_time_source());
        printf("flags   now 0x%02x, at boot 0x%02x (VLF=02 AF=08 TF=10 VBLF=80)\n", flags, t5_rtc_boot_flags());
        printf("epoch   %lld\n", (long long)now);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "set") && diag_parse_u32(argv[2], 1700000000u, 4000000000u, &v)) {
        t5_err_t e = t5_time_set((time_t)v, T5_TIME_SRC_USER);
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
    if (argc >= 3 && !strcmp(argv[1], "alarm") && diag_parse_u32(argv[2], 1, 60 * 24 * 27, &v)) {
        time_t at = t5_time_now() + 60 * (time_t)v;
        struct tm t;
        gmtime_r(&at, &t);
        t5_err_t e = t5_rtc_alarm_set(&t);
        print_tm("alarm at ", &t);
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
    if (argc >= 3 && !strcmp(argv[1], "timer") && diag_parse_u32(argv[2], 0, 3932100, &v)) {
        uint32_t actual = 0;
        t5_err_t e = t5_rtc_timer_set(v, &actual);
        printf("%s, fires in %u s\n", t5_err_str(e), (unsigned)actual);
        return e != T5_OK;
    }
    if (argc >= 2 && !strcmp(argv[1], "clear")) {
        t5_rtc_alarm_clear();
        t5_rtc_timer_set(0, NULL);
        printf("alarm and timer off, flags cleared\n");
        return 0;
    }
    printf("usage: rtc | rtc set <epoch> | rtc alarm <minutes> | rtc timer <seconds> | rtc clear\n");
    return 1;
}

/* ---- imu ---------------------------------------------------------------- */
static const char *imu_status_str(t5_imu_status_t s)
{
    static const char *n[] = { "off", "starting", "ready", "FAILED" };
    return n[s];
}

static int cmd_imu(int argc, char **argv)
{
    uint32_t v;
    if (argc == 1 || !strcmp(argv[1], "read")) {
        t5_imu_accel_t a;
        int32_t tc = 0;
        printf("status %s (attempts %d)\n", imu_status_str(t5_imu_status()), t5_imu_attempts());
        if (t5_imu_read_accel(&a) == T5_OK) {
            int mag2 = a.x * a.x + a.y * a.y + a.z * a.z;
            printf("accel  x %5d  y %5d  z %5d mg   |a|^2 %d (1 g = 1000000)\n", a.x, a.y, a.z, mag2);
        }
        if (t5_imu_read_temp(&tc) == T5_OK) printf("temp   %d.%02d C\n", (int)(tc / 100), (int)abs(tc % 100));
        return 0;
    }
    if (!strcmp(argv[1], "watch") && argc >= 3 && diag_parse_u32(argv[2], 1, 600, &v)) {
        for (uint32_t i = 0; i < v * 5; i++) {
            t5_imu_accel_t a;
            bool moved = false;
            if (t5_imu_read_accel(&a) == T5_OK) {
                t5_imu_motion_status(&moved);
                printf("%5d %5d %5d%s\n", a.x, a.y, a.z, moved ? "  MOTION" : "");
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        return 0;
    }
    if (!strcmp(argv[1], "motion") && argc >= 3) {
        if (!strcmp(argv[2], "off")) {
            t5_err_t e = t5_imu_motion_disable();
            printf("%s\n", t5_err_str(e));
            return e != T5_OK;
        }
        if (!diag_parse_u32(argv[2], 1, 1000, &v)) goto usage;
        t5_imu_motion_cfg_t c = {
            .threshold_mg = (uint16_t)v, .duration_ms = 100, .pins = T5_IMU_INT1 | T5_IMU_INT2,
            .active_high = argc >= 4 && !strcmp(argv[3], "high"), .open_drain = false, .latched = true,
        };
        t5_err_t e = t5_imu_motion_enable(&c);
        printf("any-motion %u mg on INT1+INT2, active %s, latched: %s\n", (unsigned)v,
               c.active_high ? "high" : "low", t5_err_str(e));
        return e != T5_OK;
    }
usage:
    printf("usage: imu | imu watch <s> | imu motion <mg> [high|low] | imu motion off\n");
    return 1;
}

/* ---- fs ----------------------------------------------------------------- */
#define STRESS_PATH T5_DATA_PATH "/stress.bin"
#define STRESS_MAGIC 0x54355354u

typedef struct {
    uint32_t magic, seq, len, crc;
} stress_hdr_t;

static int fs_verify(void)
{
    FILE *f = fopen(STRESS_PATH, "rb");
    if (!f) { printf("VERIFY none (no file)\n"); return 0; }
    stress_hdr_t h;
    EXT_RAM_BSS_ATTR static uint8_t buf[4096];
    size_t n = fread(&h, 1, sizeof h, f);
    size_t m = n == sizeof h && h.len <= sizeof buf ? fread(buf, 1, h.len, f) : 0;
    fclose(f);
    if (n != sizeof h || h.magic != STRESS_MAGIC || m != h.len || esp_rom_crc32_le(0, buf, h.len) != h.crc) {
        printf("VERIFY CORRUPT (hdr %u bytes, magic %08x, len %u, read %u)\n", (unsigned)n,
               (unsigned)h.magic, (unsigned)h.len, (unsigned)m);
        return 1;
    }
    printf("VERIFY ok seq %u len %u\n", (unsigned)h.seq, (unsigned)h.len);
    return 0;
}

static int fs_stress(uint32_t count)
{
    EXT_RAM_BSS_ATTR static uint8_t buf[sizeof(stress_hdr_t) + 4096];
    uint32_t seq = 0;
    FILE *f = fopen(STRESS_PATH, "rb");
    if (f) {
        stress_hdr_t h;
        if (fread(&h, 1, sizeof h, f) == sizeof h && h.magic == STRESS_MAGIC) seq = h.seq;
        fclose(f);
    }
    int64_t t0 = t5_time_us();
    for (uint32_t i = 0; i < count; i++) {
        stress_hdr_t *h = (stress_hdr_t *)buf;
        uint8_t *p = buf + sizeof *h;
        h->magic = STRESS_MAGIC;
        h->seq = ++seq;
        h->len = 256 + esp_random() % 3840;
        esp_fill_random(p, h->len);
        h->crc = esp_rom_crc32_le(0, p, h->len);
        t5_err_t e = t5_storage_write_atomic(STRESS_PATH, buf, sizeof *h + h->len);
        if (e != T5_OK) { printf("write %u failed: %s\n", (unsigned)seq, t5_err_str(e)); return 1; }
        if (i % 20 == 0) printf("seq %u\n", (unsigned)seq);
    }
    printf("STRESS done, seq %u, %.1f ms/write\n", (unsigned)seq,
           (t5_time_us() - t0) / 1000.0 / (count ? count : 1));
    return 0;
}

static int fs_ls(const char *path)
{
    DIR *d = opendir(path);
    if (!d) { printf("can't open %s\n", path); return 1; }
    struct dirent *e;
    char full[300];
    while ((e = readdir(d))) {
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        if (stat(full, &st) == 0) printf("%10ld  %s%s\n", (long)st.st_size, e->d_name, S_ISDIR(st.st_mode) ? "/" : "");
    }
    closedir(d);
    return 0;
}

static int cmd_fs(int argc, char **argv)
{
    uint32_t v;
    if (argc == 1 || !strcmp(argv[1], "df")) {
        uint64_t total = 0, used = 0;
        if (t5_storage_data_usage(&total, &used) == T5_OK) {
            printf("/data  littlefs  %llu KB used of %llu KB\n", (unsigned long long)(used >> 10),
                   (unsigned long long)(total >> 10));
        } else {
            printf("/data  NOT MOUNTED\n");
        }
        t5_sd_info_t i;
        printf("/sd    %s", t5_storage_sd_state_str(t5_storage_sd_state()));
        if (t5_storage_sd_info(&i) == T5_OK) {
            printf("  %s %u MB, %u-bit @ %u kHz, %llu MB free of %llu MB", i.name, (unsigned)i.capacity_mb,
                   i.bus_width, (unsigned)i.freq_khz, (unsigned long long)(i.free_bytes >> 20),
                   (unsigned long long)(i.total_bytes >> 20));
        }
        printf("\n");
        return 0;
    }
    if (!strcmp(argv[1], "ls")) return fs_ls(argc >= 3 ? argv[2] : T5_DATA_PATH);
    if (!strcmp(argv[1], "cat") && argc >= 3) {
        FILE *f = fopen(argv[2], "r");
        if (!f) { printf("can't open %s\n", argv[2]); return 1; }
        int c;
        while ((c = fgetc(f)) != EOF) putchar(c);
        fclose(f);
        return 0;
    }
    if (!strcmp(argv[1], "rm") && argc >= 3) { printf("%s\n", remove(argv[2]) == 0 ? "removed" : "failed"); return 0; }
    if (!strcmp(argv[1], "verify")) return fs_verify();
    if (!strcmp(argv[1], "stress") && argc >= 3 && diag_parse_u32(argv[2], 1, 100000, &v)) return fs_stress(v);
    if (!strcmp(argv[1], "eject")) { printf("%s\n", t5_err_str(t5_storage_sd_eject())); return 0; }
    if (!strcmp(argv[1], "rescan")) { printf("%s\n", t5_err_str(t5_storage_sd_rescan())); return 0; }
    if (!strcmp(argv[1], "sdraw")) {
        static const uint32_t ios[] = { 1, 8, 64, 128 };
        for (size_t i = 0; i < 4; i++) {
            printf("raw read, %3u sectors/io: %d KB/s\n", (unsigned)ios[i], t5_storage_hw_sd_raw_bench(ios[i], 8192));
        }
        return 0;
    }
    if (!strcmp(argv[1], "sdfat")) {
        int type = 0, cl = 0;
        if (t5_storage_hw_sd_fat_info(&type, &cl)) { printf("no card\n"); return 1; }
        printf("FAT type %d (1=FAT12 2=FAT16 3=FAT32 4=exFAT), cluster %d bytes\n", type, cl);
        return 0;
    }
    if (!strcmp(argv[1], "sdposix")) {
        const size_t len = 8 << 20, chunk = 64 * 1024;
        uint8_t *b = heap_caps_aligned_alloc(64, chunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!b) return 1;
        esp_fill_random(b, chunk);
        int fd = open(T5_SD_PATH "/bench.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int64_t t0 = t5_time_us();
        for (size_t i = 0; fd >= 0 && i < len; i += chunk) write(fd, b, chunk);
        if (fd >= 0) { fsync(fd); close(fd); }
        int64_t t1 = t5_time_us();
        fd = open(T5_SD_PATH "/bench.bin", O_RDONLY);
        while (fd >= 0 && read(fd, b, chunk) > 0) {}
        if (fd >= 0) close(fd);
        int64_t t2 = t5_time_us();
        remove(T5_SD_PATH "/bench.bin");
        heap_caps_free(b);
        printf("SD 8 MB posix 64K: write %.1f MB/s read %.1f MB/s\n", len / (double)(t1 - t0), len / (double)(t2 - t1));
        return 0;
    }
    if (!strcmp(argv[1], "sdbench")) {
        /* Same 4 MB with three buffer kinds: shows what the SDMMC DMA path needs. */
        static const struct { const char *name; uint32_t caps; size_t align; } kinds[] = {
            { "psram, malloc", MALLOC_CAP_SPIRAM, 0 },
            { "psram, 64-aligned", MALLOC_CAP_SPIRAM, 64 },
            { "internal DMA", MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL, 64 },
        };
        const size_t len = 4 << 20, chunk = 32 * 1024;
        for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
            /* base is what we free; b is what we use (base + 4 for the misaligned case). */
            uint8_t *base = kinds[k].align ? heap_caps_aligned_alloc(kinds[k].align, chunk + 8, kinds[k].caps)
                                           : heap_caps_malloc(chunk + 8, kinds[k].caps);
            uint8_t *b = base;
            if (base && !kinds[k].align && ((uintptr_t)base & 63) == 0) b = base + 4;
            FILE *f = b ? fopen(T5_SD_PATH "/bench.bin", "wb") : NULL;
            if (!f) { printf("no SD or no memory\n"); heap_caps_free(base); return 1; }
            esp_fill_random(b, chunk);
            int64_t t0 = t5_time_us();
            for (size_t i = 0; i < len; i += chunk) fwrite(b, 1, chunk, f);
            fclose(f);
            int64_t t1 = t5_time_us();
            f = fopen(T5_SD_PATH "/bench.bin", "rb");
            while (f && fread(b, 1, chunk, f) > 0) {}
            if (f) fclose(f);
            int64_t t2 = t5_time_us();
            remove(T5_SD_PATH "/bench.bin");
            printf("SD 4 MB, %-18s write %5.1f MB/s  read %5.1f MB/s\n", kinds[k].name,
                   len / (double)(t1 - t0), len / (double)(t2 - t1));
            heap_caps_free(base);
        }
        return 0;
    }
    printf("usage: fs [df] | fs ls [path] | fs cat <file> | fs rm <file> | fs stress <n> | fs verify | fs eject | fs rescan | fs sdbench\n");
    return 1;
}

/* ---- disp / touch ------------------------------------------------------- */
static int cmd_disp(int argc, char **argv)
{
    uint32_t v, r, g, b;
    if (argc == 1) {
        printf("panel %s, %s, brightness %d%% (applied %d%%), %s, showing fb %d, %u frames scanned\n",
               t5_bsp_panel_name(t5_bsp_panel()->panel), t5_display_ready() ? "ready" : "NOT READY",
               t5_display_brightness(), t5_display_effective_brightness(),
               t5_display_asleep() ? "asleep" : "awake", t5_display_scanout(), (unsigned)t5_display_frames());
        printf("backlight pin G%d: output signal %u (LEDC is %u, plain GPIO is %u), level %d\n", T5_PIN_LCD_BACKLIGHT,
               (unsigned)GPIO.func_out_sel_cfg[T5_PIN_LCD_BACKLIGHT].out_sel, (unsigned)LEDC_LS_SIG_OUT_PAD_OUT0_IDX,
               (unsigned)SIG_GPIO_OUT_IDX, gpio_get_level(T5_PIN_LCD_BACKLIGHT));
        return 0;
    }
    if (!strcmp(argv[1], "bl") && argc >= 3 && diag_parse_u32(argv[2], 0, 100, &v)) {
        return t5_display_set_brightness((int)v) != T5_OK;
    }
    if (!strcmp(argv[1], "blfreq") && argc >= 3 && diag_parse_u32(argv[2], T5_DISPLAY_BL_HZ_MIN, T5_DISPLAY_BL_HZ_MAX, &v)) {
        t5_err_t e = t5_display_set_backlight_freq(v);
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
    if (!strcmp(argv[1], "timing") && argc >= 3) {
        if (!strcmp(argv[2], "reset")) {
            remove(T5_DISPLAY_TIMING_FILE);
            printf("timing override removed; reboot to apply\n");
            return 0;
        }
        uint32_t pclk, vfp;
        if (argc >= 4 && diag_parse_u32(argv[2], T5_DISPLAY_PCLK_MHZ_MIN, T5_DISPLAY_PCLK_MHZ_MAX, &pclk) &&
            diag_parse_u32(argv[3], T5_DISPLAY_VFP_MIN, T5_DISPLAY_VFP_MAX, &vfp)) {
            FILE *f = fopen(T5_DISPLAY_TIMING_FILE, "w");
            if (!f) { printf("can't write /data\n"); return 1; }
            fprintf(f, "%u %u\n", (unsigned)pclk, (unsigned)vfp);
            fclose(f);
            printf("saved %u MHz, vfp %u; reboot to apply\n", (unsigned)pclk, (unsigned)vfp);
            return 0;
        }
    }
    if (!strcmp(argv[1], "sleep")) { printf("%s\n", t5_err_str(t5_display_sleep())); return 0; }
    if (!strcmp(argv[1], "wake")) { printf("%s\n", t5_err_str(t5_display_wake())); return 0; }
    if (!strcmp(argv[1], "ids")) {
        uint8_t id[3] = {0}, pm = 0;
        t5_err_t e = t5_display_read_ids(id, &pm);
        printf("%s: DCS ID %02x %02x %02x, power mode 0x%02x\n", t5_err_str(e), id[0], id[1], id[2], pm);
        return e != T5_OK;
    }
    if (!strcmp(argv[1], "fill") && argc >= 5 && diag_parse_u32(argv[2], 0, 255, &r) &&
        diag_parse_u32(argv[3], 0, 255, &g) && diag_parse_u32(argv[4], 0, 255, &b)) {
        return t5_display_fill_rect(0, 0, T5_DISPLAY_W, T5_DISPLAY_H, t5_rgb565(r, g, b)) != T5_OK;
    }
    printf("usage: disp | disp bl <0-100> | disp blfreq <hz> | disp timing <pclk> <vfp>|reset | disp fill <r> <g> <b> | disp sleep | disp wake | disp ids\n");
    return 1;
}

static volatile bool s_dots;

static void dots_cb(const t5_touch_state_t *s, void *arg)
{
    (void)arg;
    if (!s_dots) return;
    static const uint16_t colours[] = { 0xF800, 0x07E0, 0x001F, 0xFFE0, 0xF81F };
    for (int i = 0; i < s->count; i++) {
        t5_display_fill_rect(s->p[i].x - 6, s->p[i].y - 6, 12, 12, colours[s->p[i].id % 5]);
    }
}

static int cmd_touch(int argc, char **argv)
{
    uint32_t v;
    t5_touch_info_t i;
    t5_touch_info(&i);
    if (argc == 1) {
        printf("fw %u, controller %ux%u, %s, irqs %u, samples %u, errors %u\n", i.fw_version, i.max_x,
               i.max_y, i.polling ? "polling" : "interrupt", (unsigned)i.irqs, (unsigned)i.samples,
               (unsigned)i.errors);
        return 0;
    }
    if (!strcmp(argv[1], "watch") && argc >= 3 && diag_parse_u32(argv[2], 1, 600, &v)) {
        uint32_t last = 0;
        for (uint32_t k = 0; k < v * 50; k++) {
            t5_touch_state_t s;
            if (t5_touch_read(&s) == T5_OK && s.t_ms != last) {
                last = s.t_ms;
                printf("%u ms: %u point(s)", (unsigned)s.t_ms, s.count);
                for (int j = 0; j < s.count; j++) printf("  #%u (%u,%u) s%u", s.p[j].id, s.p[j].x, s.p[j].y, s.p[j].strength);
                printf("\n");
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        return 0;
    }
    if (!strcmp(argv[1], "dots") && argc >= 3) {
        s_dots = !strcmp(argv[2], "on");
        t5_touch_set_callback(dots_cb, NULL);
        if (s_dots) t5_display_fill_rect(0, 0, T5_DISPLAY_W, T5_DISPLAY_H, t5_rgb565(0x12, 0x12, 0x12));
        printf("touch dots %s\n", s_dots ? "on: draw on the screen" : "off");
        return 0;
    }
    printf("usage: touch | touch watch <s> | touch dots on|off\n");
    return 1;
}

void diag_register_dev(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "rtc", .help = "RTC and wall time", .hint = "| set | alarm | timer | clear", .func = cmd_rtc },
        { .command = "imu", .help = "BMI270 accelerometer", .hint = "| watch <s> | motion <mg>|off", .func = cmd_imu },
        { .command = "fs", .help = "/data and /sd", .hint = "df | ls | stress | verify | eject | rescan | sdbench", .func = cmd_fs },
        { .command = "disp", .help = "Display and backlight", .hint = "| bl | fill | sleep | wake | ids", .func = cmd_disp },
        { .command = "touch", .help = "Touch controller", .hint = "| watch <s> | dots on|off", .func = cmd_touch },
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
}
