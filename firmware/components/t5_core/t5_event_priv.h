/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include "t5_core/event.h"

t5_err_t t5_event_table_init(void);
void t5_event_dispatch(t5_event_id_t id, const void *data, size_t len);
void t5_event_reset_for_test(void);
