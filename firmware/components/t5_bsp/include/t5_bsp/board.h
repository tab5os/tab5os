/* SPDX-License-Identifier: Apache-2.0
 * P4-side board rules that must hold from early boot.
 */
#pragma once
#include "t5_core/err.h"

/* G35 is shared with the PMS150G power controller. If its pad config changes
 * in sleep (IDF isolates pads by default), the controller can read a key
 * press and cut power. Keep its active config through sleep. (bench 0.8) */
t5_err_t t5_bsp_board_init(void);
