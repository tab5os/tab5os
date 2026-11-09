/* SPDX-License-Identifier: Apache-2.0
 * port: external ports, claims, raw I/O, module scan, Modbus (plan 5.1-5.3).
 * The console opens ports as "system" and keeps its handles here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_console.h"
#include "t5_ports/modbus.h"
#include "t5_ports/ports.h"
#include "t5_ports/profiles.h"
#include "t5_storage/paths.h"
#include "diag_priv.h"

#define MAX_OPEN 8

static t5_port_t *s_open[MAX_OPEN];

static t5_port_t *find(const char *name, int mode)
{
    for (int i = 0; i < MAX_OPEN; i++) {
        const char *n = t5_port_name(s_open[i]);
        if (n && !strcmp(n, name) && (mode < 0 || (int)t5_port_mode(s_open[i]) == mode)) {
            return s_open[i];
        }
    }
    return NULL;
}

static void forget(t5_port_t *p)
{
    for (int i = 0; i < MAX_OPEN; i++) {
        if (s_open[i] == p) s_open[i] = NULL;
    }
}

static void list(void)
{
    printf("port         modes                         pins\n");
    for (int i = 0; i < t5_ports_count(); i++) {
        t5_port_info_t pi;
        if (t5_ports_info(i, &pi) != T5_OK) break;
        char modes[48] = "";
        for (int m = 0; m < T5_MODE_COUNT; m++) {
            if (pi.modes & (1u << m)) {
                strlcat(modes, t5_port_mode_name((t5_port_mode_t)m), sizeof modes);
                strlcat(modes, " ", sizeof modes);
            }
        }
        printf("%-12s %-29s", pi.name, modes);
        for (int j = 0; j < pi.npins; j++) {
            printf(" G%u:%s%s", pi.gpio[j], pi.label[j], (pi.risky_mask >> j) & 1 ? "!" : "");
        }
        printf("\n");
    }
    printf("\npin  level  owner                    port         mode\n");
    uint8_t g[64];
    int n = t5_ports_pins(g, 64);
    for (int i = 0; i < n && i < 64; i++) {
        t5_pin_info_t p;
        if (t5_ports_pin_info(g[i], &p) != T5_OK) continue;
        printf("G%-3u %-6d %-24s %-12s %s%s\n", p.gpio, p.level, p.owner[0] ? p.owner : "-",
               p.port ? p.port : "-", p.port ? t5_port_mode_name(p.mode) : "-", p.risky ? "  (risky)" : "");
    }
}

static bool u32(const char *s, uint32_t min, uint32_t max, uint32_t *out)
{
    return diag_parse_u32(s, min, max, out);
}

/* port open <name> <mode> [args] [risky] */
static int do_open(int argc, char **argv)
{
    const char *name = argv[2];
    t5_port_mode_t mode = t5_port_mode_parse(argv[3]);
    uint32_t flags = 0;
    if (!strcmp(argv[argc - 1], "risky")) {
        flags |= T5_PORT_FLAG_RISKY;
        argc--;
    }
    int slot = -1;
    for (int i = 0; i < MAX_OPEN && slot < 0; i++) {
        if (!t5_port_name(s_open[i])) slot = i;
    }
    if (slot < 0) {
        printf("console has %d ports open; close one first\n", MAX_OPEN);
        return 1;
    }
    uint32_t a = 0, b = 0, c = 0;
    bool has_a = argc > 4 && u32(argv[4], 0, 80000000, &a);
    bool has_b = argc > 5 && u32(argv[5], 0, 80000000, &b);
    bool has_c = argc > 6 && u32(argv[6], 0, 16, &c);
    union {
        t5_port_i2c_cfg_t i2c;
        t5_port_uart_cfg_t uart;
        t5_port_spi_cfg_t spi;
        t5_port_gpio_cfg_t gpio;
        t5_port_pwm_cfg_t pwm;
        t5_port_adc_cfg_t adc;
    } cfg;
    memset(&cfg, 0, sizeof cfg);
    switch (mode) {
    case T5_MODE_I2C: cfg.i2c.hz = has_a ? a : 0; break;
    case T5_MODE_UART:
    case T5_MODE_RS485: cfg.uart.baud = has_a ? a : 0; break;
    case T5_MODE_SPI:
        cfg.spi = (t5_port_spi_cfg_t){ .hz = has_a ? a : 0, .mode = has_b ? (uint8_t)b : 0,
                                       .cs_pin_index = has_c ? (int)c : -1 };
        break;
    case T5_MODE_GPIO:
        cfg.gpio.pin_index = has_a ? (uint8_t)a : 0;
        cfg.gpio.output = argc > 5 && !strcmp(argv[5], "out");
        cfg.gpio.pull_up = argc > 5 && !strcmp(argv[5], "pullup");
        break;
    case T5_MODE_PWM:
        cfg.pwm = (t5_port_pwm_cfg_t){ .pin_index = has_a ? (uint8_t)a : 0, .hz = has_b ? b : 0,
                                       .resolution_bits = has_c ? (uint8_t)c : 0 };
        break;
    case T5_MODE_ADC: cfg.adc.pin_index = has_a ? (uint8_t)a : 0; break;
    default:
        printf("unknown mode %s\n", argv[3]);
        return 1;
    }
    char why[T5_PORT_WHY_LEN];
    t5_err_t e = t5_port_open_ex(name, mode, &cfg, flags, &s_open[slot], why, sizeof why);
    printf("%s%s%s\n", t5_err_str(e), why[0] ? ": " : "", why);
    return e != T5_OK;
}

