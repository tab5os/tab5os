/* SPDX-License-Identifier: Apache-2.0
 * Tab5 OS SDK: LVGL 9.6 (pinned for API level 1), the MD2 widgets and
 * T5_DP(). Call LVGL only from app callbacks or between t5_ui_lock() and
 * t5_ui_unlock(). Sizes in dp: T5_DP(n) is 2 px per dp.
 *
 * Custom widgets: lv_obj_class_t is complete here (its layout is part of the
 * ABI), so an app can define a class and create objects of it with
 * lv_obj_class_create_obj() / lv_obj_class_init_obj(). Leave instance_size
 * at 0 (the base class's size) and keep the widget's own data elsewhere:
 * lv_obj_t's layout is not part of the ABI. See docs/wiki/UI-Guide.md. */
#pragma once
#include "lvgl.h"
#include "src/core/lv_obj_class_private.h"
#include "md/md.h"
#include "md/md_icons.h"
#include "md/md_tokens.h"
#include "t5_ui/rotation.h"
#include "t5_ui/t5_ui.h"
