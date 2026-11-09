/* SPDX-License-Identifier: Apache-2.0
 * devmode [on|off|forget]: developer mode (plan 8.5).
 */
#include <stdio.h>
#include <string.h>
#include "esp_console.h"
#include "t5_dev/dev.h"
#include "diag_priv.h"

static int cmd_devmode(int argc, char **argv)
{
    if (argc >= 2 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "off"))) {
        t5_err_t e = t5_dev_set_enabled(!strcmp(argv[1], "on"));
        if (e != T5_OK) printf("%s\n", t5_err_str(e));
    } else if (argc >= 2 && !strcmp(argv[1], "forget")) {
        t5_err_t fe = t5_dev_forget_all();
        if (fe != T5_OK) printf("%s\n", t5_err_str(fe));
    }
    t5_dev_status_t st;
    t5_dev_status(&st);
    printf("developer mode %s, %s on port %u, %d paired, %u requests, %u refused%s\n", st.enabled ? "on" : "off",
           st.listening ? "listening" : "not listening", st.port, st.clients, (unsigned)st.requests,
           (unsigned)st.denied, st.pairing ? ", pairing code on screen" : "");
    return 0;
}

void diag_register_devmode(void)
{
    const esp_console_cmd_t c = {
        .command = "devmode",
        .help = "Developer mode: on | off | forget (paired computers)",
        .func = cmd_devmode,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}
