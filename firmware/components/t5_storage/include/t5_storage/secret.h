/* SPDX-License-Identifier: Apache-2.0
 * Sealed secrets (plan 9.4, design "Credentials"): AES-256-GCM with a key
 * bound to this chip, for credentials kept in NVS or /data.
 *
 * The key is derived (SHA-256) from the chip's factory MAC and its 128-bit
 * unique ID in eFuse, so a flash dump read on another device or a PC
 * doesn't show the passwords. It is not a secret from someone holding this
 * Tab5, who can read both IDs over USB: secure boot and flash encryption
 * would be needed for that, and they burn eFuses for good (docs/security.md).
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

#define T5_SECRET_OVERHEAD (4 + 12 + 16)   /* magic, nonce, tag */

/* out must hold len + T5_SECRET_OVERHEAD. */
t5_err_t t5_secret_seal(const void *in, size_t len, void *out, size_t outcap, size_t *outlen);
/* T5_ERR_INVALID_ARG if it isn't sealed (or was sealed on another chip). */
t5_err_t t5_secret_open(const void *in, size_t len, void *out, size_t outcap, size_t *outlen);
/* True if data starts like a sealed blob (for migrating plaintext). */
int t5_secret_is_sealed(const void *in, size_t len);
