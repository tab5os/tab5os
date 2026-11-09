/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* True while t is the current task on either core. */
bool t5_task_on_a_core(TaskHandle_t t);
