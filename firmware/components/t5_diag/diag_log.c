/* SPDX-License-Identifier: Apache-2.0
 * dmesg: keep the log in a RAM ring as well as printing it, so boot messages
 * survive the USB console re-enumerating after a reset.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_console.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "t5_diag/t5_diag.h"
#include "diag_priv.h"

#define RING 16384
static char s_ring[RING];
static size_t s_head;            /* bytes ever written */
static vprintf_like_t s_prev;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static int capture(const char *fmt, va_list ap)
{
    char line[256];
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(line, sizeof line, fmt, copy);
    va_end(copy);
    if (n > 0) {
        if (n > (int)sizeof line - 1) n = sizeof line - 1;
        taskENTER_CRITICAL(&s_mux);
        for (int i = 0; i < n; i++) s_ring[(s_head + i) % RING] = line[i];
        s_head += n;
        taskEXIT_CRITICAL(&s_mux);
    }
    return s_prev ? s_prev(fmt, ap) : vprintf(fmt, ap);
}

void t5_diag_log_capture_start(void)
{
    if (!s_prev) s_prev = esp_log_set_vprintf(capture);
}

static int cmd_dmesg(int argc, char **argv)
{
    (void)argc; (void)argv;
    size_t head = s_head, n = head < RING ? head : RING;
    for (size_t i = head - n; i < head; i++) putchar(s_ring[i % RING]);
    printf("\n[%u bytes logged, %u shown]\n", (unsigned)head, (unsigned)n);
    return 0;
}

void diag_register_log(void)
{
    const esp_console_cmd_t c = { .command = "dmesg", .help = "Log since boot (RAM ring, 16 KB)", .func = cmd_dmesg };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}
