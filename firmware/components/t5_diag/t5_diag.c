/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include "esp_console.h"
#include "esp_check.h"
#include "diag_priv.h"
#include "t5_diag/t5_diag.h"

static const char *TAG = "t5_diag";

bool diag_parse_u32(const char *s, uint32_t min, uint32_t max, uint32_t *out)
{
    if (!s || !*s) {
        return false;
    }
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (*end != '\0' || v < min || v > max) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

esp_err_t t5_diag_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "tab5> ";
    repl_cfg.max_cmdline_length = 256;
    repl_cfg.task_stack_size = 12288;   /* scan tables and BLE results live on the stack */

    esp_console_dev_usb_serial_jtag_config_t dev_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl),
                        TAG, "repl");

    diag_print_boot_banner();
    ESP_RETURN_ON_ERROR(esp_console_register_help_command(), TAG, "help");
    diag_register_sys();
    diag_register_i2c();
    diag_register_exp();
    diag_register_dev();
    diag_register_bench();
    diag_register_log();
    diag_register_elf();
    diag_register_power();
    diag_register_net();
    diag_register_ports();
    diag_register_ui();
    diag_register_app();

    return esp_console_start_repl(repl);
}
