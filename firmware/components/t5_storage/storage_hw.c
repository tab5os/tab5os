/* SPDX-License-Identifier: Apache-2.0
 * /data on LittleFS, /sd on SDMMC slot 0 (IO MUX pins 39-44, 4-bit).
 * The SD I/O supply comes from on-chip LDO channel 4 (as in Espressif's BSP).
 */
#include <stdlib.h>
#include <string.h>
#include "driver/sdmmc_host.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "t5_storage/storage_diag.h"
#include "t5_storage_hw.h"

#define DATA_LABEL "data"
#define SD_LDO_CHAN 4

static sdmmc_card_t *s_card;
static sd_pwr_ctrl_handle_t s_sd_pwr;

t5_err_t t5_storage_hw_data_mount(void)
{
    const esp_vfs_littlefs_conf_t conf = {
        .base_path = T5_DATA_PATH,
        .partition_label = DATA_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t e = esp_vfs_littlefs_register(&conf);
    return e == ESP_OK ? T5_OK : (e == ESP_ERR_NOT_FOUND ? T5_ERR_NOT_FOUND : T5_ERR_IO);
}

t5_err_t t5_storage_hw_data_format(void)
{
    return esp_littlefs_format(DATA_LABEL) == ESP_OK ? T5_OK : T5_ERR_IO;
}

t5_err_t t5_storage_hw_data_usage(uint64_t *total, uint64_t *used)
{
    size_t t = 0, u = 0;
    if (esp_littlefs_info(DATA_LABEL, &t, &u) != ESP_OK) {
        return T5_ERR_IO;
    }
    if (total) *total = t;
    if (used) *used = u;
    return T5_OK;
}

/* Slot 0 only: the C6 owns slot 1 of the same controller. */
static t5_err_t sd_config(sdmmc_host_t *host, sdmmc_slot_config_t *slot)
{
    if (!s_sd_pwr) {
        sd_pwr_ctrl_ldo_config_t ldo = { .ldo_chan_id = SD_LDO_CHAN };
        if (sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_sd_pwr) != ESP_OK) {
            return T5_ERR_IO;
        }
    }
    *host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
    host->slot = SDMMC_HOST_SLOT_0;
    host->max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host->pwr_ctrl_handle = s_sd_pwr;
    *slot = (sdmmc_slot_config_t)SDMMC_SLOT_CONFIG_DEFAULT();
    slot->width = 4;
    return T5_OK;
}

t5_err_t t5_storage_hw_sd_mount(void)
{
    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    T5_RETURN_ON_ERR(sd_config(&host, &slot));
    const esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    /* An absent card is the normal case while polling: keep the drivers quiet. */
    esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_init", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_sd", ESP_LOG_NONE);
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
    esp_err_t e = esp_vfs_fat_sdmmc_mount(T5_SD_PATH, &host, &slot, &mount, &s_card);
    if (e == ESP_OK) {
        return T5_OK;
    }
    s_card = NULL;
    /* Card init failures (timeouts, no response) mean no card; a FAT error means a card we can't use. */
    return e == ESP_FAIL ? T5_ERR_IO : T5_ERR_NOT_FOUND;
}

bool t5_storage_hw_sd_alive(void)
{
    return s_card && sdmmc_get_status(s_card) == ESP_OK;
}

void t5_storage_hw_sd_unmount(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(T5_SD_PATH, s_card);
        s_card = NULL;
    }
}

t5_err_t t5_storage_hw_sd_info(t5_sd_info_t *out)
{
    if (!s_card) {
        return T5_ERR_STATE;
    }
    memset(out, 0, sizeof *out);
    memcpy(out->name, s_card->cid.name, sizeof s_card->cid.name < sizeof out->name - 1
                                          ? sizeof s_card->cid.name : sizeof out->name - 1);
    out->capacity_mb = (uint32_t)(((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) >> 20);
    out->freq_khz = s_card->real_freq_khz;
    out->bus_width = (uint8_t)(1u << s_card->log_bus_width);
    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(T5_SD_PATH, &total, &freeb) == ESP_OK) {
        out->total_bytes = total;
        out->free_bytes = freeb;
    }
    return T5_OK;
}

int t5_storage_hw_sd_raw_bench(uint32_t sectors_per_io, uint32_t total_sectors)
{
    if (!s_card) return -1;
    uint8_t *b = heap_caps_aligned_alloc(64, sectors_per_io * 512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!b) return -2;
    int64_t t0 = esp_timer_get_time();
    for (uint32_t s = 0; s < total_sectors; s += sectors_per_io) {
        if (sdmmc_read_sectors(s_card, b, 2048 + s, sectors_per_io) != ESP_OK) { heap_caps_free(b); return -3; }
    }
    int64_t dt = esp_timer_get_time() - t0;
    heap_caps_free(b);
    return (int)((int64_t)total_sectors * 512 * 1000 / (dt ? dt : 1));   /* bytes/ms ~= KB/s */
}

#include "ff.h"
int t5_storage_hw_sd_fat_info(int *fs_type, int *cluster_bytes)
{
    FATFS *fs = NULL;
    DWORD free_clusters = 0;
    if (!s_card || f_getfree("0:", &free_clusters, &fs) != FR_OK || !fs) return -1;
    *fs_type = fs->fs_type;
    *cluster_bytes = fs->csize * 512;
    return 0;
}

t5_err_t t5_storage_hw_sd_format(void)
{
    if (!s_card) return T5_ERR_STATE;
    return esp_vfs_fat_sdcard_format(T5_SD_PATH, s_card) == ESP_OK ? T5_OK : T5_ERR_IO;
}

/* Raw card for USB mass storage (t5_usb): no filesystem. */
t5_err_t t5_storage_hw_sd_raw_open(void **card_out)
{
    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    T5_RETURN_ON_ERR(sd_config(&host, &slot));
    esp_err_t e = sdmmc_host_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return T5_ERR_IO;
    if (sdmmc_host_init_slot(SDMMC_HOST_SLOT_0, &slot) != ESP_OK) return T5_ERR_IO;
    sdmmc_card_t *card = calloc(1, sizeof *card);
    if (!card) return T5_ERR_NO_MEM;
    if (sdmmc_card_init(&host, card) != ESP_OK) {
        free(card);
        host.deinit_p(SDMMC_HOST_SLOT_0);
        return T5_ERR_NOT_FOUND;
    }
    *card_out = card;
    return T5_OK;
}

void t5_storage_hw_sd_raw_close(void *card)
{
    sdmmc_card_t *c = card;
    if (!c) return;
    if (c->host.deinit_p) c->host.deinit_p(c->host.slot);
    free(c);
}
