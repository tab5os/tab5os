/* SPDX-License-Identifier: Apache-2.0
 * Host event loop: synchronous dispatch through the real subscriber table.
 */
#include <string.h>
#include "t5_mocks.h"

void t5_event_dispatch(t5_event_id_t id, const void *data, size_t len);
t5_err_t t5_event_table_init(void);

static struct { t5_event_id_t id; } s_log[MOCK_EV_MAX];
static int s_n;

t5_err_t t5_event_init(void) { return t5_event_table_init(); }

t5_err_t t5_event_post(t5_event_id_t id, const void *data, size_t len)
{
    if (len > T5_EVENT_MAX_DATA) return T5_ERR_INVALID_ARG;
    if (s_n < MOCK_EV_MAX) s_log[s_n++].id = id;
    uint8_t buf[T5_EVENT_MAX_DATA] = {0};
    if (data && len) memcpy(buf, data, len);
    t5_event_dispatch(id, data ? buf : NULL, data ? sizeof buf : 0);
    return T5_OK;
}

int mock_events_count(t5_event_id_t id)
{
    int n = 0;
    for (int i = 0; i < s_n; i++) n += s_log[i].id == id;
    return n;
}

void mock_events_reset(void) { s_n = 0; }