static int do_i2c(int argc, char **argv)
{
    /* port i2c scan|r|w <name> ... */
    if (argc < 4) return -1;
    const char *op = argv[2], *name = argv[3];
    t5_port_t *p = find(name, T5_MODE_I2C), *tmp = NULL;
    if (!p) {
        char why[T5_PORT_WHY_LEN];
        t5_err_t e = t5_port_open_ex(name, T5_MODE_I2C, NULL, 0, &tmp, why, sizeof why);
        if (e != T5_OK) {
            printf("%s: %s\n", t5_err_str(e), why);
            return 1;
        }
        p = tmp;
    }
    int rc = 0;
    uint32_t addr, reg, len = 1;
    if (!strcmp(op, "scan")) {
        uint8_t found[112];
        int n = t5_port_i2c_scan(p, found, sizeof found);
        for (int i = 0; i < n && i < (int)sizeof found; i++) printf("0x%02x ", found[i]);
        printf("\n%d device(s) on %s\n", n, name);
    } else if (!strcmp(op, "r") && argc >= 6 && u32(argv[4], 0x08, 0x77, &addr) && u32(argv[5], 0, 255, &reg) &&
               (argc < 7 || u32(argv[6], 1, 64, &len))) {
        uint8_t buf[64];
        t5_err_t e = t5_port_i2c_read_reg(p, (uint8_t)addr, (uint8_t)reg, buf, len);
        if (e == T5_OK) {
            for (uint32_t i = 0; i < len; i++) printf("%02x%s", buf[i], (i + 1) % 16 ? " " : "\n");
            printf("\n");
        } else {
            printf("%s\n", t5_err_str(e));
            rc = 1;
        }
    } else if (!strcmp(op, "w") && argc >= 7 && u32(argv[4], 0x08, 0x77, &addr) && u32(argv[5], 0, 255, &reg)) {
        uint8_t buf[32];
        int n = 0;
        for (int i = 6; i < argc && n < (int)sizeof buf; i++) {
            uint32_t v;
            if (!u32(argv[i], 0, 255, &v)) { rc = -1; break; }
            buf[n++] = (uint8_t)v;
        }
        if (rc == 0) {
            t5_err_t e = t5_port_i2c_write_reg(p, (uint8_t)addr, (uint8_t)reg, buf, (size_t)n);
            printf("%s\n", t5_err_str(e));
            rc = e != T5_OK;
        }
    } else {
        rc = -1;
    }
    T5_IGNORE(t5_port_close(tmp));   /* opened just above */
    return rc;
}

static int do_scan(const char *name)
{
    uint8_t found[112];
    int n = t5_profiles_scan(name, found, sizeof found);
    if (n < 0) {
        printf("%s (close the port first if the console holds it)\n", t5_err_str((t5_err_t)n));
        return 1;
    }
    printf("found:");
    for (int i = 0; i < n && i < (int)sizeof found; i++) printf(" 0x%02x", found[i]);
    printf("\n");
    t5_profile_match_t m[8];
    int k = t5_profiles_suggest(name, found, n, m, 8);
    for (int i = 0; i < k; i++) {
        const t5_profile_t *p = m[i].profile;
        printf("  %-16s %u/%u %s%s%s  %s\n", p->name, m[i].matched, p->naddrs, m[i].complete ? "complete" : "partial",
               m[i].port_ok ? "" : ", other port", p->builtin ? ", best-effort" : "", p->notes);
        if (p->power & T5_PROFILE_POWER_EXT5V) printf("  %-16s needs ext5v: rail on ext5v\n", "");
    }
    if (!k) printf("no matching profile\n");
    return 0;
}

