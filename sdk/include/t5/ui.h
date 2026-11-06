/* SPDX-License-Identifier: Apache-2.0
 * Tab5 OS SDK: LVGL 9.6 (pinned for API level 1), the MD2 widgets and
 * T5_DP(). Call LVGL only from app callbacks or between t5_ui_lock() and
 * t5_ui_unlock(). Sizes in dp: T5_DP(n) is 2 px per dp. */
#pragma once
#include "lvgl.h"
#include "md/md.h"
#include "md/md_icons.h"
#include "md/md_tokens.h"
#include "t5_ui/rotation.h"
#include "t5_ui/t5_ui.h"
