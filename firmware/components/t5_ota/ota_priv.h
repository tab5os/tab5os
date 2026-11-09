/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>

/* The running image is in an A/B slot (not the recovery image). */
bool ota_running_from_slot(void);
