/* SPDX-License-Identifier: Apache-2.0
 * ota [file PATH [unsigned] | url URL [unsigned] | selftest [SIG KEYID] | recovery | safemode |
 *      check | online [install] | c6 [file PATH [unsigned] | url URL [unsigned] | retry]]
 * (plan 9.1-9.3)
 */
#include <stdio.h>
#include <string.h>
#include "esp_console.h"
#include "t5_ota/ota.h"
#include "t5_core/os.h"
#include "t5_net/download.h"
#include "t5_ota_net/c6.h"
#include "t5_ota_net/ota_url.h"
#include "t5_ota_net/update.h"
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

/* ota check: check the releases now and wait for the answer (up to 30 s);
 * ota online: what the last check found; ota online install: install it,
 * as Settings does (signed by a built-in key, not older than this). */
static int cmd_online(int argc, char **argv)
{
    t5_err_t e = T5_OK;
    if (!strcmp(argv[1], "check")) {
        e = t5_update_check_now();
        t5_update_status_t u;
        for (int i = 0; e == T5_OK && i < 300; i++) {
            t5_update_status(&u);
            if (u.state != T5_UPDATE_CHECKING) break;
            t5_delay_ms(100);
        }
    } else if (argc >= 3 && !strcmp(argv[2], "install")) {
        e = t5_update_install();
    }
    if (e != T5_OK) printf("%s\n", t5_err_str(e));
    static const char *const names[] = { "not checked", "checking", "up to date", "available", "installing", "failed" };
    t5_update_status_t u;
    t5_update_status(&u);
    printf("online %s%s%s%s%s, checked %lld s ago\n", names[u.state], u.version[0] ? " " : "", u.version,
           u.error[0] ? ": " : "", u.error, u.checked_us ? (long long)((t5_time_us() - u.checked_us) / 1000000) : -1LL);
    return e != T5_OK;
}

/* ota c6: where the C6 sync is; ota c6 retry: run it again; ota c6 file|url:
 * write a .t5c6 (any version) to c6fw, for `net update` to put on the C6. */
static int c6_job(void *arg)
{
    int argc = ((ota_args_t *)arg)->argc;
    char **argv = ((ota_args_t *)arg)->argv;
    char err[128] = "";
    t5_err_t e = T5_OK;
    bool unsigned_ok = argc >= 5 && !strcmp(argv[4], "unsigned");
    if (argc >= 4 && !strcmp(argv[2], "file")) {
        e = t5_ota_c6_install_file(argv[3], T5_OTA_FROM_CONSOLE, unsigned_ok, NULL, err, sizeof err);
    } else if (argc >= 4 && !strcmp(argv[2], "url")) {
        t5_download_t *dl;
        e = t5_download_open(argv[3], &dl, NULL, err, sizeof err);
        if (e == T5_OK) {
            e = t5_ota_c6_install_stream(t5_download_read, dl, T5_OTA_FROM_CONSOLE, unsigned_ok, NULL, err, sizeof err);
            t5_download_close(dl);
        }
    } else if (argc >= 3 && !strcmp(argv[2], "retry")) {
        e = t5_c6_sync_retry();
    } else if (argc >= 3) {
        printf("usage: ota c6 [file PATH [unsigned] | url URL [unsigned] | retry]\n");
        return 1;
    }
    if (argc >= 3) printf("%s%s%s\n", t5_err_str(e), err[0] ? ": " : "", err);
    if (e == T5_OK && argc >= 4) printf("in c6fw: 'net update' puts it on the C6\n");
    char st[128];
    t5_c6_sync_status(st, sizeof st);
    printf("c6 sync: %s\n", st);
    return e != T5_OK;
}

static int cmd_ota(int argc, char **argv)
{
    if (argc >= 2 && (!strcmp(argv[1], "check") || !strcmp(argv[1], "online"))) return cmd_online(argc, argv);
    /* Writing a slot needs an internal stack; a URL install also does TLS. */
    ota_args_t a = { argc, argv };
    if (argc >= 2 && !strcmp(argv[1], "c6")) return diag_run_internal("t5_otacmd", 12288, c6_job, &a);
    return diag_run_internal("t5_otacmd", 12288, ota_job, &a);
}

void diag_register_ota(void)
{
    const esp_console_cmd_t c = {
        .command = "ota",
        .help = "Updates: file PATH | url URL [unsigned] | selftest | recovery | safemode | check | online [install] | c6 [file|url|retry]",
        .func = cmd_ota,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}
