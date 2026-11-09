/* SPDX-License-Identifier: Apache-2.0
 * ota [file PATH [unsigned] | url URL [unsigned] | selftest [SIG KEYID] | recovery | safemode]
 * (plan 9.1-9.3)
 */
#include <stdio.h>
#include <string.h>
#include "esp_console.h"
#include "t5_ota/ota.h"
#include "t5_ota_net/ota_url.h"
#include "diag_priv.h"

typedef struct {
    int argc;
    char **argv;
} ota_args_t;

static int ota_job(void *arg)
{
    int argc = ((ota_args_t *)arg)->argc;
    char **argv = ((ota_args_t *)arg)->argv;
    char err[128] = "";
    t5_err_t e = T5_OK;
    bool unsigned_ok = argc >= 4 && !strcmp(argv[3], "unsigned");
    if (argc >= 3 && !strcmp(argv[1], "file")) e = t5_ota_install_file(argv[2], T5_OTA_FROM_CONSOLE, unsigned_ok, err, sizeof err);
    else if (argc >= 3 && !strcmp(argv[1], "url")) e = t5_ota_install_url(argv[2], T5_OTA_FROM_CONSOLE, unsigned_ok, err, sizeof err);
    else if (argc >= 2 && !strcmp(argv[1], "selftest")) {
        /* ota selftest [SIG128HEX KEYID64HEX] */
        uint8_t sig[64], kid[32];
        bool signed_test = argc >= 4 && strlen(argv[2]) == 128 && strlen(argv[3]) == 64;
        for (int i = 0; signed_test && i < 64; i++) sscanf(argv[2] + 2 * i, "%2hhx", &sig[i]);
        for (int i = 0; signed_test && i < 32; i++) sscanf(argv[3] + 2 * i, "%2hhx", &kid[i]);
        e = t5_ota_selftest(signed_test ? sig : NULL, signed_test ? kid : NULL, err, sizeof err);
    } else if (argc >= 2 && !strcmp(argv[1], "recovery")) e = t5_ota_reboot_to_recovery();
    else if (argc >= 2 && !strcmp(argv[1], "safemode")) e = t5_ota_reboot_safe_mode();
    else if (argc >= 2) {
        printf("usage: ota [file PATH [unsigned] | url URL [unsigned] | selftest [SIG KEYID] | recovery | safemode]\n");
        return 1;
    }
    if (argc >= 2) printf("%s%s%s\n", t5_err_str(e), err[0] ? ": " : "", err);
    t5_ota_status_t st;
    t5_ota_status(&st);
    printf("update %s %s %u/%u%s%s, %s, %d failed boots before this one, safe mode %s\n", t5_ota_state_name(st.state),
           st.version,
           (unsigned)st.done, (unsigned)st.total, st.error[0] ? ": " : "", st.error,
           t5_ota_on_trial() ? "running on trial" : "confirmed", t5_ota_failed_boots(), t5_safe_mode() ? "on" : "off");
    return e != T5_OK;
}

static int cmd_ota(int argc, char **argv)
{
    /* Writing a slot needs an internal stack; a URL install also does TLS. */
    ota_args_t a = { argc, argv };
    return diag_run_internal("t5_otacmd", 12288, ota_job, &a);
}

void diag_register_ota(void)
{
    const esp_console_cmd_t c = {
        .command = "ota",
        .help = "Updates: file PATH | url URL [unsigned] | selftest | recovery | safemode",
        .func = cmd_ota,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}
