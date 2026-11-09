/* SPDX-License-Identifier: Apache-2.0
 * Byte layout of an ESP-IDF app image's start (esp_image_header_t, the
 * first segment header, esp_app_desc_t), for code that reads images
 * without IDF's headers: the C6 radio image parser, host tests and
 * tools/ota/t5ota.py (through tools/t5/t5const.py). t5_ota checks these
 * against IDF's structs at build time.
 */
#pragma once

#define T5_IMG_MAGIC 0xE9                 /* ESP_IMAGE_HEADER_MAGIC */
#define T5_IMG_HDR_LEN 24                 /* sizeof(esp_image_header_t) */
#define T5_IMG_SEG_HDR_LEN 8              /* sizeof(esp_image_segment_header_t) */
#define T5_IMG_SEGMENTS_OFF 1             /* uint8_t segment_count */
#define T5_IMG_CHIP_ID_OFF 12             /* uint16_t chip_id */
#define T5_IMG_MIN_REV_OFF 15             /* uint16_t min_chip_rev_full (major * 100 + minor) */
#define T5_IMG_MAX_REV_OFF 17             /* uint16_t max_chip_rev_full */
#define T5_IMG_HASH_APPENDED_OFF 23       /* uint8_t: 1 = SHA-256 after the checksum */
/* The app description opens the first segment. */
#define T5_IMG_DESC_OFF (T5_IMG_HDR_LEN + T5_IMG_SEG_HDR_LEN)
#define T5_IMG_DESC_MAGIC 0xABCD5432u     /* ESP_APP_DESC_MAGIC_WORD */
#define T5_IMG_DESC_VERSION_OFF 16        /* char version[32] */
#define T5_IMG_DESC_VERSION_LEN 32
#define T5_IMG_DESC_PROJECT_OFF 48        /* char project_name[32] */
#define T5_IMG_DESC_PROJECT_LEN 32

#define T5_IMG_CHIP_ESP32P4 18            /* ESP_CHIP_ID_ESP32P4 */
#define T5_IMG_CHIP_ESP32C6 13            /* ESP_CHIP_ID_ESP32C6 */
