/* SPDX-License-Identifier: Apache-2.0
 * elf bench: bench check 11. Load a small ELF from SD (or /data) into PSRAM,
 * run two kernels, compare speed and results with the same code built in.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_console.h"
#include "esp_elf.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "t5_storage/t5_storage.h"
#include "diag_priv.h"
#include "elfbench/bench_kernel.h"

extern const uint8_t elfbench_start[] asm("_binary_elfbench_elf_start");
extern const uint8_t elfbench_end[] asm("_binary_elfbench_elf_end");

static __attribute__((noinline)) uint32_t builtin(uint32_t mode, uint32_t iters)
{
    return mode == 1 ? bench_small(iters) : bench_big(iters);
}

static const char *bench_path(void)
{
    return t5_storage_sd_state() == T5_SD_MOUNTED ? T5_SD_PATH "/elfbench.elf" : T5_DATA_PATH "/elfbench.elf";
}

static uint8_t *load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 0 ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static int elf_bench(uint32_t rounds)
{
    const char *path = bench_path();
    size_t elen = (size_t)(elfbench_end - elfbench_start);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(elfbench_start, 1, elen, f) != elen) {
        printf("can't write %s\n", path);
        if (f) fclose(f);
        return 1;
    }
    fclose(f);
    esp_log_level_set("ELF", ESP_LOG_WARN);
    size_t len = 0;
    uint8_t *img = load_file(path, &len);
    if (!img) { printf("can't read back %s\n", path); return 1; }
    printf("ELF %s: %u bytes\n", path, (unsigned)len);

    static const struct { uint32_t mode, iters; const char *name; } k[] = {
        { 1, 2000, "small (cached)" },
        { 2, 200, "big (64 KB code)" },
    };
    uint32_t mismatches = 0, failures = 0;
    int64_t t_elf[2] = {0}, t_bi[2] = {0};
    uint32_t want[2];
    for (int j = 0; j < 2; j++) want[j] = builtin(k[j].mode, k[j].iters);

    for (uint32_t r = 0; r < rounds; r++) {
        esp_elf_t elf;
        esp_elf_init(&elf);
        int e = esp_elf_relocate(&elf, img, len);
        if (e) { failures++; printf("relocate failed: %d\n", e); esp_elf_deinit(&elf); continue; }
        if (r == 0) {
            printf("entry %p: %s\n", (void *)elf.entry,
                   esp_ptr_external_ram((void *)elf.entry) ? "PSRAM" : "NOT in PSRAM");
        }
        for (int j = 0; j < 2; j++) {
            elfbench_arg_t a = { .mode = k[j].mode, .iters = k[j].iters };
            char *argv[1] = { (char *)&a };
            int64_t t0 = esp_timer_get_time();
            esp_elf_request(&elf, 0, 1, argv);
            int64_t t1 = esp_timer_get_time();
            uint32_t b = builtin(k[j].mode, k[j].iters);
            int64_t t2 = esp_timer_get_time();
            if (r) {   /* round 0 warms caches */
                t_elf[j] += t1 - t0;
                t_bi[j] += t2 - t1;
            }
            if (a.result != want[j] || b != want[j]) mismatches++;
        }
        esp_elf_deinit(&elf);
        vTaskDelay(1);   /* let IDLE0 feed the task watchdog */
    }
    free(img);
    for (int j = 0; j < 2; j++) {
        double re = rounds > 1 ? t_elf[j] / 1000.0 / (rounds - 1) : 0;
        double rb = rounds > 1 ? t_bi[j] / 1000.0 / (rounds - 1) : 0;
        printf("  %-18s builtin %8.2f ms   elf %8.2f ms   elf/builtin %.2fx\n", k[j].name, rb, re,
               rb > 0 ? re / rb : 0);
    }
    printf("ELF %s: %u rounds, %u relocate failures, %u result mismatches\n",
           mismatches || failures ? "FAIL" : "PASS", (unsigned)rounds, (unsigned)failures, (unsigned)mismatches);
    return mismatches || failures;
}

static int cmd_elf(int argc, char **argv)
{
    uint32_t n = 20;
    if (argc >= 2 && !strcmp(argv[1], "bench")) {
        if (argc >= 3 && !diag_parse_u32(argv[2], 1, 100000, &n)) goto usage;
        return elf_bench(n);
    }
usage:
    printf("usage: elf bench [rounds]\n");
    return 1;
}

void diag_register_elf(void)
{
    const esp_console_cmd_t c = { .command = "elf", .help = "ELF-in-PSRAM benchmark (bench 0.10)",
                                  .hint = "bench [rounds]", .func = cmd_elf };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}
