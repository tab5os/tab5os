/* SPDX-License-Identifier: Apache-2.0
 * Bench tools for Phase 0: gpio watch (0.6, 0.7), light sleep (0.8).
 */
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "t5_display/t5_display.h"
#include "t5_pins/pins.h"
#include "diag_priv.h"

/* GPIOs that could plausibly carry RTC/IMU/SD-detect/power-key signals and
 * that nothing in Phase 1 drives. */
static const uint8_t k_watch_default[] = {
    0, 1, 2, 3, 4, 14, 16, 17, 23, 33, 35, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54,
};

/* Edges are also appended to this file with fsync, so they survive a power cut. */
#define EDGE_LOG "/data/edges.log"

static void edge_log(bool on, const char *fmt, ...)
{
    if (!on) return;
    FILE *f = fopen(EDGE_LOG, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

static int gpio_watch(uint32_t ms, const uint8_t *pins, int n, bool log)
{
    uint8_t last[64], first[64];
    uint32_t edges[64] = {0}, first_ms[64] = {0};
    for (int i = 0; i < n; i++) {
        gpio_input_enable(pins[i]);
        last[i] = first[i] = (uint8_t)gpio_get_level(pins[i]);
    }
    printf("watching %d pins for %u ms (1 ms sampling)%s\n", n, (unsigned)ms, log ? ", logging to " EDGE_LOG : "");
    edge_log(log, "watch start, %d pins, %u ms\n", n, (unsigned)ms);
    int64_t t0 = esp_timer_get_time();
    for (uint32_t t = 0; t < ms; t++) {
        for (int i = 0; i < n; i++) {
            uint8_t v = (uint8_t)gpio_get_level(pins[i]);
            if (v != last[i]) {
                if (!edges[i]) first_ms[i] = (uint32_t)((esp_timer_get_time() - t0) / 1000);
                edges[i]++;
                last[i] = v;
                if (edges[i] <= 4) {
                    unsigned at = (unsigned)((esp_timer_get_time() - t0) / 1000);
                    printf("  G%-2u -> %u at %u ms\n", pins[i], v, at);
                    edge_log(log, "G%u -> %u at %u ms\n", pins[i], v, at);
                }
            }
        }
        vTaskDelay(1);
    }
    edge_log(log, "watch end\n");
    printf("pin  start  end  edges  first-edge\n");
    for (int i = 0; i < n; i++) {
        printf("G%-3u %-6u %-4u %-6u %s%u\n", pins[i], first[i], last[i], (unsigned)edges[i],
               edges[i] ? "" : "-", edges[i] ? (unsigned)first_ms[i] : 0u);
    }
    return 0;
}

static int cmd_gpio(int argc, char **argv)
{
    uint32_t v, ms;
    if (argc >= 3 && !strcmp(argv[1], "watch") && diag_parse_u32(argv[2], 1, 600000, &ms)) {
        uint8_t pins[64];
        int n = 0;
        bool log = false;
        for (int i = 3; i < argc && n < 64; i++) {
            if (!strcmp(argv[i], "log")) { log = true; continue; }
            if (!diag_parse_u32(argv[i], 0, 54, &v)) goto usage;
            pins[n++] = (uint8_t)v;
        }
        if (!n) {
            memcpy(pins, k_watch_default, sizeof k_watch_default);
            n = sizeof k_watch_default;
        }
        return gpio_watch(ms, pins, n, log);
    }
    if (argc >= 3 && !strcmp(argv[1], "get") && diag_parse_u32(argv[2], 0, 54, &v)) {
        gpio_input_enable(v);
        printf("G%u = %d\n", (unsigned)v, gpio_get_level(v));
        return 0;
    }
usage:
    printf("usage: gpio watch <ms> [pins...] [log] | gpio get <pin>\n  log: also append edges to " EDGE_LOG " with fsync\n");
    return 1;
}

/* ---- light sleep (0.8) -------------------------------------------------- */
static char s_sleep_report[512];

static const char *cause_str(uint32_t causes)
{
    if (causes & BIT(ESP_SLEEP_WAKEUP_GPIO)) return "gpio";
    if (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) return "timer";
    if (causes & BIT(ESP_SLEEP_WAKEUP_EXT1)) return "ext1";
    if (causes & BIT(ESP_SLEEP_WAKEUP_UART)) return "uart";
    return "other";
}

static int light_sleep(uint32_t ms, uint32_t mb, const uint8_t *wpins, int nw)
{
    size_t len = (size_t)mb << 20;
    uint32_t *buf = len ? heap_caps_malloc(len, MALLOC_CAP_SPIRAM) : NULL;
    if (len && !buf) {
        printf("no PSRAM for %u MB\n", (unsigned)mb);
        return 1;
    }
    for (size_t i = 0; i < len / 4; i++) buf[i] = (uint32_t)(i * 2654435761u);

    char armed[96] = "";
    const uint8_t *pins = wpins;
    if (nw) {
        /* Wake on the level opposite to the idle level, so an idle pin can't wake us at once. */
        for (int i = 0; i < nw; i++) {
            /* Keep the pad's active config (pulls, input) during sleep instead of
             * IDF's default isolation: G35 is shared with the power controller. */
            gpio_sleep_sel_dis(pins[i]);
            gpio_input_enable(pins[i]);
            /* gpio_wakeup_enable() turns the pin's interrupt into a level one. A
             * pin with a live ISR (touch on G23) would then fire non-stop while
             * held low and starve the system, so mask the ISR during sleep. */
            gpio_intr_disable(pins[i]);
            int idle = gpio_get_level(pins[i]);
            gpio_wakeup_enable(pins[i], idle ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
            char one[24];
            snprintf(one, sizeof one, " G%u:%s", pins[i], idle ? "low" : "high");
            strncat(armed, one, sizeof armed - strlen(armed) - 1);
        }
        esp_sleep_enable_gpio_wakeup();
    }
    esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000);
    printf("light sleep %u ms, %u MB pattern, wake on timer%s. USB console will drop; run 'sleep result' after.\n",
           (unsigned)ms, (unsigned)mb, armed);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));

    int64_t t0 = esp_timer_get_time();
    esp_err_t e = esp_light_sleep_start();
    int64_t t1 = esp_timer_get_time();
    uint32_t cause = esp_sleep_get_wakeup_causes();
    uint64_t gpio_mask = (cause & BIT(ESP_SLEEP_WAKEUP_GPIO)) ? esp_sleep_get_gpio_wakeup_status() : 0;

    size_t bad = 0;
    for (size_t i = 0; i < len / 4; i++) if (buf[i] != (uint32_t)(i * 2654435761u)) bad++;
    heap_caps_free(buf);
    for (int i = 0; i < nw; i++) {
        gpio_wakeup_disable(pins[i]);   /* also sets the interrupt type to none */
        gpio_sleep_sel_en(pins[i]);
        if (pins[i] == T5_PIN_TOUCH_INT) {
            /* give the touch service its falling-edge interrupt back */
            gpio_set_intr_type(pins[i], GPIO_INTR_NEGEDGE);
            gpio_intr_enable(pins[i]);
        }
    }
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    uint8_t id[3] = {0}, pm = 0;
    t5_err_t de = t5_display_ready() ? t5_display_read_ids(id, &pm) : T5_ERR_STATE;
    snprintf(s_sleep_report, sizeof s_sleep_report,
             "SLEEP %s: slept %lld ms (asked %u), wake=%s gpio-mask=0x%llx, psram %u MB %s (%u bad words), "
             "panel read %s power-mode 0x%02x",
             e == ESP_OK ? "ok" : esp_err_to_name(e), (long long)((t1 - t0) / 1000), (unsigned)ms,
             cause_str(cause), (unsigned long long)gpio_mask, (unsigned)mb, bad ? "CORRUPT" : "intact",
             (unsigned)bad, t5_err_str(de), pm);
    printf("%s\n", s_sleep_report);
    return bad != 0 || e != ESP_OK;
}

