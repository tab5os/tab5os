/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
typedef struct {
    char name[32];
    uint8_t id[32];          /* SHA-256 of the DER public key */
    uint8_t point[65];       /* 0x04 || X || Y */
} t5_ota_key_t;
extern const t5_ota_key_t t5_ota_keys[];
extern const int t5_ota_nkeys;
