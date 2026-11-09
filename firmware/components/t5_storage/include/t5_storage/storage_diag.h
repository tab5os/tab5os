/* SPDX-License-Identifier: Apache-2.0
 * SD card measurements for the console (t5_diag), not for apps.
 */
#pragma once
#include <stdint.h>

/* Raw sector read speed, bypassing FAT, in KB/s (negative: no card). */
int t5_storage_hw_sd_raw_bench(uint32_t sectors_per_io, uint32_t total_sectors);
/* FAT type (1 FAT12, 2 FAT16, 3 FAT32, 4 exFAT) and cluster size of the
 * mounted card. 0 on success. */
int t5_storage_hw_sd_fat_info(int *fs_type, int *cluster_bytes);
