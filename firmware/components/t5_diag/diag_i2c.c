/* SPDX-License-Identifier: Apache-2.0
 * i2c: scan, raw register access, stats, trace, recovery, stress (0.3, 1.2, 1.10)
 */
#include <stdio.h>
#include <string.h>
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "t5_core/os.h"
#include "t5_i2c/t5_i2c.h"
#include "t5_pins/names.h"
#include "t5_pins/pins.h"
#include "diag_priv.h"

static int i2c_scan(void)
{
    uint8_t found[128];
    int n = 0;
    printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
    for (int row = 0; row < 128; row += 16) {
        printf("%02x: ", row);
        for (int col = 0; col < 16; col++) {
            int a = row + col;
            if (a < 0x08 || a > 0x77) {
                printf("   ");
                continue;
            }
            t5_err_t err = t5_i2c_probe(a);
            if (err == T5_OK) {
                printf("%02x ", a);
                found[n++] = a;
            } else {
                printf(err == T5_ERR_TIMEOUT ? "TO " : "-- ");
            }
        }
        printf("\n");
    }
    printf("\n%d device(s) on G%d/G%d:\n", n, T5_PIN_SYS_I2C_SDA, T5_PIN_SYS_I2C_SCL);
    for (int i = 0; i < n; i++) {
        const char *name = t5_bsp_i2c_name(found[i]);
        printf("  0x%02x  %s\n", found[i], name ? name : "UNKNOWN");
    }
    return 0;
}

static int with_dev(uint32_t addr, t5_i2c_dev_t **d)
{
    t5_err_t err = t5_i2c_open((uint8_t)addr, 400000, "console", d);
    if (err != T5_OK) {
        printf("open 0x%02x: %s\n", (unsigned)addr, t5_err_str(err));
        return 1;
    }
    return 0;
}

static void hexdump(uint32_t reg, const uint8_t *buf, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        printf("%s0x%04x: 0x%02x", i % 8 ? "  " : (i ? "\n" : ""), (unsigned)(reg + i), buf[i]);
    }
    printf("\n");
}

static int i2c_stats(void)
{
    t5_i2c_bus_stats_t b;
    t5_i2c_bus_stats(&b);
    printf("bus: %u xfers, %u errors, %u recoveries (%u failed), %u lock timeouts, %u hand-offs\n",
           (unsigned)b.xfers, (unsigned)b.errors, (unsigned)b.recoveries,
           (unsigned)b.recovery_failures, (unsigned)b.lock_timeouts, (unsigned)t5_i2c_handoffs());
    t5_i2c_dev_stats_t d[T5_I2C_MAX_DEVICES];
    int n = t5_i2c_dev_stats(d, T5_I2C_MAX_DEVICES);
    printf("addr  name          xfers    errors  nack   timeout  state\n");
    for (int i = 0; i < n && i < T5_I2C_MAX_DEVICES; i++) {
        printf("0x%02x  %-12s  %-8u %-7u %-6u %-8u %s\n", d[i].addr, d[i].name, (unsigned)d[i].xfers,
               (unsigned)d[i].errors, (unsigned)d[i].nacks, (unsigned)d[i].timeouts,
               d[i].faulted ? "FAULTED" : "ok");
    }
    return 0;
}

static int i2c_trace(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[2], "on")) { t5_i2c_trace_enable(true); return 0; }
    if (argc >= 3 && !strcmp(argv[2], "off")) { t5_i2c_trace_enable(false); return 0; }
    static t5_i2c_trace_t t[T5_I2C_TRACE_LEN];
    int n = t5_i2c_trace_read(t, T5_I2C_TRACE_LEN);
    for (int i = 0; i < n; i++) {
        printf("%8u ms  0x%02x  tx %3u [%02x..]  rx %3u  %s\n", (unsigned)t[i].t_ms, t[i].addr, t[i].txlen,
               t[i].first, t[i].rxlen, t5_err_str((t5_err_t)t[i].err));
    }
    printf("%d entries\n", n);
    return 0;
}

/* ---- stress (1.10) ------------------------------------------------------- */
typedef struct {
    const char *name;
    uint8_t addr;
    uint8_t reg;
    uint8_t len;
    uint8_t mask;       /* compare (byte0 & mask) against want */
    uint8_t want;
    volatile uint32_t ok, err, bad;
    volatile bool run;
} stress_t;

static stress_t s_stress[] = {
    { .name = "exp0.id", .addr = T5_I2C_ADDR_EXP0, .reg = 0x01, .len = 1, .mask = 0xE0, .want = 0xA0 },
    { .name = "exp1.id", .addr = T5_I2C_ADDR_EXP1, .reg = 0x01, .len = 1, .mask = 0xE0, .want = 0xA0 },
    { .name = "imu.id", .addr = T5_I2C_ADDR_BMI270, .reg = 0x00, .len = 1, .mask = 0xFF, .want = 0x24 },
    { .name = "ina.mfr", .addr = T5_I2C_ADDR_INA226, .reg = 0xFE, .len = 2, .mask = 0xFF, .want = 0x54 },   /* "TI" = 0x5449 */
    { .name = "rtc.time", .addr = T5_I2C_ADDR_RX8130, .reg = 0x10, .len = 7, .mask = 0x80, .want = 0x00 },  /* seconds bit7 always 0 */
};
#define N_STRESS (sizeof s_stress / sizeof s_stress[0])
static volatile int s_stress_done;