static int do_modbus(int argc, char **argv)
{
    /* port modbus <slave> <fn> <addr> <count|value> */
    uint32_t slave, fn, addr, arg;
    if (argc < 6 || !u32(argv[2], 0, 247, &slave) || !u32(argv[3], 1, 0x10, &fn) || !u32(argv[4], 0, 0xFFFF, &addr) ||
        !u32(argv[5], 0, 0xFFFF, &arg)) {
        return -1;
    }
    t5_port_t *p = find("rs485", -1), *tmp = NULL;
    if (!p) {
        t5_port_uart_cfg_t uc = { .baud = 9600 };
        t5_err_t e = t5_port_open("rs485", T5_MODE_RS485, &uc, &tmp);
        if (e != T5_OK) {
            printf("rs485: %s\n", t5_err_str(e));
            return 1;
        }
        p = tmp;
        printf("(rs485 at 9600 8N1; 'port open rs485 rs485 <baud>' to choose)\n");
    }
    t5_modbus_t mb;
    t5_modbus_init(&mb, p);
    t5_err_t e;
    uint16_t regs[T5_MODBUS_MAX_READ];
    switch (fn) {
    case 0x01:
        e = (arg >= 1 && arg <= 1000) ? t5_modbus_read_coils(&mb, (uint8_t)slave, (uint16_t)addr, (uint16_t)arg, (uint8_t *)regs)
                                      : T5_ERR_INVALID_ARG;
        if (e == T5_OK) {
            for (uint32_t i = 0; i < arg; i++) printf("%d", (((uint8_t *)regs)[i / 8] >> (i % 8)) & 1);
            printf("\n");
        }
        break;
    case 0x03:
    case 0x04:
        e = (arg >= 1 && arg <= T5_MODBUS_MAX_READ)
                ? (fn == 3 ? t5_modbus_read_holding : t5_modbus_read_input)(&mb, (uint8_t)slave, (uint16_t)addr,
                                                                            (uint16_t)arg, regs)
                : T5_ERR_INVALID_ARG;
        for (uint32_t i = 0; e == T5_OK && i < arg; i++) printf("%5u: 0x%04x %u\n", (unsigned)(addr + i), regs[i], regs[i]);
        break;
    case 0x05: e = t5_modbus_write_coil(&mb, (uint8_t)slave, (uint16_t)addr, arg != 0); break;
    case 0x06: e = t5_modbus_write_register(&mb, (uint8_t)slave, (uint16_t)addr, (uint16_t)arg); break;
    case 0x10: regs[0] = (uint16_t)arg; e = t5_modbus_write_registers(&mb, (uint8_t)slave, (uint16_t)addr, regs, 1); break;
    default: e = T5_ERR_NOT_SUPPORTED; break;
    }
    if (mb.exception) printf("exception 0x%02x: ", mb.exception);
    printf("%s\n", t5_err_str(e));
    T5_IGNORE(t5_port_close(tmp));   /* opened just above */
    return e != T5_OK;
}

