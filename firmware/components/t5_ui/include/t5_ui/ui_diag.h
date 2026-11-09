/* SPDX-License-Identifier: Apache-2.0
 * The UI's bench and diagnostics knobs (`ui` console commands). Not for apps
 * or services: only t5_diag includes this.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "t5_ui/rotation.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The last frames, newest last: when each started (ms since boot), how long
 * LVGL rendered, how many pixels it redrew, and the first redrawn area.
 * For finding what a frame redraws (`ui frames`). Returns the count. */
typedef struct {
    uint32_t start_ms, render_us, px, nareas;
    uint32_t ppa_us, ppa_n;     /* blocked in PPA draws (translucent fills, alpha images) */
    uint32_t wait_us;           /* frame end: waiting for the last band copy */
    uint32_t flush_wait_us, flushes;   /* waiting for a free band; bands copied */
    uint32_t copy_exec_us, copies;     /* PPA time on this frame's async copies */
    int16_t x1, y1, x2, y2;
} t5_ui_frame_rec_t;
#define T5_UI_FRAME_LOG 32
int t5_ui_frame_log(t5_ui_frame_rec_t *out, int max);

/* Where LVGL's allocations land (lv_mem_t5.c): its internal pool, other
 * internal RAM, or PSRAM. Counts since the last reset; live and peak bytes
 * outside the pool. */
typedef struct {
    uint32_t pool_allocs, int_allocs, psram_allocs, failed;
    uint32_t big_allocs, big_max;      /* over the pool's request limit */
    uint32_t overflow_allocs;          /* small, but the pool was full */
    int32_t int_live, int_peak, psram_live, psram_peak;
    uint32_t pool_bytes, pool_free, pool_min_free;
} t5_ui_lvmem_t;
void t5_ui_lvmem(t5_ui_lvmem_t *out);
void t5_ui_lvmem_reset(void);
/* Integrity check of LVGL's pool (prints what it finds wrong). */
bool t5_ui_lvmem_check(void);

/* Bench A/B switches for LVGL's PPA unit: solid fills (off by default: see
 * ui.c), images, faded and ARGB8888 images, translucent fills (scrims). */
void t5_ui_set_ppa_fills(bool on);
bool t5_ui_ppa_fills(void);
void t5_ui_set_ppa_images(bool on);
void t5_ui_set_ppa_alpha(bool on);
void t5_ui_set_ppa_alpha_fills(bool on);

/* LVGL's clock n times slower: animations in slow motion (1 = normal). */
void t5_ui_set_slowmo(uint32_t n);

/* The auto-rotate filter's state: the last in-plane angle, whether the
 * board lies flat, and the rotation it is heading for. */
void t5_ui_orient_debug(int16_t *angle, bool *flat, t5_rotation_t *pending);

#ifdef __cplusplus
}
#endif
