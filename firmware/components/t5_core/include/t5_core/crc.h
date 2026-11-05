/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CRC-32 (IEEE 802.3, reflected, as zlib). Pass 0 as crc to start. */
uint32_t t5_crc32(uint32_t crc, const void *data, size_t len);
/* CRC-8 (poly 0x07, init 0). For tiny records such as RTC RAM. */
uint8_t t5_crc8(const void *data, size_t len);

#ifdef __cplusplus
}
#endif
