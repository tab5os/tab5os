/* SPDX-License-Identifier: Apache-2.0
 * app: the app runtime (plan 5).
 *
 *   app                          installed and built-in apps, the foreground app
 *   app run <id> / app exit      launch, exit
 *   app install <path.t5app> [sd|data]
 *   app rm <id> [keep]           remove (keep: keep its data)
 *   app usage                    the foreground app's tracked resources
 *   app crashes                  crash guard records
 *   app dump <id>                its saved core dump, base64 between DUMP and END
 *   app abi <file.elf>           check an ELF's imports against the ABI
 *   app alarm <id>               run its on_alarm headless now
 *   app fetch <url> [sd|data]    download a .t5app over HTTP(S) and install it
 *   app stress <id> <n>          open and close n times; heap, claims, locks
 *                                and rails must return to baseline (5 exit test)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "t5_core/crc.h"
#include "t5_core/fault.h"
#include "t5_app/abi.h"
#include "t5_app/anr_test.h"
#include "t5_app/apps.h"
#include "t5_app/registry.h"
#include "t5_core/os.h"
#include "t5_core/res.h"
#include "t5_dev/control.h"
#include "t5_diag/t5_diag.h"
#include "t5_power/power.h"
#include "t5_power/rail.h"
#include "t5_net/download.h"
#include "t5_ports/ports.h"
#include "t5_storage/paths.h"
#include "t5_storage/t5_storage.h"
#include "md/md.h"
#include "t5_ui/t5_ui.h"
#include "t5_ui/ui_diag.h"
#include "diag_priv.h"

static void list(void)
{
    t5_app_entry_t *e = calloc(T5_APPS_MAX, sizeof *e);
    if (!e) return;
    int n = t5_apps_list(e, T5_APPS_MAX);
    char cur[64];
    t5_apps_current(cur, sizeof cur);
    printf("API level %d, %u exported symbols; foreground: %s\n", T5_API_LEVEL, (unsigned)t5_abi_size(),
           cur[0] ? cur : "(launcher)");
    for (int i = 0; i < n; i++) {
        printf("  %-32s %-10s %s%s%s\n", e[i].id, e[i].version, e[i].builtin ? "built-in " : "",
               e[i].running ? "running " : "", e[i].autolaunch_blocked ? "auto-launch blocked" : "");
    }
    free(e);
}

static void usage_print(void)
{
    t5_app_usage_t u;
    if (t5_apps_usage(&u) != T5_OK) {
        printf("no foreground app\n");
        return;
    }
    printf("heap %u bytes in %u blocks (peak %u)", (unsigned)u.heap_bytes, (unsigned)u.heap_blocks,
           (unsigned)u.heap_peak);
    for (int i = 0; i < T5_RES_KIND_COUNT; i++) printf(", %s %u", t5_res_kind_name((t5_res_kind_t)i), (unsigned)u.res[i]);
    printf(", untracked %u, denied imports %u\n", (unsigned)u.dropped, (unsigned)u.denied_symbols);
}

/* Things that must come back to where they were after every close. */
typedef struct {
    size_t heap;
    int claimed_pins;
    int locks;
    int rail_holds;
} baseline_t;

/* Screen transitions hold two full-screen snapshots (md_freeze.c, ~1.8 MB
 * each) until the animation ends, which is after an app's exit returns:
 * measure once they're gone, or the baseline is off by megabytes. */
static void settle_ui(void)
{
    for (int i = 0; i < 100; i++) {
        t5_ui_lock();
        bool frozen = md_screens_frozen();
        t5_ui_unlock();
        if (!frozen) return;
        t5_delay_ms(10);
    }
}

