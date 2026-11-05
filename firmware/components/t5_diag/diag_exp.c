/* SPDX-License-Identifier: Apache-2.0
 * exp: expanders via the t5_xpin service (0.4, 1.3)
 */
#include <stdio.h>
#include <string.h>
#include "esp_console.h"
#include "t5_bsp/xpin.h"
#include "t5_i2c/t5_i2c.h"
#include "t5_pins/names.h"
#include "diag_priv.h"

static const char *mode_str(t5_xpin_mode_t m)
{
    return m == T5_XPIN_MODE_OUTPUT ? "output" : m == T5_XPIN_MODE_OUTPUT_HIZ ? "output hi-z" : "input";
}

static const char *pull_str(t5_xpin_pull_t p)
{
    return p == T5_XPIN_PULL_UP ? "up" : p == T5_XPIN_PULL_DOWN ? "down" : "-";
}

static int dump(void)
{
    static const uint8_t addrs[] = { T5_I2C_ADDR_EXP0, T5_I2C_ADDR_EXP1 };
    for (int e = 0; e < 2; e++) {
        printf("EXP 0x%02x (mode/out/pull from shadow; 'live' read from chip)   reapplied %u times\n", addrs[e],
               (unsigned)t5_xpin_reapply_count());
        printf("  pin  signal          mode         out  live pull\n");
        for (int p = 0; p < 8; p++) {
            t5_xpin_t x = T5_XPIN(addrs[e], p);
            bool in = false;
            t5_err_t err = t5_xpin_read(x, &in);
            const char *name = t5_bsp_xpin_name(addrs[e], p);
            t5_xpin_mode_t m = t5_xpin_mode(x);
            printf("  P%d   %-15s %-12s %-4s %-4s %s\n", p, name ? name : "(nc)", mode_str(m),
                   m == T5_XPIN_MODE_INPUT ? "-" : (t5_xpin_out_level(x) ? "1" : "0"),
                   err == T5_OK ? (in ? "1" : "0") : "?", pull_str(t5_xpin_pull(x)));
        }
    }
    return 0;
}

static bool parse_xpin(const char *a, const char *p, t5_xpin_t *out)
{
    uint32_t va, vp;
    if (!diag_parse_u32(a, 0, 0x7F, &va) || !diag_parse_u32(p, 0, 7, &vp)) return false;
    if (va == 0) va = T5_I2C_ADDR_EXP0;
    if (va == 1) va = T5_I2C_ADDR_EXP1;
    if (va != T5_I2C_ADDR_EXP0 && va != T5_I2C_ADDR_EXP1) return false;
    *out = T5_XPIN(va, vp);
    return true;
}

static int cmd_exp(int argc, char **argv)
{
    t5_xpin_t x;
    uint32_t v;
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "dump"))) return dump();
    if (argc >= 5 && !strcmp(argv[1], "out") && parse_xpin(argv[2], argv[3], &x) && diag_parse_u32(argv[4], 0, 1, &v)) {
        if (x == T5_XP_PWROFF_PULSE && v && !(argc >= 6 && !strcmp(argv[5], "--force"))) {
            printf("refusing: 0x44 P4 is PWROFF_PLUSE (schematic) and cuts power. Add --force.\n");
            return 1;
        }
        t5_err_t e = t5_xpin_output(x, v);
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
    if (argc >= 4 && !strcmp(argv[1], "in") && parse_xpin(argv[2], argv[3], &x)) {
        t5_xpin_pull_t pull = T5_XPIN_PULL_NONE;
        if (argc >= 5) pull = !strcmp(argv[4], "up") ? T5_XPIN_PULL_UP : !strcmp(argv[4], "down") ? T5_XPIN_PULL_DOWN : T5_XPIN_PULL_NONE;
        bool lvl = false;
        t5_err_t e = t5_xpin_input(x, pull);
        if (e == T5_OK) e = t5_xpin_read(x, &lvl);
        printf("%s level %d\n", t5_err_str(e), lvl);
        return e != T5_OK;
    }
    if (argc >= 5 && !strcmp(argv[1], "pulse") && parse_xpin(argv[2], argv[3], &x) && diag_parse_u32(argv[4], 1, 5000, &v)) {
        if (x == T5_XP_PWROFF_PULSE && !(argc >= 6 && !strcmp(argv[5], "--force"))) {
            printf("refusing PWROFF_PLUSE without --force\n");
            return 1;
        }
        bool lvl = !t5_xpin_out_level(x);
        t5_err_t e = t5_xpin_pulse(x, lvl, v);
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
    if (argc >= 2 && !strcmp(argv[1], "reapply")) {
        t5_err_t e = t5_xpin_reapply();
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
    printf("usage:\n  exp [dump]\n  exp out <exp> <pin> <0|1> [--force]\n  exp in <exp> <pin> [up|down]\n"
           "  exp pulse <exp> <pin> <ms> [--force]   (to the opposite level and back)\n  exp reapply\n"
           "(<exp>: 0x43 or 0x44; 0 and 1 are shorthand)\n");
    return 1;
}

void diag_register_exp(void)
{
    const esp_console_cmd_t cmd = {
        .command = "exp",
        .help = "I/O expanders 0x43/0x44 through the xpin service",
        .hint = "[dump] | out | in | pulse | reapply",
        .func = cmd_exp,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