static void stress_task(void *arg)
{
    stress_t *s = arg;
    t5_i2c_dev_t *d;
    if (t5_i2c_open(s->addr, 400000, s->name, &d) != T5_OK) {
        s->run = false;
    }
    uint8_t buf[8];
    while (s->run) {
        t5_err_t e = t5_i2c_read(d, s->reg, buf, s->len);
        if (e != T5_OK) s->err++;
        else if ((buf[0] & s->mask) != s->want) s->bad++;
        else s->ok++;
        if ((s->ok & 63) == 0) taskYIELD();
    }
    t5_i2c_close(d);
    s_stress_done++;
    vTaskDelete(NULL);
}

static int i2c_stress(uint32_t secs)
{
    t5_i2c_bus_stats_t b0, b1;
    t5_i2c_bus_stats(&b0);
    s_stress_done = 0;
    for (size_t i = 0; i < N_STRESS; i++) {
        s_stress[i].ok = s_stress[i].err = s_stress[i].bad = 0;
        s_stress[i].run = true;
        xTaskCreatePinnedToCore(stress_task, "i2c_stress", 3072, &s_stress[i], 3, NULL, i % 2);
    }
    printf("stress: %u tasks for %u s (Ctrl-C not supported; wait)\n", (unsigned)N_STRESS, (unsigned)secs);
    for (uint32_t t = 1; t <= secs; t++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (t % 10 == 0 || t == secs) {
            uint32_t ok = 0, err = 0, bad = 0;
            for (size_t i = 0; i < N_STRESS; i++) { ok += s_stress[i].ok; err += s_stress[i].err; bad += s_stress[i].bad; }
            t5_i2c_bus_stats(&b1);
            printf("[%4u s] ok %u  err %u  bad-data %u  recoveries %u\n", (unsigned)t, (unsigned)ok,
                   (unsigned)err, (unsigned)bad, (unsigned)(b1.recoveries - b0.recoveries));
        }
    }
    for (size_t i = 0; i < N_STRESS; i++) s_stress[i].run = false;
    while (s_stress_done < (int)N_STRESS) vTaskDelay(pdMS_TO_TICKS(10));
    uint32_t tot_err = 0, tot_bad = 0;
    for (size_t i = 0; i < N_STRESS; i++) {
        printf("  %-9s ok %-9u err %-6u bad %u\n", s_stress[i].name, (unsigned)s_stress[i].ok,
               (unsigned)s_stress[i].err, (unsigned)s_stress[i].bad);
        tot_err += s_stress[i].err;
        tot_bad += s_stress[i].bad;
    }
    t5_i2c_bus_stats(&b1);
    uint32_t lt = b1.lock_timeouts - b0.lock_timeouts;
    bool pass = tot_bad == 0 && lt == 0;
    printf("STRESS %s: errors %u, bad data %u, recoveries %u, lock timeouts %u\n",
           pass ? "PASS" : "FAIL", (unsigned)tot_err, (unsigned)tot_bad,
           (unsigned)(b1.recoveries - b0.recoveries), (unsigned)lt);
    return !pass;
}

static int cmd_i2c(int argc, char **argv)
{
    uint32_t addr, reg, v, n = 1;
    if (argc >= 2 && !strcmp(argv[1], "scan")) return i2c_scan();
    if (argc >= 2 && !strcmp(argv[1], "stats")) return i2c_stats();
    if (argc >= 2 && !strcmp(argv[1], "trace")) return i2c_trace(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "recover")) {
        t5_err_t e = t5_i2c_recover();
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
    if (argc >= 2 && !strcmp(argv[1], "stress")) {
        uint32_t secs = 60;
        if (argc >= 3 && !diag_parse_u32(argv[2], 1, 86400, &secs)) goto usage;
        return i2c_stress(secs);
    }
    bool r16 = argc >= 2 && !strcmp(argv[1], "r16");
    if (argc >= 4 && (r16 || !strcmp(argv[1], "r")) && diag_parse_u32(argv[2], 0x08, 0x77, &addr) &&
        diag_parse_u32(argv[3], 0, r16 ? 0xFFFF : 0xFF, &reg)) {
        if (argc >= 5 && !diag_parse_u32(argv[4], 1, 64, &n)) goto usage;
        t5_i2c_dev_t *d;
        if (with_dev(addr, &d)) return 1;
        uint8_t buf[64];
        t5_err_t e = r16 ? t5_i2c_read16(d, (uint16_t)reg, buf, n) : t5_i2c_read(d, (uint8_t)reg, buf, n);
        t5_i2c_close(d);
        if (e != T5_OK) { printf("read failed: %s\n", t5_err_str(e)); return 1; }
        hexdump(reg, buf, n);
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "w") && diag_parse_u32(argv[2], 0x08, 0x77, &addr) &&
        diag_parse_u32(argv[3], 0, 0xFF, &reg)) {
        uint8_t buf[16];
        int k = 0;
        for (int i = 4; i < argc && k < 16; i++) {
            if (!diag_parse_u32(argv[i], 0, 0xFF, &v)) goto usage;
            buf[k++] = (uint8_t)v;
        }
        t5_i2c_dev_t *d;
        if (with_dev(addr, &d)) return 1;
        t5_err_t e = t5_i2c_write(d, (uint8_t)reg, buf, k);
        t5_i2c_close(d);
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
usage:
    printf("usage:\n  i2c scan | stats | recover | trace [on|off]\n  i2c r <addr> <reg> [n]\n"
           "  i2c r16 <addr> <reg16> [n]\n  i2c w <addr> <reg> <byte>...\n  i2c stress [seconds]\n");
    return 1;
}

void diag_register_i2c(void)
{
    const esp_console_cmd_t cmd = {
        .command = "i2c",
        .help = "System I2C bus: scan, registers, stats, trace, recover, stress",
        .hint = "scan | stats | trace | recover | r | r16 | w | stress",
        .func = cmd_i2c,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