static void snapshot(baseline_t *b)
{
    settle_ui();
    /* LVGL's objects live in its own pool (lv_mem_t5.c), invisible to the heap. */
    t5_ui_lvmem_t lm;
    t5_ui_lvmem(&lm);
    b->heap = heap_caps_get_free_size(MALLOC_CAP_8BIT) + lm.pool_free;
    b->claimed_pins = 0;
    uint8_t pins[64];
    int n = t5_ports_pins(pins, 64);
    for (int i = 0; i < n; i++) {
        t5_pin_info_t pi;
        if (t5_ports_pin_info(pins[i], &pi) == T5_OK && pi.claimed) b->claimed_pins++;
    }
    b->locks = 0;
    for (int t = 0; t < T5_LOCK_TYPE_COUNT; t++) b->locks += t5_wakelock_count((t5_lock_type_t)t);
    b->rail_holds = 0;
    for (int r = 0; r < T5_RAIL_COUNT; r++) b->rail_holds += t5_rail_count((t5_rail_t)r);
}

static int stress(const char *id, uint32_t n)
{
    baseline_t base, now;
    /* Hold the screen on, as a person using the device would: otherwise the
     * idle timers turn it off mid-run and the UI (rightly) stops working. */
    t5_wakelock_t *awake = t5_wakelock_acquire(T5_LOCK_SCREEN, "app stress");
    /* One warm-up round: LVGL caches (glyphs, images) fill on first use. */
    if (t5_apps_launch_wait(id, 5000) != T5_OK || t5_apps_exit_wait(5000) != T5_OK) {
        printf("can't run %s\n", id);
        t5_wakelock_release(awake);
        return 1;
    }
    snapshot(&base);
    uint32_t fails = 0;
    long worst = 0;
    for (uint32_t i = 1; i <= n; i++) {
        t5_err_t e = t5_apps_launch_wait(id, 5000);
        if (e == T5_OK) {
            t5_delay_ms(100);    /* let its tasks and timers run */
            e = t5_apps_exit_wait(5000);
        }
        t5_delay_ms(20);
        snapshot(&now);
        long dheap = (long)base.heap - (long)now.heap;
        if (dheap > worst) worst = dheap;
        /* Fragmentation moves free heap by a few hundred bytes; a leak grows. */
        bool bad = e != T5_OK || now.claimed_pins != base.claimed_pins || now.locks != base.locks ||
                   now.rail_holds != base.rail_holds || dheap > 4096;
        if (bad) fails++;
        if (bad || i % 50 == 0 || i == n) {
            printf("  %4u: %s heap %+ld, pins %d/%d, locks %d/%d, rails %d/%d%s\n", (unsigned)i, t5_err_str(e),
                   -dheap, now.claimed_pins, base.claimed_pins, now.locks, base.locks, now.rail_holds,
                   base.rail_holds, bad ? "  <- not back to baseline" : "");
        }
    }
    t5_wakelock_release(awake);
    printf("stress %s: %u cycles, %u not back to baseline, worst heap drift %ld bytes -> %s\n", id, (unsigned)n,
           (unsigned)fails, worst, fails ? "FAIL" : "PASS");
    return fails != 0;
}

static int abi_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("can't open %s\n", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 0 ? malloc((size_t)n) : NULL;
    bool ok = b && fread(b, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok) {
        free(b);
        return 1;
    }
    char missing[256];
    t5_err_t e = t5_apps_check_abi(b, (size_t)n, missing, sizeof missing);
    free(b);
    printf("%s: %s%s%s\n", path, e == T5_OK ? "OK" : t5_err_str(e), missing[0] ? ": " : "", missing);
    return e != T5_OK;
}

/* app put PATH SIZE: receive a file over the console (plan 8.6), as
 * t5_diag.h describes. Only under the temp folder or the SD card. About
 * 14 KB/s. */
