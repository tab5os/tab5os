/* SPDX-License-Identifier: Apache-2.0
 * dmesg: the log ring (t5_core/logring.h), so boot messages survive the USB
 * console re-enumerating after a reset.
 */
#include <stdio.h>
#include "esp_console.h"
#include "t5_core/logring.h"
#include "diag_priv.h"

static int cmd_dmesg(int argc, char **argv)
{
    (void)argc; (void)argv;
    uint64_t pos = 0, head = t5_log_head();
    char buf[256];
    size_t n, shown = 0;
    while ((n = t5_log_read(&pos, buf, sizeof buf)) > 0) {
        fwrite(buf, 1, n, stdout);
        shown += n;
        if (pos >= head) break;
    }
    printf("\n[%u bytes logged, %u shown]\n", (unsigned)head, (unsigned)shown);
    return 0;
}

void diag_register_log(void)
{
    const esp_console_cmd_t c = { .command = "dmesg", .help = "Log since boot (RAM ring, 16 KB)", .func = cmd_dmesg };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}
