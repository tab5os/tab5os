/* SPDX-License-Identifier: Apache-2.0
 * Deflated assets (tools/assets/zpack.py): the firmware build stores the
 * font bitmaps and shadow masks as zlib streams and inflates them into PSRAM
 * once, the first time md needs one. The simulator builds the plain files.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *z;          /* zlib stream */
    uint32_t zlen;
    uint32_t len;              /* inflated size */
    const void **target;       /* the descriptor field that gets the copy */
} md_zasset_t;

/* NULL-terminated, generated (md_zassets_table.c). */
extern const md_zasset_t *const md_zassets[];

/* Inflate every asset (idempotent, thread-safe enough: runs on first use,
 * which is the UI task or boot). Aborts if PSRAM can't hold them, since
 * nothing can draw text without them. No-op in the simulator. */
void md_assets_load(void);
