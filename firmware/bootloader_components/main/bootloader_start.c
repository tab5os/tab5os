/* SPDX-License-Identifier: Apache-2.0
 * Tab5 OS second-stage bootloader: IDF's, with the boot-attempt counter
 * (t5_bootfmt/bootctl.h) deciding between the OS and the recovery image.
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
#include "t5_bootfmt/bootctl.h"

static const char *TAG = "t5boot";

static bool recovery_valid(const bootloader_state_t *bs)
{
    if (bs->test.size == 0) return false;
    esp_image_metadata_t meta = {0};
    return esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &bs->test, &meta) == ESP_OK;
}

_Static_assert(T5_BOOTCTL_SECTOR_FROM_END == FLASH_SECTOR_SIZE, "the record is one flash sector");

/* Count this boot; returns the index to boot. */
static int choose(bootloader_state_t *bs)
{
    int normal = bootloader_utility_get_selected_boot_partition(bs);
    if (bs->test.size < 2 * T5_BOOTCTL_SECTOR_FROM_END) return normal;
    uint32_t addr = bs->test.offset + bs->test.size - T5_BOOTCTL_SECTOR_FROM_END;
    uint8_t rec[T5_BOOTCTL_REC_LEN];
    if (bootloader_flash_read(addr, rec, sizeof rec, false) != ESP_OK) return normal;

    t5_bootctl_boot_t b = t5_bootctl_boot(rec, esp_rom_get_reset_reason(0) == RESET_REASON_CORE_DEEP_SLEEP);
    if (b.mark && bootloader_flash_write(addr + b.word_off, &b.word, 4, false) != ESP_OK) {
        ESP_LOGE(TAG, "can't count this boot");
    }
    if (b.recovery) {
        if (recovery_valid(bs)) {
            if (b.requested) ESP_LOGW(TAG, "requested: starting recovery");
            else ESP_LOGW(TAG, "%d failed boots: starting recovery", b.failed);
            return TEST_APP_INDEX;
        }
        ESP_LOGE(TAG, "no recovery image: starting the OS anyway");
        bootloader_flash_erase_sector(addr / FLASH_SECTOR_SIZE);
    } else if (b.failed) {
        ESP_LOGW(TAG, "%d unconfirmed boot%s before this one", b.failed, b.failed > 1 ? "s" : "");
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
