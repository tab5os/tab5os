/* SPDX-License-Identifier: Apache-2.0
 * prof: a sampling profiler for core 1 (the UI core). A core 0 task reads
 * core 1's program counter from the assist-debug block every few
 * microseconds; `prof dump` prints the hottest addresses (tools/t5 maps
 * them to functions with addr2line).
 *   prof start [ms] [delay_ms]   sample for ms (2000) after delay_ms (0)
 *   prof dump [n]                the n (400) most frequent PCs
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/assist_debug_ll.h"
#include "soc/assist_debug_reg.h"
#include "diag_priv.h"

#define MAX_SAMPLES 200000

static uint32_t *s_pc;
static volatile uint32_t s_n;
static volatile bool s_busy;
static uint32_t s_ms, s_delay;

static void prof_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(s_delay));
    assist_debug_ll_enable_pc_recording(1, true);
    int64_t end = esp_timer_get_time() + (int64_t)s_ms * 1000;
    uint32_t n = 0;
    /* Busy: core 0 is given over to sampling; yield briefly every 10 ms so
     * its watchdog and the PPA/ISR work there keep going. */
    int64_t yield_at = esp_timer_get_time() + 10000;
    while (n < MAX_SAMPLES && esp_timer_get_time() < end) {
        s_pc[n++] = REG_READ(ASSIST_DEBUG_CORE_1_RCD_PDEBUGPC_REG);
        esp_rom_delay_us(10);
        if (esp_timer_get_time() >= yield_at) {
            vTaskDelay(1);
            yield_at = esp_timer_get_time() + 10000;
        }
    }
    s_n = n;
    s_busy = false;
    vTaskDelete(NULL);
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

typedef struct { uint32_t pc, n; } hit_t;

static int cmp_hit(const void *a, const void *b)
{
    const hit_t *x = a, *y = b;
    return x->n < y->n ? 1 : x->n > y->n ? -1 : 0;
}

static int cmd_prof(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "start")) {
        if (s_busy) { printf("busy\n"); return 1; }
        if (!s_pc) s_pc = heap_caps_malloc(MAX_SAMPLES * sizeof *s_pc, MALLOC_CAP_SPIRAM);
        if (!s_pc) { printf("no memory\n"); return 1; }
        s_ms = argc >= 3 ? (uint32_t)atoi(argv[2]) : 2000;
        s_delay = argc >= 4 ? (uint32_t)atoi(argv[3]) : 0;
        s_n = 0;
        s_busy = true;
        xTaskCreatePinnedToCore(prof_task, "prof", 4096, NULL, 20, NULL, 0);
        printf("sampling core 1 for %u ms after %u ms\n", (unsigned)s_ms, (unsigned)s_delay);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "dump")) {
        if (s_busy || !s_n) { printf("no samples%s\n", s_busy ? " yet" : ""); return 1; }
        uint32_t top = argc >= 3 ? (uint32_t)atoi(argv[2]) : 400;
        uint32_t n = s_n;
        qsort(s_pc, n, sizeof *s_pc, cmp_u32);
        hit_t *h = heap_caps_malloc(n * sizeof *h, MALLOC_CAP_SPIRAM);
        if (!h) return 1;
        uint32_t u = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (u && h[u - 1].pc == s_pc[i]) h[u - 1].n++;
            else h[u++] = (hit_t){ s_pc[i], 1 };
        }
        qsort(h, u, sizeof *h, cmp_hit);
        printf("prof samples %u unique %u\n", (unsigned)n, (unsigned)u);
        for (uint32_t i = 0; i < u && i < top; i++) printf("P %08x %u\n", (unsigned)h[i].pc, (unsigned)h[i].n);
        printf("prof end\n");
        free(h);
        s_n = 0;
        return 0;
    }
    printf("usage: prof start [ms] [delay_ms] | dump [n]\n");
    return 1;
}

void diag_register_prof(void)
{
    const esp_console_cmd_t c = { .command = "prof", .help = "Sampling profiler for core 1 (UI)",
                                  .hint = "start [ms] [delay] | dump [n]", .func = cmd_prof };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}
