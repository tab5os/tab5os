/* SPDX-License-Identifier: Apache-2.0
 * Storage service.
 *   /data  LittleFS on the "data" partition: power-safe settings, system apps.
 *   /sd    microSD, 4-bit SDIO, FAT. No card-detect line exists on the Tab5,
 *          so a service task polls: CMD13 status while mounted, a mount
 *          attempt every T5_SD_RETRY_MS while absent.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_DATA_PATH "/data"
#define T5_SD_PATH "/sd"
#define T5_SD_POLL_MS 1000
#define T5_SD_RETRY_MS 3000
#define T5_SD_FAILED_RETRY_MS 15000   /* card present but won't mount */

typedef enum {
    T5_SD_ABSENT,
    T5_SD_MOUNTED,
    T5_SD_FAILED,      /* a card answers but its filesystem won't mount */
    T5_SD_EJECTED,     /* unmounted on request; stays so until t5_storage_sd_rescan() */
} t5_sd_state_t;

typedef struct {
    char name[8];
    uint32_t capacity_mb;
    uint32_t freq_khz;
    uint8_t bus_width;
    uint64_t total_bytes;
    uint64_t free_bytes;
} t5_sd_info_t;

/* Mount /data (formatting it if it won't mount) and start SD polling. */
t5_err_t t5_storage_init(void);
bool t5_storage_data_mounted(void);
t5_err_t t5_storage_data_usage(uint64_t *total, uint64_t *used);

t5_sd_state_t t5_storage_sd_state(void);
const char *t5_storage_sd_state_str(t5_sd_state_t s);
t5_err_t t5_storage_sd_info(t5_sd_info_t *out);
/* Run sync hooks, then unmount. Safe to pull the card afterwards. */
t5_err_t t5_storage_sd_eject(void);
/* Leave EJECTED/FAILED and try to mount on the next poll. */
t5_err_t t5_storage_sd_rescan(void);
/* USB mass storage: unmount /sd and lend the raw card (an sdmmc_card_t *)
 * to the USB stack; reclaim remounts it. */
t5_err_t t5_storage_sd_lend(void **card);
/* Erase the mounted SD card (a new FAT filesystem; slow on big cards: not
 * on the UI task). */
t5_err_t t5_storage_sd_format(void);
t5_err_t t5_storage_sd_reclaim(void *card);

/* Write a whole file so a reader sees the old or the new contents, never a
 * mix: write <path>.tmp, fsync, rename. Atomic on /data (LittleFS); on FAT
 * the old file is removed first, so a power cut can lose it. */
t5_err_t t5_storage_write_atomic(const char *path, const void *data, size_t len);

/* A small binary record with a header (magic, version, length, CRC-32),
 * written with t5_storage_write_atomic(). read returns T5_ERR_NOT_FOUND if
 * the file is missing and T5_ERR_IO if the header, length or CRC is wrong,
 * so a torn or foreign file is never mistaken for data. */
t5_err_t t5_storage_blob_write(const char *path, uint16_t version, const void *data, size_t len);
t5_err_t t5_storage_blob_read(const char *path, uint16_t version, void *buf, size_t cap, size_t *len);

/* Things that buffer writes (settings, logs) register a flush. Power calls
 * t5_storage_sync() before sleep and soft off. */
typedef t5_err_t (*t5_sync_hook_t)(void *arg);
t5_err_t t5_storage_register_sync(t5_sync_hook_t fn, void *arg);
t5_err_t t5_storage_sync(void);

/* One step of the SD poller (exposed for host tests). */
void t5_storage_sd_poll(uint32_t now_ms);

#ifdef __cplusplus
}
#endif
