/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include "t5_core/err.h"
#include "t5_bootfmt/espimg.h"

#define T5_C6_CHIP_ID T5_IMG_CHIP_ESP32C6

typedef t5_err_t (*t5_c6img_read_t)(void *ctx, uint32_t off, void *buf, uint32_t len);

typedef struct {
    uint16_t chip_id;
    char version[32];
    uint32_t size;          /* whole image, including checksum and hash */
} t5_c6img_info_t;

/* T5_ERR_NOT_FOUND: no image (erased). T5_ERR_INVALID_ARG: corrupt. */
t5_err_t t5_c6img_parse(t5_c6img_read_t rd, void *ctx, uint32_t part_size, t5_c6img_info_t *out);