static int put_file(const char *path, size_t size)
{
    if (strncmp(path, T5_TMP_DIR "/", sizeof T5_TMP_DIR) && strncmp(path, T5_SD_PATH "/", sizeof T5_SD_PATH)) {
        printf("put: only under %s or %s\n", T5_TMP_DIR, T5_SD_PATH);
        return 1;
    }
    if (strstr(path, "..") || size == 0 || size > T5_DEV_PUSH_MAX) {
        printf("put: bad path or size\n");
        return 1;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("put: can't create %s\n", path);
        return 1;
    }
    static char line[(T5_DIAG_PUT_CHUNK + 2) / 3 * 4 + 16];
    static unsigned char bin[T5_DIAG_PUT_CHUNK];
    size_t got = 0;
    uint32_t crc = 0;
    bool ok = true;
    /* No Sleep during a transfer: it would suspend the SD card under us. */
    t5_wakelock_t *wl = t5_wakelock_acquire(T5_LOCK_CPU, "app put");
    printf("READY\n");
    fflush(stdout);
    size_t l = 0;
    uint32_t idle = 0;
    while (got < size) {
        /* stdin is non-blocking here: build each line a byte at a time. */
        int ch = fgetc(stdin);
        if (ch == EOF) {
            clearerr(stdin);
            vTaskDelay(1);
            if (++idle > 30000) {        /* 30 s with nothing: the sender is gone */
                printf("put: timed out\n");
                ok = false;
                break;
            }
            continue;
        }
        idle = 0;
        if (ch != '\n' && ch != '\r') {
            if (l < sizeof line - 1) line[l++] = (char)ch;
            continue;
        }
        if (!l) continue;
        line[l] = 0;
        l = 0;
        if (!strcmp(line, "ABORT")) {
            ok = false;
            break;
        }
        size_t n = 0;
        char *sp = strchr(line, ' ');
        uint32_t want = sp ? (uint32_t)strtoul(sp + 1, NULL, 16) : 0;
        if (sp) *sp = 0;
        if (!sp || mbedtls_base64_decode(bin, sizeof bin, &n, (const unsigned char *)line, strlen(line)) != 0 ||
            t5_crc32(0, bin, n) != want) {
            printf("@re\n");
            fflush(stdout);
            continue;
        }
        if (got + n > size) {
            printf("put: too much data\n");
            ok = false;
            break;
        }
        if (fwrite(bin, 1, n, f) != n) {
            printf("put: write failed\n");
            ok = false;
            break;
        }
        crc = t5_crc32(crc, bin, n);
        got += n;
        printf("@ok\n");
        fflush(stdout);
    }
    if (fclose(f) != 0) ok = false;
    t5_wakelock_release(wl);
    if (!ok) {
        remove(path);
        return 1;
    }
    printf("OK %u %08x\n", (unsigned)got, (unsigned)crc);
    return 0;
}

static int alarm_job(void *id)
{
    t5_err_t e = t5_apps_run_alarm(id);
    printf("%s\n", t5_err_str(e));
    return e != T5_OK;
}

