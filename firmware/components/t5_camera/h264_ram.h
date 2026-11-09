/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>

/* Offer block (len bytes, internal RAM) to the encoder's next allocation. */
void h264_ram_give(void *block, size_t len);
/* Take it back once the encoder is deleted: NULL if there was none, or if
 * the encoder still holds it (then its free releases it). */
void *h264_ram_take(void);
