/* SPDX-License-Identifier: Apache-2.0
 * Boot health, safe mode and restarts into recovery (plan 9.2, 9.3): see
 * ota.h and bootctl.h.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_bootfmt/bootctl.h"
#include "t5_ota/ota.h"
#include "t5_storage/paths.h"
#include "ota_priv.h"

static const char *TAG = "t5.ota";

#define TRIAL_MS (60 * 1000)
#define RESTART_LOG_MS 200           /* the log line out before the reset */

static bool s_trial, s_safe;
static int s_failed;
static t5_boot_req_t s_req;

/* Where the bootloader keeps the record: the app partition of subtype
 * test (bootloader_state_t.test). */
static const esp_partition_t *ctl_part(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_TEST, NULL);
}

static t5_err_t ctl_request(const char *req)
{
    const esp_partition_t *p = ctl_part();
    if (!p) return T5_ERR_NOT_FOUND;
    size_t off = p->size - T5_BOOTCTL_SECTOR_FROM_END;
    if (esp_partition_erase_range(p, off, T5_BOOTCTL_SECTOR_FROM_END) != ESP_OK) return T5_ERR_IO;
    if (req && esp_partition_write(p, off + T5_BOOTCTL_REQ_OFFSET, req, T5_BOOTCTL_REQ_LEN) != ESP_OK) {
        return T5_ERR_IO;
    }
    return T5_OK;
}

void t5_ota_boot_check(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        s_trial = true;
        T5_LOGW(TAG, "this OS was just installed: on trial until it has run %d s", TRIAL_MS / 1000);
    }
    const esp_partition_t *p = ctl_part();
    uint8_t rec[T5_BOOTCTL_REC_LEN];
    if (!p || esp_partition_read(p, p->size - T5_BOOTCTL_SECTOR_FROM_END, rec, sizeof rec) != ESP_OK) return;
    s_req = t5_bootctl_request(rec);
    s_failed = t5_bootctl_failed_before(rec, esp_reset_reason() == ESP_RST_DEEPSLEEP);
    if (s_failed) T5_LOGW(TAG, "%d unconfirmed boot%s before this one", s_failed, s_failed > 1 ? "s" : "");
    if (!ota_running_from_slot()) return;          /* recovery: safe mode is the OS's */
    bool asked = s_req == T5_BOOT_REQ_SAFEMODE;
    s_safe = t5_bootctl_safe_mode(s_req, s_failed);
    if (s_safe) T5_LOGW(TAG, "safe mode: %s", asked ? "requested" : "after failed boots");
    if (asked) T5_LOG_ON_ERR(TAG, ctl_request(NULL));       /* once */
}

static esp_timer_handle_t s_trial_timer;

static void trial_passed(void *arg)
{
    (void)arg;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        s_trial = false;
        T5_LOGI(TAG, "update confirmed: this OS is now the one that boots");
    }
}

void t5_ota_boot_ok(void)
{
    static bool done;
    if (done) return;
    done = true;
    T5_LOG_ON_ERR(TAG, ctl_request(NULL));        /* healthy: the counter starts again */
    if (!s_trial) {
        /* Images flashed over USB have no trial; this is a no-op for them. */
        esp_ota_mark_app_valid_cancel_rollback();
        return;
    }
    const esp_timer_create_args_t a = { .callback = trial_passed, .name = "ota_trial" };
    if (esp_timer_create(&a, &s_trial_timer) == ESP_OK) esp_timer_start_once(s_trial_timer, (uint64_t)TRIAL_MS * 1000);
}

static void (*s_restart)(void);

void t5_ota_set_restart_hook(void (*fn)(void))
{
    s_restart = fn;
}

static t5_err_t reboot_with(const char *req)
{
    T5_RETURN_ON_ERR(ctl_request(req));
    T5_LOGI(TAG, "restarting: %s", req);
    if (s_restart) s_restart();
    t5_delay_ms(RESTART_LOG_MS);
    esp_restart();
    return T5_OK;
}

t5_err_t t5_ota_check_sd_flag(void)
{
    static const char *const flags[] = { T5_SD_PATH "/RECOVERY", T5_SD_PATH "/recovery.txt",
                                         T5_SD_PATH "/tab5/recovery" };
    for (size_t i = 0; i < sizeof flags / sizeof flags[0]; i++) {
        struct stat st;
        if (stat(flags[i], &st) != 0) continue;
        /* Once: the flag goes before the restart, so recovery's "restart"
         * comes back here. */
        char done[64];
        snprintf(done, sizeof done, "%s.done", flags[i]);
        remove(done);
        rename(flags[i], done);
        T5_LOGW(TAG, "%s on the SD card: restarting into recovery", flags[i]);
        return reboot_with(T5_BOOTCTL_REQ_RECOVERY);
    }
    return T5_OK;
}

void t5_ota_bootctl_clear(void)
{
    T5_LOG_ON_ERR(TAG, ctl_request(NULL));
}

bool t5_ota_on_trial(void)
{
    return s_trial;
}

int t5_ota_failed_boots(void)
{
    return s_failed;
}

t5_boot_req_t t5_ota_boot_request(void)
{
    return s_req;
}

bool t5_safe_mode(void)
{
    return s_safe;
}

t5_err_t t5_ota_reboot_to_recovery(void)
{
    return reboot_with(T5_BOOTCTL_REQ_RECOVERY);
}

t5_err_t t5_ota_reboot_safe_mode(void)
{
    return reboot_with(T5_BOOTCTL_REQ_SAFEMODE);
}