static int cmd_app(int argc, char **argv)
{
    if (argc == 1) {
        list();
        return 0;
    }
    const char *c = argv[1];
    if (!strcmp(c, "run") && argc == 4) {          /* with a launch argument; doesn't wait */
        t5_err_t e = t5_apps_launch_arg(argv[2], argv[3]);
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
    if (!strcmp(c, "run") && argc == 3) {
        t5_err_t e = t5_apps_launch_wait(argv[2], 10000);
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
    if (!strcmp(c, "exit")) {
        t5_err_t e = t5_apps_exit_wait(5000);
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
    if (!strcmp(c, "put") && argc == 4) return put_file(argv[2], (size_t)strtoul(argv[3], NULL, 10));
    if (!strcmp(c, "install") && argc >= 3) {
        t5_pkg_dest_t dest = t5_apps_dest_parse(argc >= 4 ? argv[3] : NULL);
        char err[256] = "";
        t5_err_t e = t5_apps_install(argv[2], dest, NULL, err, sizeof err);
        printf("%s%s%s\n", t5_err_str(e), err[0] ? ": " : "", err);
        if (e == T5_OK) printf("%s\n", T5_NATIVE_APP_WARNING);
        return e != T5_OK;
    }
    if (!strcmp(c, "fetch") && argc >= 3) {
        const char *name = strrchr(argv[2], '/');
        char path[160];
        snprintf(path, sizeof path, "%s/%.60s", t5_storage_sd_state() == T5_SD_MOUNTED ? T5_SD_PATH : T5_DATA_PATH,
                 name ? name + 1 : "download.t5app");
        char err[256] = "";
        int64_t bytes = 0;
        t5_err_t e = t5_download_file(argv[2], path, &bytes, err, sizeof err);
        if (e != T5_OK) {
            printf("%s: %s\n", t5_err_str(e), err);
            remove(path);
            return 1;
        }
        printf("%lld bytes -> %s\n", (long long)bytes, path);
        t5_pkg_dest_t dest = t5_apps_dest_parse(argc >= 4 ? argv[3] : NULL);
        e = t5_apps_install(path, dest, NULL, err, sizeof err);
        remove(path);
        printf("install: %s%s%s\n", t5_err_str(e), err[0] ? ": " : "", err);
        return e != T5_OK;
    }
    if (!strcmp(c, "rm") && argc >= 3) {
        t5_err_t e = t5_apps_remove(argv[2], argc >= 4 && !strcmp(argv[3], "keep"));
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
    if (!strcmp(c, "usage")) {
        usage_print();
        return 0;
    }
    if (!strcmp(c, "crashes")) {
        t5_crash_rec_t r[T5_CRASH_MAX_APPS];
        int n = t5_apps_crashes(r, T5_CRASH_MAX_APPS);
        printf("faults survived since boot: %u\n", (unsigned)t5_fault_recovered());
        if (!n) printf("no crash records\n");
        for (int i = 0; i < n; i++) {
            char dump[96] = "";
            if (t5_apps_coredump_path(r[i].id, dump, sizeof dump) != T5_OK) dump[0] = 0;
            printf("  %-32s %u in a row, %u total, last: %s%s%s\n", r[i].id, r[i].consecutive, r[i].total,
                   t5_crash_kind_name((t5_crash_kind_t)r[i].last_kind), dump[0] ? ", dump " : "", dump);
        }
        return 0;
    }
    if (!strcmp(c, "dump") && argc == 3) {
        /* An app's saved core dump as base64 (48 bytes a line) between DUMP
         * and END, for esp-coredump on a computer. */
        char path[96];
        if (t5_apps_coredump_path(argv[2], path, sizeof path) != T5_OK) {
            printf("no core dump for %s\n", argv[2]);
            return 1;
        }
        FILE *f = fopen(path, "rb");
        if (!f) {
            printf("can't open %s\n", path);
            return 1;
        }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        printf("DUMP %ld\n", size);
        unsigned char bin[48], line[80];
        size_t n;
        while ((n = fread(bin, 1, sizeof bin, f)) > 0) {
            size_t olen = 0;
            mbedtls_base64_encode(line, sizeof line, &olen, bin, n);
            line[olen] = 0;
            printf("%s\n", line);
        }
        fclose(f);
        printf("END\n");
        return 0;
    }
    if (!strcmp(c, "abi") && argc == 3) return abi_file(argv[2]);
    if (!strcmp(c, "anr") && argc == 3) {
        t5_err_t e = !strcmp(argv[2], "show")    ? anr_test_show()
                     : !strcmp(argv[2], "close") ? anr_test_close()
                                                 : anr_test_hide();
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
    if (!strcmp(c, "alarm") && argc == 3) {
        /* Loading an ELF disables the cache. A real alarm runs on the boot task. */
        return diag_run_internal("t5_alarmcmd", 8192, alarm_job, argv[2]);
    }
    if (!strcmp(c, "stress") && argc == 4) {
        uint32_t n;
        if (!diag_parse_u32(argv[3], 1, 100000, &n)) goto usage;
        return stress(argv[2], n);
    }
usage:
    printf("usage: app [run <id> [arg] | exit | install <file> [sd|data] | rm <id> [keep] | usage | crashes | dump <id> |\n"
           "            abi <elf> | anr show|hide|close | alarm <id> | fetch <url> [sd|data] | stress <id> <n>]\n");
    return 1;
}

void diag_register_app(void)
{
    const esp_console_cmd_t c = { .command = "app", .help = "App runtime: run, install, stress",
                                  .hint = "[run|exit|install|rm|usage|crashes|abi|alarm|stress]", .func = cmd_app };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}
