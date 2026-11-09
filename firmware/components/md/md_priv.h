/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "md/md.h"
#include "md/md_diag.h"

/* md_freeze.c: a dialog's set-up times, for md_freeze_stats. */
void md_freeze_note_dialog(uint32_t prep_ms, uint32_t box_snap_ms, uint32_t compose_ms);
/* A scrim over a frozen backdrop. Top-layer objects created before the
 * scrim aren't in the backdrop (the screen alone) and keep redrawing live.
 * bake: into the snapshot (md_freeze_draw_into), everywhere but over those
 * objects; !bake: live, over just those objects (from the scrim's
 * DRAW_MAIN_END). Together they match a live scrim. */
void md_scrim_draw(lv_layer_t *layer, lv_obj_t *scrim, lv_opa_t opa, bool bake);

/* Waiting for md_freeze_settled() after a full-screen change, polled every
 * 4 ms for at most 30 polls (~120 ms), then done(arg). Stop cancels it. */
typedef struct {
    lv_timer_t *timer;
    uint32_t polls;
    void (*done)(void *arg);
    void *arg;
} md_settle_t;
void md_settle_start(md_settle_t *s, void (*done)(void *arg), void *arg);
void md_settle_stop(md_settle_t *s);

#define MD_ELEV_LEVELS 10
extern const int md_elev_levels[MD_ELEV_LEVELS];
/* Background style for a surface at dp (dark overlay), updated in place. */
lv_style_t *md_style_elevation(int dp);
/* Theme rebuilt: shadows re-pick light/dark behaviour. */
void md_shadow_scheme_changed(void);
/* Attach (or update) a baked shadow drawn under obj. dp 0 removes it. */
void md_shadow_attach(lv_obj_t *obj, int dp);
/* obj's baked shadow at elevation dp, drawn on `layer` (e.g. into a frozen
 * snapshot); the elevation obj's shadow is attached at (0 = none). */
void md_shadow_draw(lv_layer_t *layer, lv_obj_t *obj, int dp);
int md_shadow_dp(lv_obj_t *obj);
/* Pressed-state style that turns off the parent theme's colour filter. */
lv_style_t *md_style_no_press(void);
/* Variants applied on top of the contained-button theme style. */
lv_style_t *md_style_btn_text(void);
lv_style_t *md_style_btn_outlined(void);