static int cmd_sleep(int argc, char **argv)
{
    uint32_t ms, mb = 8;
    if (argc >= 2 && !strcmp(argv[1], "result")) {
        printf("%s\n", s_sleep_report[0] ? s_sleep_report : "no sleep yet");
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "light") && diag_parse_u32(argv[2], 10, 600000, &ms)) {
        uint8_t wp[8];
        int nw = 0;
        uint32_t pin;
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "gpio")) {
                wp[0] = T5_PIN_TOUCH_INT; wp[1] = T5_PIN_PWR_KEY; wp[2] = T5_PIN_C6_WAKE; nw = 3;
            } else if (argv[i][0] == 'g' && nw < 8 && diag_parse_u32(argv[i] + 1, 0, 54, &pin)) {
                wp[nw++] = (uint8_t)pin;
            } else if (!diag_parse_u32(argv[i], 0, 24, &mb)) {
                goto usage;
            }
        }
        return light_sleep(ms, mb, wp, nw);
    }
usage:
    printf("usage: sleep light <ms> [MB] [gpio | g<pin>...] | sleep result\n"
           "  gpio: wake on touch INT (G23), power key (G35), C6 wake (G14); g23 g14: pick pins\n"
           "  WARNING: arming G35 resets the board on Unit A (bench 0.8)\n");
    return 1;
}

void diag_register_bench(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "gpio", .help = "Watch GPIO levels and edges", .hint = "watch <ms> [pins] | get <pin>", .func = cmd_gpio },
        { .command = "sleep", .help = "Light sleep test (bench 0.8)", .hint = "light <ms> [MB] [gpio] | result", .func = cmd_sleep },
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
}
