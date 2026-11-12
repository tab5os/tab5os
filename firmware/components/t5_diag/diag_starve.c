/* SPDX-License-Identifier: Apache-2.0
 * starve: find what keeps a task from running.
 *
 * A probe task at a chosen priority (default 5, an app's player task) sleeps
 * 10 ms at a time, unpinned, and times each sleep with esp_timer. A
 * hardware timer interrupt samples, every millisecond, the task running on
 * each core into a ring. When a sleep overshoots by more than the
 * threshold, the probe prints which tasks held each core meanwhile, and how
 * far the tick count moved (a stopped tick means a core sat with its
 * interrupts off). Runs in the background: starve <s> [prio] [ms], starve stop.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gptimer.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "diag_priv.h"

#define RING 4096                /* samples, 1 ms apart: 4 s */
#define TOP 5

typedef struct {
    TaskHandle_t cur[2];
} sample_t;

static sample_t *s_ring;
static volatile uint32_t s_n;   /* samples taken */
static gptimer_handle_t s_tmr;
static TaskHandle_t s_probe;
static volatile bool s_stop;
static uint32_t s_secs, s_prio, s_thresh_ms;

static bool IRAM_ATTR on_alarm(gptimer_handle_t t, const gptimer_alarm_event_data_t *d, void *u)
{
    (void)t; (void)d; (void)u;
    sample_t *s = &s_ring[s_n % RING];
    s->cur[0] = xTaskGetCurrentTaskHandleForCore(0);
    s->cur[1] = xTaskGetCurrentTaskHandleForCore(1);
    s_n++;
    return false;
}

static void report(uint32_t from, uint32_t to, int64_t late_us, TickType_t ticks, uint32_t expect_ticks)
{
    if (to - from > RING) from = to - RING;
    printf("starve: probe (prio %u) %lld ms late; ticks moved %u of %u expected; %u samples\n", (unsigned)s_prio,
           late_us / 1000, (unsigned)ticks, (unsigned)expect_ticks, (unsigned)(to - from));
    for (int c = 0; c < 2; c++) {
        TaskHandle_t h[TOP * 4] = { 0 };
        uint32_t k[TOP * 4] = { 0 };
        int nh = 0;
        for (uint32_t i = from; i < to; i++) {
            TaskHandle_t x = s_ring[i % RING].cur[c];
            int j = 0;
            while (j < nh && h[j] != x) j++;
            if (j == nh) {
                if (nh == TOP * 4) continue;
                h[nh++] = x;
            }
            k[j]++;
        }
        printf("  core %d:", c);
        for (int r = 0; r < TOP && r < nh; r++) {
            int b = 0;
            for (int j = 1; j < nh; j++) if (k[j] > k[b]) b = j;
            if (!k[b]) break;
            printf(" %s %u%%", h[b] ? pcTaskGetName(h[b]) : "?", (unsigned)(k[b] * 100 / (to - from ? to - from : 1)));
            k[b] = 0;
        }
        printf("\n");
    }
}

static void probe(void *arg)
{
    (void)arg;
    uint32_t stalls = 0;
    int64_t worst = 0, end = esp_timer_get_time() + (int64_t)s_secs * 1000000;
    while (!s_stop && esp_timer_get_time() < end) {
        uint32_t n0 = s_n;
        TickType_t k0 = xTaskGetTickCount();
        int64_t t0 = esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS(10));
        int64_t late = esp_timer_get_time() - t0 - 10000;
        if (late > worst) worst = late;
        if (late > (int64_t)s_thresh_ms * 1000) {
            stalls++;
            report(n0, s_n, late, xTaskGetTickCount() - k0, (uint32_t)((late + 10000) / 1000));
        }
    }
    printf("starve: done, %u stalls over %u ms, worst %lld ms late\n", (unsigned)stalls, (unsigned)s_thresh_ms,
           worst / 1000);
    gptimer_stop(s_tmr);
    gptimer_disable(s_tmr);
    gptimer_del_timer(s_tmr);
    s_tmr = NULL;
    heap_caps_free(s_ring);
    s_ring = NULL;
    s_probe = NULL;
    vTaskDelete(NULL);
}

/* The timer's interrupt goes to the core that sets it up: core 1, so it
 * samples even while core 0 is busy with the radio and audio. */
static void setup(void *arg)
{
    TaskHandle_t caller = arg;
    gptimer_config_t cfg = { .clk_src = GPTIMER_CLK_SRC_DEFAULT, .direction = GPTIMER_COUNT_UP,
                             .resolution_hz = 1000000 };
    bool ok = gptimer_new_timer(&cfg, &s_tmr) == ESP_OK;
    gptimer_event_callbacks_t cbs = { .on_alarm = on_alarm };
    gptimer_alarm_config_t al = { .alarm_count = 1000, .reload_count = 0, .flags.auto_reload_on_alarm = true };
    ok = ok && gptimer_register_event_callbacks(s_tmr, &cbs, NULL) == ESP_OK &&
         gptimer_set_alarm_action(s_tmr, &al) == ESP_OK && gptimer_enable(s_tmr) == ESP_OK &&
         gptimer_start(s_tmr) == ESP_OK;
    if (!ok && s_tmr) {
        gptimer_del_timer(s_tmr);
        s_tmr = NULL;
    }
    xTaskNotifyGive(caller);
    vTaskDelete(NULL);
}

static int cmd_starve(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "stop")) {
        s_stop = true;
        return 0;
    }
    uint32_t secs, prio = 5, ms = 150;
    if (argc < 2 || !diag_parse_u32(argv[1], 1, 7200, &secs) || (argc > 2 && !diag_parse_u32(argv[2], 1, 20, &prio)) ||
        (argc > 3 && !diag_parse_u32(argv[3], 20, 10000, &ms))) {
        printf("usage: starve <s> [prio] [ms] | starve stop\n");
        return 1;
    }
    if (s_probe) {
        printf("already running\n");
        return 1;
    }
    s_ring = heap_caps_calloc(RING, sizeof *s_ring, MALLOC_CAP_SPIRAM);
    if (!s_ring) return 1;
    s_n = 0;
    s_stop = false;
    s_secs = secs;
    s_prio = prio;
    s_thresh_ms = ms;
    xTaskCreatePinnedToCore(setup, "starve.set", 3072, xTaskGetCurrentTaskHandle(), 20, NULL, 1);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    if (!s_tmr) {
        printf("no timer\n");
        heap_caps_free(s_ring);
        s_ring = NULL;
        return 1;
    }
    if (xTaskCreate(probe, "starve", 4096, NULL, prio, &s_probe) != pdPASS) {
        s_stop = true;
        return 1;
    }
    printf("starve: probing for %u s at prio %u, reporting stalls over %u ms\n", (unsigned)secs, (unsigned)prio,
           (unsigned)ms);
    return 0;
}

void diag_register_starve(void)
{
    const esp_console_cmd_t c = { .command = "starve", .help = "Find what keeps a task from running",
                                  .hint = "<s> [prio] [ms] | stop", .func = cmd_starve };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}
