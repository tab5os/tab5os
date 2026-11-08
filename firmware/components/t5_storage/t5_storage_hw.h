/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "t5_core/err.h"
#include "t5_storage/t5_storage.h"

t5_err_t t5_storage_hw_data_mount(void);          /* formats on failure */
t5_err_t t5_storage_hw_data_usage(uint64_t *total, uint64_t *used);
/* T5_OK mounted, T5_ERR_NOT_FOUND no card, T5_ERR_IO card present but mount failed */
t5_err_t t5_storage_hw_sd_mount(void);
bool t5_storage_hw_sd_alive(void);                 /* CMD13 status */
void t5_storage_hw_sd_unmount(void);
t5_err_t t5_storage_hw_sd_format(void);
t5_err_t t5_storage_hw_sd_raw_open(void **card);
void t5_storage_hw_sd_raw_close(void *card);
t5_err_t t5_storage_hw_sd_info(t5_sd_info_t *out);
