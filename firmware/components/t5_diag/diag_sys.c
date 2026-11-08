/* SPDX-License-Identifier: Apache-2.0
 * sys, tasks, psram, reboot (bench item 0.2)
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_console.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_private/esp_clk.h"
#include "hal/efuse_hal.h"
#include "sdkconfig.h"
#include "t5_core/os.h"
#include "diag_priv.h"

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB peripheral";
    case ESP_RST_JTAG:      return "JTAG";
    case ESP_RST_EFUSE:     return "efuse error";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU lockup";
    default:                return "unknown";
    }
}

void diag_print_boot_banner(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    unsigned rev = efuse_hal_chip_revision();
    const esp_partition_t *run = esp_ota_get_running_partition();
    printf("\nTab5 OS %s  (IDF %s, variant %s)\n", app->version, esp_get_idf_version(),
#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3
           "rev1"
#else
           "rev3"
#endif
          );
    printf("chip ESP32-P4 v%u.%u  cpu %d MHz  psram %u MB  reset: %s  slot: %s\n",
           rev / 100, rev % 100, esp_clk_cpu_freq() / 1000000,
           (unsigned)(esp_psram_get_size() >> 20), reset_reason_str(esp_reset_reason()),
           run ? run->label : "?");
    printf("Type 'help' for commands.\n\n");
}

static int cmd_sys(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    unsigned rev = efuse_hal_chip_revision();
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BASE);
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();

    printf("app        %s %s (built %s %s)\n", app->project_name, app->version, app->date, app->time);
    printf("idf        %s\n", esp_get_idf_version());
    printf("chip       ESP32-P4 v%u.%u, %d cores, min rev in image v%d.%d\n", rev / 100, rev % 100,
           ci.cores, CONFIG_ESP_REV_MIN_FULL / 100, CONFIG_ESP_REV_MIN_FULL % 100);
    printf("base mac   %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    printf("cpu        %d MHz\n", esp_clk_cpu_freq() / 1000000);
    printf("flash      %" PRIu32 " MB\n", flash >> 20);
    printf("psram      %u MB @ %d MHz\n", (unsigned)(esp_psram_get_size() >> 20),
#if CONFIG_SPIRAM_SPEED_200M
           200
#elif CONFIG_SPIRAM_SPEED_80M
           80
#else
           20
#endif
          );
    printf("reset      %s\n", reset_reason_str(esp_reset_reason()));
    printf("partition  %s @ 0x%" PRIx32 " (%" PRIu32 " KB)\n", run ? run->label : "?",
           run ? run->address : 0, run ? run->size >> 10 : 0);
    printf("uptime     %.1f s\n", esp_timer_get_time() / 1e6);
    printf("heap int   free %u  largest %u  min-ever %u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    /* "int" includes LP RAM and TCM, which DMA (Wi-Fi, TLS, camera) can't use. */
    printf("heap dma   free %u  largest %u  min-ever %u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    printf("heap lp    free %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_RTCRAM));
    printf("heap psram free %u  largest %u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    return 0;
}

static int cmd_tasks(int argc, char **argv)
{
    (void)argc; (void)argv;
    const size_t sz = 4096;
    char *buf = malloc(sz);
    if (!buf) {
        return 1;
    }
    printf("Name            State Prio Stack#  Core\n");
    vTaskList(buf);
    fputs(buf, stdout);
    printf("\nName            Abs time       %%time\n");
    vTaskGetRunTimeStats(buf);
    fputs(buf, stdout);
    free(buf);
    return 0;
}

/* Per-task stacks: how big, how much was ever left, and in which memory.
 * What sizes the stacks and shows who holds internal RAM. */
static const char *stack_mem(const void *p)
{
    if (esp_ptr_external_ram(p)) return "psram";
    if (esp_ptr_in_rtc_dram_fast(p) || heap_caps_get_allocated_size((void *)p) == 0) return "lp";
    return esp_ptr_dma_capable(p) ? "L2" : "lp";
}

