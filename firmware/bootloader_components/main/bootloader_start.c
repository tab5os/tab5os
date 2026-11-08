/* SPDX-License-Identifier: Apache-2.0
 * Tab5 OS second-stage bootloader: IDF's, with the boot-attempt counter
 * (t5_ota/bootctl.h) deciding between the OS and the recovery image.
 */
#include <stdbool.h>
#include <string.h>
#include <sys/reent.h>
#include "sdkconfig.h"
#include "bootloader_common.h"
#include "bootloader_config.h"
#include "bootloader_flash_priv.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "soc/reset_reasons.h"
#include "t5_ota/bootctl.h"

static const char *TAG = "t5boot";

static bool recovery_valid(const bootloader_state_t *bs)
{
    if (bs->test.size == 0) return false;
    esp_image_metadata_t meta = {0};
    return esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &bs->test, &meta) == ESP_OK;
}

/* Count this boot; returns the index to boot. */
static int choose(bootloader_state_t *bs)
{
    int normal = bootloader_utility_get_selected_boot_partition(bs);
    if (bs->test.size < 2 * T5_BOOTCTL_SECTOR_FROM_END) return normal;
    uint32_t addr = bs->test.offset + bs->test.size - T5_BOOTCTL_SECTOR_FROM_END;
    uint8_t rec[T5_BOOTCTL_ATTEMPT_BYTES + T5_BOOTCTL_REQ_LEN];
    if (bootloader_flash_read(addr, rec, sizeof rec, false) != ESP_OK) return normal;

    bool want_recovery = !memcmp(rec + T5_BOOTCTL_REQ_OFFSET, T5_BOOTCTL_REQ_RECOVERY, T5_BOOTCTL_REQ_LEN);
    int failed = t5_bootctl_count(rec);
    soc_reset_reason_t why = esp_rom_get_reset_reason(0);
    if (!want_recovery && why != RESET_REASON_CORE_DEEP_SLEEP) {
        /* Count this attempt: clear the lowest set bit. */
        for (int i = 0; i < T5_BOOTCTL_ATTEMPT_BYTES; i++) {
            if (rec[i]) {
                /* Flash writes are whole aligned words: rewrite the word
                 * with one more bit cleared (the rest programs as itself). */
                int w = i & ~3;
                uint32_t word;
                memcpy(&word, rec + w, 4);
                uint8_t *wb = (uint8_t *)&word;
                wb[i - w] = rec[i] & (uint8_t)(rec[i] - 1);
                if (bootloader_flash_write(addr + (uint32_t)w, &word, 4, false) != ESP_OK) {
                    ESP_LOGE(TAG, "can't count this boot");
                }
                break;
            }
        }
    }
    if (want_recovery || failed >= T5_BOOTCTL_RECOVERY_AFTER) {
        if (recovery_valid(bs)) {
            ESP_LOGW(TAG, "%s: starting recovery", want_recovery ? "requested" : "three failed boots");
            return TEST_APP_INDEX;
        }
        ESP_LOGE(TAG, "no recovery image: starting the OS anyway");
        bootloader_flash_erase_sector(addr / 0x1000u);
    } else if (failed) {
        ESP_LOGW(TAG, "%d unconfirmed boot%s before this one", failed, failed > 1 ? "s" : "");
    }
    return normal;
}

void __attribute__((noreturn)) call_start_cpu0(void)
{
    if (bootloader_init() != ESP_OK) bootloader_reset();
#ifdef CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP
    bootloader_utility_load_boot_image_from_deep_sleep();
#endif
    bootloader_state_t bs = {0};
    if (!bootloader_utility_load_partition_table(&bs)) {
        ESP_LOGE(TAG, "load partition table error!");
        bootloader_reset();
    }
    int boot_index = choose(&bs);
    if (boot_index == INVALID_INDEX) bootloader_reset();
    bootloader_utility_load_boot_image(&bs, boot_index);
}

#if CONFIG_LIBC_NEWLIB
struct _reent *__getreent(void)
{
    return _GLOBAL_REENT;
}
#endif
