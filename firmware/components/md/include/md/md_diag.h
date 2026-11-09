/* SPDX-License-Identifier: Apache-2.0
 * md's bench and diagnostics knobs: the rendering gate (plan 4.8) and the
 * snapshot compositing switches (`ui gate`, `ui freeze` on the console).
 * Not for apps or services: t5_diag and the simulator include this.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Off: animations redraw the live widgets every frame (for comparison). */
void md_freeze_set_enabled(bool on);
/* Which animations use snapshots: transitions and dialog backdrops by
 * default (bench-results.md, "Smoothness"). */
#define MD_FREEZE_TRANSITIONS 1u
#define MD_FREEZE_DIALOG_BACKDROP 2u
#define MD_FREEZE_DIALOG_BOX 4u
#define MD_FREEZE_ALL 7u
void md_freeze_set_mode(uint32_t mask);
uint32_t md_freeze_mode(void);   /* 0 while disabled */
typedef struct {
    uint32_t transitions, fallbacks, snapshots, failed;
    uint32_t snap_ms_last, snap_ms_max;
    uint32_t bake_ms_last;          /* md_freeze_draw_into */
    uint32_t dialog_prep_ms_last;   /* a dialog's snapshots, bake and compose */
    uint32_t box_snap_ms_last, compose_ms_last;
} md_freeze_stats_t;
void md_freeze_stats(md_freeze_stats_t *out);
void md_freeze_print(void);   /* the frozen screens, to the log */

/* The test screen from the design: app bar, 100-row list, card grid, FAB and
 * an open dialog. Returns the screen (not loaded). */
lv_obj_t *md_gate_create(void);
/* Scripted load for measurements, driven by a timer:
 *   MD_GATE_SCROLL      scroll the list up and down continuously
 *   MD_GATE_TRANSITION  slide between two full screens continuously
 *   MD_GATE_DIALOG      open and close the dialog continuously
 *   MD_GATE_IDLE        stop */
typedef enum { MD_GATE_IDLE, MD_GATE_SCROLL, MD_GATE_TRANSITION, MD_GATE_DIALOG } md_gate_mode_t;
void md_gate_run(lv_obj_t *gate, md_gate_mode_t mode);
const char *md_gate_mode_name(md_gate_mode_t m);
/* Frame rate while the gate animates (transition and dialog modes): frames
 * and milliseconds counted between consecutive frames with an animation
 * running, leaving out each step's first frame, whose delay (set-up such as
 * snapshots) is start_ms_max. The first call starts counting. */
typedef struct {
    uint32_t frames, ms, start_ms_max;
} md_gate_anim_stats_t;
void md_gate_anim_stats(md_gate_anim_stats_t *out, bool reset);

#ifdef __cplusplus
}
#endif
