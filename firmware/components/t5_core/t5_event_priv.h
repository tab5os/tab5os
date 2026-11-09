/* SPDX-License-Identifier: Apache-2.0
 * Between the portable event core (t5_event.c) and the loop under it:
 * t5_event_idf.c on the device, test/host/mocks/t5_event_mock.c on the host.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "t5_core/event.h"

/* A posted payload: its length and a zero-padded copy. Handlers get data
 * and len from here. */
typedef struct {
    uint32_t len;
    uint8_t data[T5_EVENT_MAX_DATA];
} t5_event_msg_t;

t5_err_t t5_event_table_init(void);
/* Hand msg (NULL: no payload) to the loop. Implemented by the loop. */
t5_err_t t5_event_loop_post(t5_event_id_t id, const t5_event_msg_t *msg);
/* Run every matching handler. Called by the loop for each posted event. */
void t5_event_deliver(t5_event_id_t id, const t5_event_msg_t *msg);
