/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "md/md.h"

#define MD_ELEV_LEVELS 10
extern const int md_elev_levels[MD_ELEV_LEVELS];
/* Background style for a surface at dp (dark overlay), updated in place. */
lv_style_t *md_style_elevation(int dp);
/* Theme rebuilt: shadows re-pick light/dark behaviour. */
void md_shadow_scheme_changed(void);
/* Attach (or update) a baked shadow drawn under obj. dp 0 removes it. */
void md_shadow_attach(lv_obj_t *obj, int dp);
/* Pressed-state style that turns off the parent theme's colour filter. */
lv_style_t *md_style_no_press(void);
/* Variants applied on top of the contained-button theme style. */
lv_style_t *md_style_btn_text(void);
lv_style_t *md_style_btn_outlined(void);
