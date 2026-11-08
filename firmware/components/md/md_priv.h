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
/* obj's baked shadow at elevation dp, drawn on `layer` (e.g. into a frozen
 * snapshot); the elevation obj's shadow is attached at (0 = none). */
void md_shadow_draw(lv_layer_t *layer, lv_obj_t *obj, int dp);
int md_shadow_dp(lv_obj_t *obj);
/* Pressed-state style that turns off the parent theme's colour filter. */
lv_style_t *md_style_no_press(void);
/* Variants applied on top of the contained-button theme style. */
lv_style_t *md_style_btn_text(void);
lv_style_t *md_style_btn_outlined(void);

/* RGB565 alpha blend, two channels at a time: spread a pixel to
 * 0b00000GGGGGG00000RRRRR000000BBBBB so 5-bit weights can't carry over. */
static inline uint32_t md_spread565(uint16_t c)
{
    return ((uint32_t)c | ((uint32_t)c << 16)) & 0x07E0F81Fu;
}

static inline uint16_t md_pack565(uint32_t x)
{
    x &= 0x07E0F81Fu;
    return (uint16_t)(x | (x >> 16));
}
