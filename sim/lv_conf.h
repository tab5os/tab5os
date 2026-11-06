/* SPDX-License-Identifier: Apache-2.0
 * LVGL configuration for the simulator. Mirrors the firmware's LVGL Kconfig
 * (firmware/sdkconfig.defaults, "Phase 4") wherever it affects how things
 * look; adds the SDL driver. */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_USE_OS LV_OS_NONE
#define LV_DEF_REFR_PERIOD 16
#define LV_DPI_DEF 320
#define LV_DRAW_BUF_ALIGN 64
#define LV_USE_DRAW_SW 1
#define LV_DRAW_SW_CIRCLE_CACHE_SIZE 16
#define LV_DRAW_SW_SHADOW_CACHE_SIZE 0
#define LV_USE_MATRIX 1
#define LV_USE_FLOAT 1
#define LV_USE_SNAPSHOT 1
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_DEFAULT &lv_font_montserrat_28
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_THEME_DEFAULT_GROW 0
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_SDL 1
#define LV_SDL_RENDER_MODE LV_DISPLAY_RENDER_MODE_DIRECT
#define LV_SDL_BUF_COUNT 1
#define LV_SDL_FULLSCREEN 0
#define LV_SDL_DIRECT_EXIT 1
#define LV_USE_THORVG 0
#define LV_USE_VECTOR_GRAPHIC 0

#endif