static int cmd_port(int argc, char **argv)
{
    if (argc == 1) {
        list();
        return 0;
    }
    const char *c = argv[1];
    int rc = -1;
    if (!strcmp(c, "open") && argc >= 4) {
        rc = do_open(argc, argv);
    } else if (!strcmp(c, "close") && argc >= 3) {
        int n = 0;
        t5_port_t *p;
        while ((p = find(argv[2], -1)) != NULL) {
            forget(p);
            T5_IGNORE(t5_port_close(p));   /* found open just above */
            n++;
        }
        printf("closed %d\n", n);
        rc = 0;
    } else if (!strcmp(c, "i2c")) {
        rc = do_i2c(argc, argv);
    } else if (!strcmp(c, "gpio") && argc >= 3) {
        t5_port_t *p = find(argv[2], T5_MODE_GPIO);
        bool lv;
        t5_err_t e = !p ? T5_ERR_STATE : argc >= 4 ? t5_port_gpio_set(p, atoi(argv[3]) != 0) : t5_port_gpio_get(p, &lv);
        if (e == T5_OK && argc < 4) printf("%d\n", lv);
        else printf("%s%s\n", t5_err_str(e), p ? "" : " (open it in gpio mode first)");
        rc = e != T5_OK;
    } else if (!strcmp(c, "pwm") && argc >= 4) {
        t5_port_t *p = find(argv[2], T5_MODE_PWM);
        t5_err_t e = p ? t5_port_pwm_set(p, (uint32_t)atoi(argv[3])) : T5_ERR_STATE;
        printf("%s\n", t5_err_str(e));
        rc = e != T5_OK;
    } else if (!strcmp(c, "adc") && argc >= 3) {
        t5_port_t *p = find(argv[2], T5_MODE_ADC);
        int mv = 0;
        t5_err_t e = p ? t5_port_adc_read_mv(p, &mv) : T5_ERR_STATE;
        if (e == T5_OK) printf("%d mV\n", mv);
        else printf("%s%s\n", t5_err_str(e), p ? "" : " (open it in adc mode first)");
        rc = e != T5_OK;
    } else if (!strcmp(c, "uart") && argc >= 4) {
        t5_port_t *p = find(argv[2], -1);
        if (!p || !t5_port_uart_baud(p)) {
            printf("open %s in uart or rs485 mode first\n", argv[2]);
            return 1;
        }
        if (!strcmp(argv[3], "w") && argc >= 5) {
            for (int i = 4; i < argc; i++) {
                t5_port_uart_write(p, argv[i], strlen(argv[i]));
                t5_port_uart_write(p, i + 1 < argc ? " " : "\r\n", i + 1 < argc ? 1 : 2);
            }
            rc = 0;
        } else if (!strcmp(argv[3], "r")) {
            uint32_t ms = 500;
            if (argc >= 5) u32(argv[4], 0, 60000, &ms);
            char buf[129];
            int n = t5_port_uart_read(p, buf, sizeof buf - 1, ms);
            buf[n > 0 ? n : 0] = '\0';
            printf("%d bytes: %s\n", n, buf);
            rc = 0;
        }
    } else if (!strcmp(c, "scan") && argc >= 3) {
        rc = do_scan(argv[2]);
    } else if (!strcmp(c, "profiles")) {
        int added = t5_profiles_reload();
        for (int i = 0; i < t5_profiles_count(); i++) {
            const t5_profile_t *p = t5_profiles_get(i);
            printf("%-16s %-6s", p->name, t5_port_mode_name(p->bus));
            for (int a = 0; a < p->naddrs; a++) printf(" 0x%02x", p->addrs[a]);
            printf("%s\n", p->builtin ? "  (built-in)" : "");
        }
        printf("%d loaded from " T5_PROFILES_DATA_DIR " and " T5_PROFILES_SD_DIR "\n", added);
        rc = 0;
    } else if (!strcmp(c, "modbus")) {
        rc = do_modbus(argc, argv);
    }
    if (rc < 0) {
        printf("usage: port | port open <name> <mode> [args] [risky] | port close <name>\n"
               "       port i2c scan <name> | port i2c r <name> <addr> <reg> [len] | port i2c w <name> <addr> <reg> <b>...\n"
               "       port gpio <name> [0|1] | port pwm <name> <permille> | port adc <name>\n"
               "       port uart <name> w <text> | port uart <name> r [ms]\n"
               "       port scan <name> | port profiles | port modbus <slave> <fn> <addr> <count|value>\n"
               "open args: i2c [hz] | uart|rs485 [baud] | spi [hz] [mode] [cs_index]\n"
               "           gpio <pin_index> [in|out|pullup] | pwm <pin_index> [hz] [bits] | adc <pin_index>\n");
        return 1;
    }
    return rc;
}

void diag_register_ports(void)
{
    const esp_console_cmd_t cmd = {
        .command = "port",
        .help = "External ports: claims, I/O, module scan, Modbus",
        .hint = "[open|close|i2c|gpio|pwm|adc|uart|scan|profiles|modbus]",
        .func = cmd_port,
    };
    ESP_ERROR_CHECK(t5_ports_init() == T5_OK ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