static int cmd_stacks(int argc, char **argv)
{
    (void)argc; (void)argv;
    UBaseType_t cap = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *st = heap_caps_malloc(cap * sizeof *st, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!st) return 1;
    UBaseType_t n = uxTaskGetSystemState(st, cap, NULL);
    size_t l2 = 0, lp = 0, psram = 0;
    printf("%-16s %-5s %4s %7s %7s %5s\n", "task", "mem", "prio", "stack", "unused", "used%");
    for (UBaseType_t i = 0; i < n; i++) {
        const void *base = st[i].pxStackBase;
        if (base) base = (const uint8_t *)base - t5_task_margin_of((t5_task_t)st[i].xHandle);
        size_t size = base ? heap_caps_get_allocated_size((void *)base) : 0;
        size_t unused = st[i].usStackHighWaterMark;
        const char *m = base ? stack_mem(base) : "?";
        if (!strcmp(m, "L2")) l2 += size;
        else if (!strcmp(m, "lp")) lp += size;
        else if (!strcmp(m, "psram")) psram += size;
        printf("%-16s %-5s %4u %7u %7u %4u%%\n", st[i].pcTaskName, m, (unsigned)st[i].uxCurrentPriority, (unsigned)size,
               (unsigned)unused, size ? (unsigned)(100 - 100 * unused / size) : 0);
    }
    printf("%u tasks, stacks: L2 %u  lp %u  psram %u\n", (unsigned)n, (unsigned)l2, (unsigned)lp, (unsigned)psram);
    heap_caps_free(st);
    return 0;
}

/* l2eat KB | 0: hold internal RAM to test the low-memory path (isolate.h). */
#define EAT_MAX 256
static void *s_eat[EAT_MAX];

static int cmd_l2eat(int argc, char **argv)
{
    uint32_t kb = 0;
    if (argc != 2 || !diag_parse_u32(argv[1], 0, 1024, &kb)) {
        printf("usage: l2eat KB (0 frees)\n");
        return 1;
    }
    for (int i = 0; i < EAT_MAX; i++) {
        heap_caps_free(s_eat[i]);
        s_eat[i] = NULL;
    }
    uint32_t got = 0;
    for (int i = 0; i < EAT_MAX && got < kb; i++) {
        s_eat[i] = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (!s_eat[i]) break;
        got += 4;
    }
    printf("holding %u KB; L2 free %u\n", (unsigned)got, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    return 0;
}

static inline uint32_t xorshift32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

/* psram [MB]: pattern test plus rough bandwidth figures. */
static int cmd_psram(int argc, char **argv)
{
    uint32_t mb = 8;
    if (argc > 1 && !diag_parse_u32(argv[1], 1, 30, &mb)) {
        printf("usage: psram [MB 1-30]\n");
        return 1;
    }
    size_t len = (size_t)mb << 20;
    uint32_t *buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        printf("can't allocate %" PRIu32 " MB (largest block %u)\n", mb,
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        return 1;
    }
    size_t words = len / 4;
    int64_t t0, t1;

    /* Bandwidth: memset (write), memcpy half to half (read + write) */
    t0 = esp_timer_get_time();
    memset(buf, 0xA5, len);
    t1 = esp_timer_get_time();
    double w_mbs = (double)len / (t1 - t0);
    t0 = esp_timer_get_time();
    memcpy(buf, (uint8_t *)buf + len / 2, len / 2);
    t1 = esp_timer_get_time();
    double c_mbs = (double)(len / 2) / (t1 - t0);

    /* Pattern: two passes with different seeds so stuck bits show up. */
    size_t errors = 0;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t seed = pass ? 0xDEADBEEF : 0x12345678;
        uint32_t s = seed;
        for (size_t i = 0; i < words; i++) {
            buf[i] = xorshift32(&s);
        }
        s = seed;
        for (size_t i = 0; i < words; i++) {
            uint32_t want = xorshift32(&s);
            if (buf[i] != want) {
                if (errors < 8) {
                    printf("  mismatch @%p: got %08" PRIx32 " want %08" PRIx32 "\n",
                           (void *)&buf[i], buf[i], want);
                }
                errors++;
            }
        }
        vTaskDelay(1); /* let the idle task feed the watchdog */
    }
    heap_caps_free(buf);
    printf("psram %" PRIu32 " MB: %s (%u bad words)  write %.0f MB/s  copy %.0f MB/s\n",
           mb, errors ? "FAIL" : "ok", (unsigned)errors, w_mbs, c_mbs);
    return errors ? 1 : 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_restart();
    return 0;
}

void diag_register_sys(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "sys", .help = "Chip, clocks, memory, partition, reset reason", .func = cmd_sys },
        { .command = "tasks", .help = "FreeRTOS task list and CPU use", .func = cmd_tasks },
        { .command = "l2eat", .help = "l2eat KB: hold internal RAM (low-memory test), 0 frees", .hint = "KB", .func = cmd_l2eat },
        { .command = "stacks", .help = "Each task's stack: size, memory, high-water mark", .func = cmd_stacks },
        { .command = "psram", .help = "psram [MB]: pattern test and bandwidth", .hint = "[MB]", .func = cmd_psram },
        { .command = "reboot", .help = "Software reset", .func = cmd_reboot },
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
}
