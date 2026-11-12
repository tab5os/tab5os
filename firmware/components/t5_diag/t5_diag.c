/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include "esp_console.h"
#include "sdkconfig.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "t5_core/os.h"
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
    t5_task_stacks_in_psram(true);   /* 12 KB, mostly unused, for the life of the OS */
    esp_err_t repl_err = esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl);
    t5_task_stacks_in_psram(false);
    ESP_RETURN_ON_ERROR(repl_err, TAG, "repl");

    diag_print_boot_banner();
    ESP_RETURN_ON_ERROR(esp_console_register_help_command(), TAG, "help");
    diag_register_sys();
    diag_register_i2c();
    diag_register_exp();
    diag_register_dev();
#if CONFIG_T5_DIAG_BENCH
    diag_register_bench();
    diag_register_elf();
#endif
    diag_register_log();
    diag_register_power();
    diag_register_net();
    diag_register_ports();
    diag_register_ui();
    diag_register_app();
    diag_register_media();
    diag_register_prof();
    diag_register_py();
    diag_register_ota();
    diag_register_devmode();

    return esp_console_start_repl(repl);
}

typedef struct {
    int (*fn)(void *arg);
    void *arg;
    int rc;
    SemaphoreHandle_t done;
    StaticSemaphore_t done_buf;
} internal_job_t;

static void internal_task(void *arg)
{
    internal_job_t *j = arg;
    j->rc = j->fn(j->arg);
    xSemaphoreGive(j->done);
}

int diag_run_internal(const char *name, uint32_t stack, int (*fn)(void *arg), void *arg)
{
    internal_job_t j = { .fn = fn, .arg = arg, .rc = 1 };
    j.done = xSemaphoreCreateBinaryStatic(&j.done_buf);
    if (t5_task_create(name, internal_task, &j, stack, 5, 0, NULL) != T5_OK) {
        printf("can't start %s\n", name);
        return 1;
    }
    xSemaphoreTake(j.done, portMAX_DELAY);
    return j.rc;
}
