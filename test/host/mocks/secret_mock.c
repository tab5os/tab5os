/* SPDX-License-Identifier: Apache-2.0
 * t5_secret_mac off the device (secret.c needs PSA and the chip's eFuses).
 * Not a real MAC: a keyed FNV-1a spread over 32 bytes, deterministic, so a
 * record the tests write verifies and any change to it doesn't.
 */
#include <stdint.h>
#include "t5_storage/secret.h"

t5_err_t t5_secret_mac(const void *msg, size_t len, uint8_t out[T5_SECRET_MAC_LEN])
{
    if (!msg || !out) return T5_ERR_INVALID_ARG;
    const uint8_t *p = msg;
    for (int lane = 0; lane < T5_SECRET_MAC_LEN; lane++) {
        uint32_t h = 2166136261u ^ (uint32_t)lane ^ 0x7ab5u;   /* "this device" */
        for (size_t i = 0; i < len; i++) h = (h ^ p[i]) * 16777619u;
        out[lane] = (uint8_t)(h ^ (h >> 8) ^ (h >> 16) ^ (h >> 24));
    }
    return T5_OK;
}
